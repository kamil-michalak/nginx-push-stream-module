# nginx-push-stream-module (fork `test22`) — analiza wydajności pod ~100k połączeń/instancję

Analiza różnicy `origin/master..HEAD` (33 commity, ~3150 linii C) + realnej konfiguracji
z `docker-setup` (`containers/nginx/config/snippets/location_api_sockets.conf`,
`push_http.conf`, `nginx_server_frontend.conf`). Branch `test22` jest tym, co buduje
`containers/nginx/build/push-stream-module/source`.

Data analizy: 2026-09-08.

---

## Kontekst konfiguracji (istotny dla oceny)

- `push_stream_shared_memory_size 24576M`, `max_messages_stored_per_channel 512`,
  `message_ttl 10m`, `channel_inactivity_time 20m`,
  `channel_inactivity_delete_with_subscribers on`
- WS (`/api/live/ws`): `push_stream_ping_mode application`,
  `ping_message_interval 21s`, `unknown_event_id_behavior disconnect`,
  `websocket_allow_resubscribe on`, `websocket_max_channels_per_connection 10`,
  `subscriber_connection_ttl 4h`, szablon `~text~`
- fan-out przez kanały wildcard `wch_` per kraj/język
- frontend host-networked, `worker_processes 32`, `worker_connections 16384`,
  limit ~40k WS przez `nf_conntrack` (osobny temat: `docker-setup/docs/notes/firewall.md`)
- przeglądarki **zawsze** oferują `permessage-deflate` -> `ctx->deflate_enabled=1`
  dla praktycznie każdego klienta WS; kompresja negocjowana z
  `server_no_context_takeover; client_no_context_takeover`

---

## Ocena ogólna

Większość zmian to **poprawki poprawnościowe krytyczne przy tej skali**, nie regresje:

- wycieki przy `nginx -s reload` (fd IPC, `ping_msg` w shm, padding UAF,
  `ngx_stat_writing` zaklinowany, nieskończona pętla w cleanup workera przy
  shutdownie) — przy 100k połączeń każdy reload bez tych poprawek =
  wyciek / hang / outage
- chunking (4 KB) + bucketing rozmiarów shm — bezpośrednia odpowiedź na
  `ngx_slab_alloc() failed: no memory` z fragmentacji (dlatego shm podbite do 24G)
- IPC alerty wyniesione poza `channel->mutex` w `broadcast()`, refaktor census
  (mniej pracy pod globalnym lockiem) — realne plusy skalowalności
- dedupe `raw_content` przez refcount przy publikacji fan-out — oszczędza N kopii
  tego samego body w shm

Nowe ryzyka wydajnościowe są skupione w kilku miejscach poniżej.

---

## Problemy wydajnościowe (wg wagi)

### 🔴 1. Burza reconnectów (deploy / reload / failover LB) — główne ryzyko przy 100k

Na świeżym połączeniu **bez `Last-Event-Id`** ścieżka `subscriber_assign_channel`
robi teraz sekwencyjnie:

1. `has_old_messages_to_send()` — `ngx_shmtx_lock(channel->mutex)`, skan do 512 wiadomości
2. `send_startup_message()` — `ngx_shmtx_lock(channel->mutex)`, **wysyłka całego
   snapshotu do bufora połączenia (`ngx_http_output_filter` -> `writev`) pod
   trzymanym lockiem**
3. `send_old_messages()` catch-up — `ngx_shmtx_lock(channel->mutex)` znowu, kolejny
   skan 512 wiadomości + wysyłka

`channel->mutex` jest per-kanał (dobrze — burza rozkłada się na ~200 kanałów
krajowych), ale popularne kraje = gorące kanały. Przy 100k reconnectów skupionych
na kilkunastu kanałach ten mutex staje się punktem serializacji **między
workerami**, a sekcja krytyczna zawiera alokację bufora + syscall zapisu snapshotu.
Wzorzec "thundering herd" -> stalle workerów, skoki latencji, kaskadowe timeouty.

Wzorzec **częściowo prexistujący** (`send_old_messages` z upstreamu też trzyma
lock podczas `send_response_message`), ale fork:

- rozszerza go na **domyślny** connect bez historii (wcześniej płacili tylko
  klienci z `Last-Event-Id`)
- dokłada 2. i 3. cykl lock/skan

