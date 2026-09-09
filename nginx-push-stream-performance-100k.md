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

### 🔴 1. Burza reconnectów (deploy / reload / failover LB) — główne ryzyko przy 100k  ✅ NAPRAWIONE (branch `test23`)

> **Fix zaaplikowany 2026-09-09 (dwie części):**
>
> **Część 1 — `send_startup_message`** (krok 2). Pod `channel->mutex`: odczyt
> wskaźnika `channel->startup_message`, `msg->workers_ref_count++`, kopia
> event-id do `r->pool`, unlock. Wysyłka (`send_response_message` → syscall
> zapisu) leci **poza** lockiem. Bezpieczne dzięki refcountowi — to ten sam
> mechanizm co fix #2, nie dwa osobne.
>
> **Część 2 — `send_old_messages`** (krok 3, catch-up dla klientów z
> `Last-Event-Id`/backtrack). Wcześniej pętla wysyłająca N pasujących
> wiadomości (do `max_messages_stored_per_channel`, setki) leciała **w
> całości pod `channel->mutex`** — jeden `send_response_message` na
> wiadomość, wszystkie pod tym samym per-kanałowym lockiem co publish.
> Teraz: pod lockiem tylko **skan + zebranie wskaźników** do tablicy
> (`ngx_pcalloc(r->pool, ...)`, rozmiar = `channel->stored_messages`) +
> `workers_ref_count++` na każdej zebranej wiadomości; unlock; wysyłka
> całej paczki **poza lockiem**; dekrement refcountów pod `shpool->mutex`
> po wysyłce. Bezpieczne z tego samego powodu co #2 — lista
> `channel->message_queue` może się zmienić pod nami w dowolnym momencie po
> odblokowaniu (eviction przy przepełnieniu/wygaśnięciu, nowa publikacja),
> więc nie da się trzymać iteratora przez unlock — trzeba zebrać wskaźniki
> z góry i zabezpieczyć je refcountem, tak jak w #2.
>
> Krok 1 (`has_old_messages_to_send`, wywoływane raz na starcie
> `send_old_messages` i raz przez wołających wcześniej — patrz #7,
> nienaprawione) nadal skanuje pod lockiem, ale to **tylko skan** (memcmp na
> ~512 wiadomościach, bez I/O) — rząd wielkości tańszy niż wysyłka, więc
> pozostawiony bez zmian.
>
> Zweryfikowane end-to-end pod realnym buildem (`-Werror -Wall`, zero
> warningów): 20 szybkich podmian `Startup-Message` na gorący kanał +
> równoległy subscriber (bez crashy, `messages_in_trash` poprawnie
> rośnie/spada po TTL); catch-up przez `Last-Event-Id` (5 wiadomości,
> poprawna kolejność od granicy); `backtrack=2` i `backtrack=30` na 55
> zbuforowanych wiadomościach (poprawna liczba i kolejność ramek, pierwsza/
> ostatnia zgodne z oczekiwaniem).

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
- ✅ wysyłka snapshotu **i** catch-upu **poza** `channel->mutex` — zaimplementowane
  (obie części), patrz ramka wyżej. Pod lockiem zostaje tylko sam skan
  (`has_old_messages_to_send`, tanie, bez I/O)

### 🟠 2. UAF przy wolnym kliencie a wymiana `startup_message` / `channel_deleted_message`  ✅ NAPRAWIONE dla `startup_message` (branch `test23`)