**Spotęgowanie przez `unknown_event_id_behavior disconnect`:** po przestoju
dłuższym niż `message_ttl` (10m) **każdy** reconnectujący klient ma nieważny
`$arg_event` -> `OLD_MESSAGES_NOT_FOUND` -> JSON + rozłączenie -> klient łączy się
ponownie (teraz bez event-id) -> dostaje startup + pełny catch-up. Podwójna fala +
pełny skan bufora pod lockiem *zanim* subskrybent zostanie zarejestrowany.

**Mitygacje:**

- snapshot startowy **mały** (pojedyncze KB — dok `nginx-push-stream-startup-message.md`
  już to zaleca; `40 000 × 200 KB ≈ 8 GB` przejściowo)
- klient **zawsze** wysyła `Last-Event-Id`/`$arg_event` przy reconnekcie —
  całkowicie omija ścieżkę startup i (przy świeżym id) omija disconnect
- jitter po stronie klienta przy reconnekcie (rozłożyć w czasie)
- rozważyć `message_ttl` > najgorszy realny czas przestoju, żeby event-id nie
  wygasały masowo
- rozważyć wysyłkę snapshotu **poza** `channel->mutex` (skopiować wskaźnik msg pod
  lockiem, `workers_ref_count++`, wysłać po zwolnieniu) — obecnie snapshot leci
  pod lockiem

### 🟠 2. UAF przy wolnym kliencie a wymiana `startup_message` / `channel_deleted_message`

`send_response_text` / `send_content` to **zero-copy** (`b->memory=1`, wskaźnik
prosto w shm). Bufor zostaje w łańcuchu `busy` połączenia dopóki klient nie odbierze.

Dla wiadomości z kolejki działa ochrona `workers_ref_count` + `message_ttl`
(10m >> okno flushowania). Ale:

- `send_startup_message()` **nie** podbija `workers_ref_count`
- `set_channel_startup_message()` po podmianie wskaźnika woła
  `free_message_memory(old)` (tylko pod `shpool->mutex`, bez sprawdzenia
  jakiegokolwiek refcountu połączeń)

Jeśli wolny klient nie zflushował poprzedniego snapshotu, a publisher w tym czasie
nadpisał `Startup-Message` -> bufor w `busy` wskazuje na zwolniony chunk shm ->
**use-after-free / wysłanie śmieci**. Okno wąskie (snapshoty wymieniane w skali
sekund, flush w ms), ale realne przy 100k połączeń z "ogonem" zestallowanych +
częstej aktualizacji snapshotu. Ten sam mechanizm dotyczy
`channel_deleted_message` (fork czyni kanały usuwalne w locie przez
`channel_inactivity_delete_with_subscribers on`).

**Mitygacja:** albo kopiować snapshot do puli requestu przy wysyłce, albo objąć go
`workers_ref_count` i odraczać `free` jak dla wiadomości z kolejki, albo trzymać
poprzedni snapshot w "trash" z TTL zamiast natychmiastowego `free`.

### 🟠 3. CPU publikacji — `deflateInit2`/`deflateEnd` per wiadomość per kanał

`ngx_http_push_stream_deflate_websocket_frame()` woła `deflateInit2()` +
`deflateEnd()` **na każdą wiadomość** w `convert_char_to_msg_on_shared`. Każdy
`deflateInit2` alokuje i zwalnia ~256–260 KB workspace.

Przy publikacji fan-out do kanałów wildcard (gol -> wszystkie kanały krajowe ×
języki) jedna publikacja HTTP = N wiadomości = **N kompresji + N formatowań
szablonów**, synchronicznie w requeście publishera, każda pod `channel->mutex`
danego kanału. Duże fan-outy stają się wolniejsze i dłużej trzymają locki.

**Zaleta jest realna** (kompresja raz, te same bajty do wszystkich 100k
subskrybentów dzięki `server_no_context_takeover` — bez kompresji per-połączenie i
bez okien deflate 100k×256KB), więc feature zostawić, ale:

- **reużywać jeden `z_stream` per worker z `deflateReset()`** zamiast init/end per
  wiadomość — usuwa cały churn alokatora
- kompresowany wariant jest budowany **zawsze** gdy szablon jest `websocket`,
  niezależnie czy ktokolwiek połączony negocjował deflate i czy kanał ma
  subskrybentów — przy pustych/rzadkich kanałach to zmarnowany CPU + kopia w shm

### 🟡 4. Pojemność shm i koszt bucket-roundingu

Na wiadomość fork trzyma teraz: `raw_content` (refcounted) + kopia sformatowana per
szablon + **kopia skompresowana per szablon WS** + rozmiary **zaokrąglone w górę
do kubełka** (`64,128,...,131072` — średnio ~1.3–1.5× narzutu na zmiennej długości
JSON) + reprezentacja chunkowana 4 KB dla >16 KB.

Efekt: mniej "no memory" z fragmentacji (przewidywalne), ale **wyższy bezwzględny
footprint**. Dedupe `raw` przez fan-out odzyskuje część. Przy 512 wiadomości/kanał
× tysiące kanałów w 24G trzeba monitorować nowe pola `shm_free_mb` /
`shm_free_pages` w `/channels-stats` (summarized).

### 🟡 5. `ngx_http_push_stream_shm_free_pages()` pod `shpool->mutex` na endpoincie statystyk

Walk po liście wolnych *runów* (nie pojedynczych stron — więc O(fragmentacja), nie
O(6M stron)) pod **globalnym** `shpool->mutex`. Wołane przy każdym `ngx_slab_alloc`
failure (OK, rzadkie) **oraz na każdym żądaniu `/channels-stats` summarized**. Przy
silnej fragmentacji to tysiące iteracji zamrażających całą alokację shm we
wszystkich workerach.

**Mitygacja:** nie scrapować tego endpointu z wysoką częstotliwością (netdata —
sprawdzić interwał, `docker-setup/docs/notes/monitoring.md`).

### 🟢 6. `ping_mode application` przy 100k idle WS — mniejszy problem niż się wydaje

100k / 21s ≈ 4.8k wiadomości/s ÷ 32 workery ≈ **~150/s/worker**, każda to
`get_formatted_message` (O(1), prekompilowane) + jeden `writev`. Steady-state
trywialne. Fork dodał optymalizację "pomiń ping jeśli realna wiadomość poszła w
interwale" + "resetuj timer tylko jeśli uzbrojony" — netto **plus**. NATIVE
(2-bajtowa ramka PING) byłby tańszy i nie budzi JS, ale `application` wybrany
świadomie (heartbeat widoczny w `onmessage`). Zostawić, chyba że latencja pod
burzą (#1) każe ciąć każdy grosz.

### 🟢 7. Podwójne skany `has_old_messages_to_send`

Ścieżka assign woła `has_old_messages_to_send()` jawnie, potem `send_old_messages()`
woła je **ponownie** wewnętrznie — 2 blokady + skany kolejki na połączenie z
historią (wzorzec z upstreamu, nie regresja, ale przy 100k reconnektach mnoży
kontencję — patrz #1).

---

## Rzeczy do zweryfikowania testem obciążeniowym przed produkcją 100k

1. **Burza reconnectów**: 100k WS, restart instancji API/frontendu, zmierzyć czas
   do ustabilizowania, p99 latencji connectu, CPU per worker, czy `channel->mutex`
   gorących kanałów nie serializuje. Osobno scenariusz "przestój > 10m -> wszystkie
   event-id nieważne".
2. **UAF #2**: pętla publikująca `Startup-Message: 1` co ~200 ms na gorący kanał +
   kilkuset celowo wolnych klientów (throttle odbioru), pod ASAN/valgrind lub z
   małym `push_stream_shared_memory_size` (szybsza rotacja).
3. **CPU publikacji #3**: profil `perf` publishera przy realnym fan-out gola (ile
   kanałów, ile `deflateInit2`/s).
4. **shm**: obserwować `shm_free_mb` przez 24–48h przy produkcyjnym ruchu — czy
   trend opada (wyciek) czy stabilny.
5. **RSS workerów #8**: 100k WS z klienckim `?`-ping keepalive przez kilka godzin —
   czy RSS per worker rośnie liniowo (narost `r->pool`).
6. **Inflate #9**: klient wysyła skompresowaną ramkę WS ~5–10 KB (kompresowalną
   >4×), sprawdzić czy dochodzi w całości czy połączenie jest zrywane.

---

## Uzupełnienie — druga analiza (2026-09-08, przegląd `websocket.c` / `utils.c` liniowo)

Dodatkowe ustalenia, których nie ma w rankingu 1–7 powyżej:

### 🔴 8. `r->pool` rośnie bez ograniczenia na długożyjących połączeniach WS

Nowe ścieżki komend WS alokują z `r->pool`, który żyje przez **całe** połączenie
(przy `subscriber_connection_ttl 4h` — do 4 godzin):

- `ngx_http_push_stream_websocket_handle_ping()` — `json` + `frame` z `r->pool`
  przy **każdym** `?`-pingu (`websocket.c` ~74, ~92)