> **Fix zaaplikowany 2026-09-09:** to i fix #1 to **jeden mechanizm**, nie dwa
> osobne — nie da się bezpiecznie wysłać snapshotu poza `channel->mutex` (#1)
> bez jednoczesnego zamknięcia tego UAF, bo to dokładnie to samo okno czasowe
> widziane z dwóch końców.
>
> - `send_startup_message()` (`subscriber.c`) — pod `channel->mutex`:
>   `msg->workers_ref_count++` **przed** unlockiem. Gwarantuje, że żaden
>   `set_channel_startup_message()` nie może już podmienić wskaźnika w
>   sposób, który pozwoliłby ominąć ten refcount (podmiana i inkrement są w
>   tym samym krytycznym sekcji `channel->mutex` — nie mogą się przeplatać).
>   Po wysyłce (poza lockiem): dekrement pod `shpool->mutex` (ta sama
>   konwencja co `ngx_http_push_stream_free_worker_message_memory`).
> - `set_channel_startup_message()` (`utils.c`) — zamiast bezwarunkowego
>   `free_message_memory(old)`, **zawsze** `ngx_http_push_stream_throw_the_message_away(old_msg, ...)`
>   — ten sam mechanizm trash+TTL co zwykłe wiadomości z kolejki
>   (`NGX_HTTP_PUSH_STREAM_DEFAULT_SHM_MEMORY_CLEANUP_OBJECTS_TTL` = 10s).
>   Realne zwolnienie dopiero gdy `workers_ref_count<=0` **i** TTL minął —
>   nie próbuje się precyzyjnie śledzić momentu faktycznego flush do
>   socketu (nginx nie daje na to czystego hooka), tylko daje bezpieczny
>   margines jak dla zwykłych wiadomości.
>
> **`channel_deleted_message` NIE jest tu tknięty** — inny cykl życia
> (ustawiany raz przy usunięciu kanału, sam kanał i tak czeka
> `NGX_HTTP_PUSH_STREAM_DEFAULT_SHM_MEMORY_CLEANUP_OBJECTS_TTL` = 10s w
> `channels_trash` zanim `nxg_http_push_stream_free_channel_memory` go
> zwolni — ma więc już wbudowany 10s grace, mniej pilne). Zostaje jako
> mniejsze ryzyko rezydualne.
>
> Zweryfikowane end-to-end: publish `Startup-Message: 1` × 20 w pętli na
> gorący kanał + subskrybent w locie — brak crashy/CRIT w logu, kolejny
> subskrybent dostaje najnowszy snapshot, `messages_in_trash` rośnie przy
> podmianach i poprawnie spada do 0 po ~10s.

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

### 🟠 3. CPU publikacji — `deflateInit2`/`deflateEnd` per wiadomość per kanał  ✅ NAPRAWIONE (branch `test23`)

> **Fix zaaplikowany 2026-09-08:**
> - **`deflateReset()` per worker** — jeden trwały `z_stream` na proces workera
>   (`ngx_http_push_stream_deflate_zs` w `utils.c`), resetowany przed każdą
>   wiadomością zamiast `deflateInit2`/`deflateEnd`. Znika ~256 KB alloc+free
>   na każdą publikowaną wiadomość (i N× przy fan-oucie wildcard). Sprzątany w
>   `ngx_http_push_stream_exit_worker`. Workery to osobne procesy jednowątkowe
>   na event-loopie → brak locka.
> - **Nowa dyrektywa `push_stream_websocket_permessage_deflate on|off`**
>   (per location, domyślnie `on`). `off` = (1) handshake WS nie negocjuje
>   `permessage-deflate` (`ctx->deflate_enabled` zostaje 0, brak nagłówka
>   odpowiedzi) **i** (2) `convert_char_to_msg_on_shared` **nie buduje** kopii
>   skompresowanej — zero kosztu CPU przy publikacji i **brak drugiej kopii w
>   shm**. Flaga jest per-szablon (`template->websocket_permessage_deflate`),
>   OR-owana po wszystkich lokacjach WS używających danego szablonu — jeśli
>   choć jedna chce deflate, kopia powstaje.
> - Kompiluje się czysto (nginx 1.26.2, z/bez `NGX_HAVE_ZLIB`, `-Wall`).
>
> Opis poniżej dla kontekstu.

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

### 🟡 5. `ngx_http_push_stream_shm_free_pages()` pod `shpool->mutex` na endpoincie statystyk  ✅ NAPRAWIONE (branch `test23`)

> **Fix zaaplikowany 2026-09-09:** `ngx_http_push_stream_get_shm_gauge()` w
> `utils.c` — cache per-worker z TTL 3 s (`NGX_HTTP_PUSH_STREAM_SHM_GAUGE_TTL`):
> `total_size` liczony raz (niezmienny po inicjalizacji strefy, czytany bez
> locka), walk free-listy odświeżany co najwyżej raz na TTL, **twardy cap**
> `NGX_HTTP_PUSH_STREAM_SHM_GAUGE_WALK_CAP` = 4096 runów niezależnie od TTL
> (flaga `capped` sygnalizuje zaniżenie). Endpoint stats i diagnostyka
> alloc-failure (#11) współdzielą ten sam cache.
>
> **Przy okazji naprawiony błąd w samej metryce:** stara wersja liczyła
> **wpisy listy** (runy), nie strony, i podpisywała to jako `shm_free_pages` →
> `shm_free_mb` był zaniżony nawet o 1–2 rzędy wielkości pod fragmentacją.
> Zweryfikowane end-to-end: świeża strefa 32 MB, jeden wolny run →
> stara logika zwróciłaby `shm_free_pages: 1` → `shm_free_mb: 0`; nowa
> poprawnie: `shm_free_pages: 8128, shm_free_mb: 31`.
>
> **Dwa nowe pola w `/channels-stats` summarized** (wszystkie formaty:
> PLAIN/JSON/YAML/XML): `shm_free_runs` (fragmentacja — rosnące przy płaskim
> `shm_free_mb` = pogarszająca się fragmentacja) i
> `shm_largest_free_run_kb` (**najważniejsza liczba** — jeśli mniejsza od
> Twojej największej alokacji, dostaniesz „no memory" mimo wolnego miejsca).
>
> Zweryfikowane realnym buildem (`-Werror -Wall`, zero warningów, `make`
> pełnego `objs/nginx`) i uruchomieniem: handshake WS, `?`-ping→pong,
> `push_stream_websocket_max_message_length` odrzuca zbyt długą deklarowaną
> ramkę (close frame, bez czytania payloadu), `push_stream_websocket_permessage_deflate off`
> poprawnie nie zwraca nagłówka rozszerzenia mimo oferty klienta.
>
> Opis problemu poniżej dla kontekstu.

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
   kanałów, ile kompresji/s) — po fixie `deflateReset` sprawdzić czy churn
   alokatora zniknął.
4. **shm**: obserwować `shm_free_mb` przez 24–48h przy produkcyjnym ruchu — czy
   trend opada (wyciek) czy stabilny.
5. **RSS workerów #8**: 24–48 h produkcyjnego ruchu — czy RSS per worker rośnie
   monotonicznie (narost `r->pool` z acków komend `+`/`-`/`?`). Przy
   `ping_mode application` + przeglądarki spodziewany płaski; jeśli rośnie →
   recyklowany `ctx->cmd_out` (patrz #8).
6. **Inflate #9**: klient wysyła skompresowaną ramkę WS ~5–10 KB (kompresowalną
   >4×), sprawdzić czy dochodzi w całości czy połączenie jest zrywane.

---

## Uzupełnienie — druga analiza (2026-09-08, przegląd `websocket.c` / `utils.c` liniowo)

Dodatkowe ustalenia, których nie ma w rankingu 1–7 powyżej:

### 🟡 8. `r->pool` rośnie monotonicznie z liczbą komend na połączeniu WS

> **Korekta 2026-09-09:** pierwotnie oceniony 🔴 z fixem „przenieś na
> `ctx->temp_pool`". Po analizie: **fix przez `temp_pool` jest błędny** (UAF),
> a skala dla konfiguracji f24 jest niska. Zejście na 🟡. Szczegóły niżej.

Nowe ścieżki komend WS alokują z `r->pool`, który żyje przez **całe** połączenie
(request WS nie finalizuje się aż socket się zamknie; przy
`subscriber_connection_ttl 4h` — do 4 h, przy `0` — bezterminowo). Pule nginx to
alokatory bump — brak zwalniania pojedynczej alokacji, pamięć wraca dopiero przy
`ngx_destroy_pool`.

- `ngx_http_push_stream_websocket_handle_ping()` — `json` + `frame` z `r->pool`
  przy **każdym** `?`-pingu (`websocket.c` ~97, ~115)
- `ngx_http_push_stream_websocket_send_ack()` — przy każdym `+` / `-` / błędzie
  (`websocket.c` ~40, ~46)
- `handle_subscribe()` — JSON `unknown_event_id` + frame z `r->pool` (~207)
- `send_startup_message()` — `boundary_event_id` do `r->pool` przy każdym
  resubscribe bez event-id (`subscriber.c` ~486)

Każda alokacja ~60–280 B, **za każdym razem gdy klient wyśle tę komendę**,
zwalniane dopiero przy rozłączeniu → monotoniczny narost RSS workera per
połączenie, **niewidoczny w `shm`** (to pamięć procesu, nie strefa współdzielona;
`/channels-stats` tego nie pokazuje — widać tylko jako rosnący RSS procesu).

**Dlaczego to NIE jest naiwny błąd — `r->pool` jest tu wyborem bezpieczeństwa:**
`send_response_text` jest **zero-copy** (`b->memory = 1; b->pos = text` — wskaźnik
prosto w pulę). Bajty ramki muszą przeżyć **asynchroniczny** zapis do socketu;
przy zatkanym TCP (a przy 100k połączeń część zawsze jest) nginx trzyma bufor w
łańcuchu `busy` i `b->pos` musi być ważne aż się opróżni. Przeniesienie na
`ctx->temp_pool` (niszczoną per-ramka) daje **use-after-free** na acku wolnego
klienta. Ścieżka głównych wiadomości jest bezpieczna, bo źródłem jest shared
memory (żyje `message_ttl` = 10 min).

**Skala dla f24 — prawdopodobnie niska:** `push_stream_ping_mode application` =
**serwer** pinguje klienta, bezczynna przeglądarka nic nie wysyła → zero narostu.
Narost tylko z `+`/`-` resubscribe (nawigacja między meczami), ewentualnego
klienckiego `?`-pingu, publish przez WS. Szac. user przełączający mecz co minutę
przez godzinę ≈ 120 acków × ~160 B ≈ ~19 KB/h/połączenie; przez 4 h TTL
kilkadziesiąt–kilkaset KB, × 100k ≈ jednostki GB w skali floty, resetowane przy
recyklingu połączeń na TTL.

**Właściwy fix (jeśli pomiar pokaże narost):** jeden mały bufor wyjściowy komend
**recyklowany per połączenie** — w danym momencie w locie jest zawsze jedna
ramka-odpowiedź. W `ctx`: `cmd_out` ~512 B alokowane raz z `r->pool`, każdy
ack/pong pisany do tego samego bufora, nadpisanie dopiero gdy łańcuch `busy`
połączenia jest pusty (fallback: jednorazowa alokacja z `r->pool` w rzadkim
przypadku zatkania). Efekt: stała pamięć wyjścia komend per połączenie zamiast
liniowej, bez ryzyka UAF.

**Do zrobienia najpierw:** zmierzyć RSS workerów przez 24–48 h produkcyjnego
ruchu (netdata, metryki procesu). Przy `ping_mode application` + przeglądarki
najpewniej płaski — wtedy fix niepotrzebny.

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

### 🟠 10. `deflate` bezwarunkowy i bez przełącznika konfiguracyjnego  ✅ NAPRAWIONE (branch `test23`)

> **Fix:** dyrektywa `push_stream_websocket_permessage_deflate on|off` (patrz
> ramka przy #3). `off` wyłącza negocjację handshake **i** budowę kopii w shm.

Rozszerzenie #3: `deflate_websocket_frame()` jest wołane przy **każdej** publikacji
gdy istnieje szablon `websocket` i payload ≥ 200 B — niezależnie od tego, czy
jakikolwiek subskrybent wynegocjował deflate. Skompresowana kopia siedzi w shm
przez cały TTL wiadomości → **~2× pamięci na przechowywane wiadomości** przy
zerowym zysku, jeśli klienci deflate nie występują. Brak dyrektywy typu
`push_stream_websocket_permessage_deflate on|off`. Dla f24 klienci to przeglądarki
(deflate zawsze) więc zysk jest realny — ale brak kill-switcha na wypadek
regresji CPU/pamięci pod produkcyjnym fan-outem.

### 🟡 11. `log_slab_alloc_failure()` woła `shm_free_pages()` na KAŻDYM nieudanym alloc  ✅ NAPRAWIONE (branch `test23`)

> **Fix zaaplikowany 2026-09-09:** dwie niezależne warstwy, patrz #5:
> 1. sam walk korzysta teraz z cache'owanego gauge'a (TTL 3 s) — większość
>    wywołań w serii failures w ogóle nie bierze `shpool->mutex`
> 2. **rate-limit samej linii logu** (`NGX_HTTP_PUSH_STREAM_ALLOC_FAILURE_LOG_INTERVAL`
>    = 2 s) niezależnie od cache'u gauge'a — burza failures generowała też
>    burzę linii `NGX_LOG_CRIT` (I/O logu); teraz pierwsza awaria loguje się
>    natychmiast, kolejne w oknie 2 s są tylko zliczane
>    (`ngx_http_push_stream_suppressed_alloc_failures`) i zwinięte w
>    kolejną linię („N more failures suppressed in the last 2s")

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