- `ngx_http_push_stream_websocket_send_ack()` — przy każdym `+` / `-` / błędzie
  (`websocket.c` ~17, ~23)
- `handle_subscribe()` — JSON `unknown_event_id` + frame z `r->pool` (~184)
- `send_startup_message()` — `boundary_event_id` kopiowany do `r->pool` przy każdym
  resubscribe bez event-id (`subscriber.c` ~486)

Ścieżka **publikacji** przez WS poprawnie używa `ctx->temp_pool` (niszczony
per-frame w `next_frame:`), ale ścieżki komend — nie. Aplikacja używająca `?`-ping
jako keepalive albo dynamicznego `+`/`-` puchnie liniowo: 100k połączeń × kilka
alokacji/min × godziny = wielogigabajtowy narost RSS workerów **niewidoczny w
`shm`** (to pamięć procesu, nie strefa współdzielona). Upstreamowy WS tego nie miał
(czytał tylko ramki + publish na temp_pool).

**Fix:** wszystkie alokacje komend przenieść na krótkotrwały pool (utworzyć per
komenda i zniszczyć po wysłaniu ramki), jak zrobiono dla publisha.

### 🟠 9. Dekompresja permessage-deflate ucina większe ramki → rozłączenie klienta  ✅ NAPRAWIONE (branch `test23`)

> **Fix zaaplikowany 2026-09-08 (branch `test23`):**
> - `websocket.c` — pętla `inflate` z podwajaniem bufora wyjściowego aż
>   wiadomość jest w całości rozpakowana; „bufor pełny a nie `Z_STREAM_END`"
>   wyzwala grow-retry, nie jest już traktowane jako sukces.
> - **Nowa dyrektywa `push_stream_websocket_max_message_length`** (rozmiar,
>   domyślnie **64 KB**) — jeden limit egzekwowany w 3 miejscach:
>   (1) długość zadeklarowana w nagłówku ramki (odrzucenie **zanim** payload
>   zostanie odczytany/zaalokowany — łata też brak jakiegokolwiek limitu na
>   `payload_len` z 64-bit nagłówka, nie tylko ścieżkę deflate);
>   (2) rozmiar po re-asemblacji ramek fragmentowanych (inaczej klient tnie
>   duży komunikat na wiele małych i rośnie `temp_pool` bez końca);
>   (3) rozmiar **zdekompresowany** ramki permessage-deflate.
>   `0` = wyłącza (1) i (2); (3) i tak ma nie-wyłączalny backstop 32 MB
>   (`NGX_HTTP_PUSH_STREAM_WEBSOCKET_INFLATE_HARD_MAX`).
> - Przekroczenie któregokolwiek limitu → czyste zamknięcie połączenia zamiast
>   cichego ucięcia / nieograniczonej alokacji.
> - Kompiluje się czysto (nginx 1.26.2, `NGX_HAVE_ZLIB`, `-Wall`).
>
> **Uwaga migracyjna:** domyślne 64 KB to zmiana zachowania — instalacja
> wysyłająca przez WS komunikaty >64 KB (duży `websocket_allow_publish`) musi
> podnieść dyrektywę. Ruch komend (`?`/`+`/`-`) i typowe publikacje JSON
> mieszczą się z zapasem.
>
> Opis błędu poniżej dla kontekstu.

`websocket.c` ~426, ścieżka inflate klient→serwer:

```c
inflated_size = ctx->frame->payload_len * 4 + 256;   // jednorazowo; komentarz "expand once if needed" kłamie
zret = inflate(&zs, Z_SYNC_FLUSH);
if (zret != Z_OK && zret != Z_STREAM_END && zret != Z_BUF_ERROR) { ...błąd... }
```

Bufor wyjściowy alokowany raz jako 4× wejścia i **nigdy nie rozszerzany**. Deflate
na powtarzalnym JSON łatwo osiąga 10–20×, więc większa skompresowana ramka →
`inflate` zwraca `Z_OK` z `avail_out == 0`, payload **po cichu ucięty**, brak
detekcji (kod nie sprawdza zapełnienia bufora), potem test UTF-8 zwykle nie
przechodzi → `goto finalize` → **zerwane połączenie**. Przy klientach publikujących
przez WS = systematyczne rozłączenia → sprzężenie z burzą reconnectów (#1).
`Z_BUF_ERROR` traktowany jako sukces dodatkowo maskuje niekompletne wejście.

Przeglądarki oferują `permessage-deflate` zawsze, ale kompresują ramkę wychodzącą
tylko gdy się opłaca — problem uderza dokładnie w klientów wysyłających większe
komunikaty (batch subscribe, duży payload aplikacyjny).

**Fix:** pętla `inflate` z realokacją przy `avail_out == 0` + twardy limit; „bufor
pełny, a nie `Z_STREAM_END`” traktować jako błąd, nie sukces.

### 🟠 10. `deflate` bezwarunkowy i bez przełącznika konfiguracyjnego

Rozszerzenie #3: `deflate_websocket_frame()` jest wołane przy **każdej** publikacji
gdy istnieje szablon `websocket` i payload ≥ 200 B — niezależnie od tego, czy
jakikolwiek subskrybent wynegocjował deflate. Skompresowana kopia siedzi w shm
przez cały TTL wiadomości → **~2× pamięci na przechowywane wiadomości** przy
zerowym zysku, jeśli klienci deflate nie występują. Brak dyrektywy typu
`push_stream_websocket_permessage_deflate on|off`. Dla f24 klienci to przeglądarki
(deflate zawsze) więc zysk jest realny — ale brak kill-switcha na wypadek
regresji CPU/pamięci pod produkcyjnym fan-outem.

### 🟡 11. `log_slab_alloc_failure()` woła `shm_free_pages()` na KAŻDYM nieudanym alloc

Doprecyzowanie #5: w warunkach fragmentacji (czyli tych, dla których feature
powstał) alokacje sypią się **seriami**, a każdy failure przechodzi całą free-listę
pod globalnym `shpool->mutex` — lock convoy dokładnie w momencie, gdy strefa jest
pod presją. „OK, rzadkie” z #5 nie zachodzi w najgorszym scenariuszu.

**Fix:** rate-limit / sampling logu alloc-failure (np. 1/s), nie per wywołanie.

### 🟡 12. Chunkowany wysył (>16 KB) = N× `ngx_buf` per subskrybent

`send_content()` buduje łańcuch jednego `ngx_buf` na 4 KB chunk. Wiadomość 64 KB =
16 bufów/subskrybenta zamiast 1 przy fan-oucie. Bufy recyklują się przez
`ctx->free`, ale wolny klient trzyma 16 bufów wskazujących w shm (zwiększa okno
UAF z #2). Dla rzadkich dużych wiadomości akceptowalne; przy `~text~` i małych
komunikatach nie występuje.

### 🟢 13. `message_separator` podwaja wywołania output-filter dla streamingu

Dodatkowy `send_response_text()` (alokacja bufa + `ngx_http_output_filter`) przed
każdą wiadomością poza pierwszą, per subskrybent. Nie dotyczy WS (osobne ramki)
ani EventSource. Dla f24 (`/api/live/ws`) bez znaczenia; istotne gdyby włączono
separator na trybie streaming z dużym fan-outem.

### 🟢 14. Bare `?` (payload_len == 1) nie jest obsłużony

Warunek `payload_len > 1` w `websocket.c` ~775 — klient wysyłający samo `?` nie
dostaje `pong`; ramka wpada w ścieżkę publish albo jest ignorowana. Trzeba wysłać
`?<payload>`. Drobiazg, ale może generować timeouty po stronie klienta jeśli
biblioteka kliencka wysyła goły `?`.

---

## Zmiany będące plusami przy tej skali (dla równowagi)

- `broadcast()`: IPC alerty (`ngx_write_channel`) wyniesione poza `channel->mutex`
- refaktor census subskrybentów (3 fazy, mniej pracy pod globalnym lockiem,
  brak wewnętrznej pętli per-kanał w common case)
- `get_header()` bez kopii (wskaźnik w już sparsowany nagłówek nginx)
- ping timer resetowany tylko gdy `timer_set` (mniej churnu w rbtree timerów)
- fix nieskończonej pętli w `cleanup_shutting_down_worker_data` (worker 100% CPU
  przy reload)
- fix zaklinowanego `ngx_stat_writing` / connection stuck in writing po reload
- `send_event`: skip gdy nikt nie subskrybuje events channel
- `delete_channel`: slab alloc `channel_deleted_message` wyniesiony poza
  `channels_queue_mutex` (kluczowe przy
  `channel_inactivity_delete_with_subscribers on`)
- batch usuwania nieaktywnych kanałów z subskrybentami ograniczony do 3/cykl
  (ochrona przed burzą blokujących `ngx_write_channel`)
