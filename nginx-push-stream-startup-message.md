# nginx-push-stream-module — Wiadomosc startowa kanalu (Startup-Message)

## Opis

Kazdy kanal moze miec **jedna** trwala "wiadomosc startowa" (snapshot stanu).
Subskrybent, ktory dolacza do kanalu **bez proszenia o historie** (bez
`Last-Event-Id`, bez `If-Modified-Since`, bez backtracku `.b`), dostaje:

```
wiadomosc startowa  +  wszystkie wiadomosci z bufora opublikowane PO jej ustawieniu  +  live
```

czyli **snapshot + catch-up + live**, bez luki i bez potrzeby wysylania
`Last-Event-Id`. Wiadomosc startowa niesie wlasny `Event-Id`, wiec po odebraniu
calosci klient ma spojny punkt do wznawiania przy nastepnym polaczeniu.

Subskrybent, ktory podaje `Last-Event-Id` (wznawianie po zerwaniu), **nie**
dostaje wiadomosci startowej — ma juz stan i wznawia normalnie z bufora.

Wiadomosc startowa:

- **nie** trafia do bufora wiadomosci (`push_stream_message_buffer_size`),
- **nie** jest rozsylana do juz podlaczonych subskrybentow,
- **nie** wygasa (zyje do nadpisania / wyczyszczenia / usuniecia kanalu),
- przezywa `nginx -s reload` (lezy na strukturze kanalu w pamieci wspoldzielonej,
  jak `Message-TTL` czy komunikat usuniecia kanalu).

Dziala we wszystkich trybach subskrypcji: streaming, EventSource, WebSocket,
polling i long-polling, a takze przy dynamicznym `+channel` (bez `event_id`)
gdy wlaczone jest `push_stream_websocket_allow_resubscribe`.

### Granica catch-up

Serwer wybiera granice odtwarzania (od ktorego miejsca w buforze doslac
wiadomosci po snapshocie) tak:

**1. `Event-Id` wiadomosci startowej** (gdy publisher go ustawil) — priorytet.

Publisher, budujac snapshot, wie ktora ostatnia wiadomosc snapshot uwzglednia,
i podaje jej `Event-Id` w naglowku wiadomosci startowej: *„ten snapshot jest
aktualny na stan wiadomosci X"*. Serwer szuka w buforze wiadomosci z tym
`event_id` i odtwarza **wszystko po niej** — dokladnie ta sama semantyka co
`Last-Event-Id` przysłany przez klienta.

To jest **deterministyczne przy wielu publisherach naraz**: ciecie wyznacza
konkretna tozsamosc wiadomosci, a nie chwila zegara zlapana w losowym momencie.
Kolejnosc publikacji z roznych polaczen i tak jest ustalana przez serwer (przez
kto pierwszy zdobedzie locka kanalu) — snapshot pinuje ciecie w tym samym
porzadku.

- `Event-Id` = id ostatniej realnej wiadomosci objetej snapshotem → snapshot +
  wiadomosci po niej.
- `Event-Id` = swiezy, nigdy niepublikowany identyfikator → snapshot + tylko
  live (brak catch-up), a klient i tak dostaje id do wznawiania. Tryb „snapshot
  jest kompletny, zaczynaj strumien od zera".
- `Event-Id` juz wyrotowal z bufora (za stary snapshot) → brak catch-up, sam
  snapshot + live. **Kontrakt publishera: publikowac swiezy snapshot zanim jego
  `Event-Id` wypadnie z bufora** (`push_stream_message_buffer_size` /
  `push_stream_message_ttl`). To samo ograniczenie co przy kliencie wznawiajacym
  ze starym `Last-Event-Id`.

**2. Pozycja strumienia** — fallback, gdy wiadomosc startowa nie ma `Event-Id`.

Serwer zapamietuje w niej biezaca pozycje kanalu (`last_message_time` /
`last_message_tag`) z chwili ustawienia i odtwarza wszystko z bufora o pozycji
**wiekszej**. Snapshot na pustym kanale → pozycja `(0, 0)` → odtwarza sie caly
przyszly bufor.

W obu wariantach kolejny snapshot przesuwa granice do przodu — nastepni klienci
dostaja nowy snapshot + tylko to, co po nim. Naturalny wzorzec: publikowac
swiezy snapshot co jakis czas, zeby catch-up pozostawal krotki mimo duzego
bufora.

---

## Protokol — ustawianie i czyszczenie

Wiadomosc startowa ustawia sie **na tym samym locationie publishera** co
zwykle wiadomosci, dodajac naglowek `Startup-Message`:

```
POST /pub?id=matchA HTTP/1.1
Startup-Message: 1
Event-Id: snap_1712345678
Content-Type: application/json

{"type":"snapshot","score":"1:0","minute":37, ...}
```

- Naglowek `Startup-Message` obecny + **niepuste** cialo → cialo zostaje
  zapisane jako wiadomosc startowa kanalu (nadpisuje poprzednia, przesuwa
  granice catch-up).
- Naglowek `Startup-Message` obecny + **puste** cialo → wiadomosc startowa
  kanalu zostaje wyczyszczona.
- `Event-Id` / `Event-Type`, jesli podane, dotycza wiadomosci startowej
  (formatowane w szablonie tak samo jak przy zwyklej publikacji). `Event-Id`
  pelni dodatkowo role granicy catch-up — patrz „Granica catch-up" wyzej.
- Dziala z lista kanalow (`id=a,b,c`) i z kanalem wildcard — kazdy wskazany
  kanal dostaje wlasna kopie. **Nie** dziala z `id=ALL` ani z `*` w id
  (odrzucane tak samo jak przy zwyklej publikacji).
- Wartosc naglowka jest ignorowana — liczy sie sama obecnosc. Konwencjonalnie
  `Startup-Message: 1`.

### Odpowiedzi

| Sytuacja | Status | Cialo |
|---|---|---|
| ustawiono | `200 OK` | `Startup message set.` (lub JSON `channel_info_on_publish`) |
| wyczyszczono | `200 OK` | `Startup message cleared.` |
| blad alokacji w pamieci wspoldzielonej | `500` | — |

---

## Zachowanie po stronie subskrybenta

| Polaczenie | Co dostaje |
|---|---|
| `GET /sub/matchA` (bez naglowkow historii) | wiadomosc startowa + bufor po granicy + live |
| `GET /sub/matchA` + `Last-Event-Id: X` | to co po `X` z bufora (bez wiadomosci startowej) |
| `GET /sub/matchA.b3` (backtrack) | ostatnie 3 wiadomosci z bufora (bez startowej) |
| `GET /lp/matchA` (long-polling, swiezy) | wiadomosc startowa + bufor po granicy |
| WS `/ws/matchA` (initial, bez `Last-Event-Id`) | wiadomosc startowa + bufor po granicy + live |
| WS `+matchA` (dynamic subscribe, bez `:event_id`) | ACK, potem wiadomosc startowa + bufor po granicy |
| WS `+matchA:X` | ACK, potem to co po `X` z bufora (bez startowej) |

W trybie polling: swiezy poll z oczekujaca wiadomoscia startowa jest liczony
jako "sa dane do wyslania" (nie zwraca `304`, long-poll nie parkuje). Naglowki
`Last-Modified` / `Etag` odpowiedzi wskazuja **najnowsza** wiadomosc kanalu
(albo, na kanale bez zadnej wiadomosci w buforze, biezacy czas), wiec kolejny
poll klienta (z `If-Modified-Since` / `If-None-Match`) nie odtwarza juz
snapshotu — wznawia normalnie od tego punktu.

---

## gzip / kompresja

- **HTTP (streaming / EventSource / polling / long-polling)**: filtr `gzip` nginx
  kompresuje caly strumien odpowiedzi razem z wiadomoscia startowa i catch-up —
  nic dodatkowego nie trzeba robic (`gzip on` + `gzip_types` na locationie
  subskrybenta). Zweryfikowane: `Content-Encoding: gzip` obejmuje snapshot.
- **WebSocket (`permessage-deflate`)**: wiadomosc startowa jest budowana z
  wariantami skompresowanymi tak samo jak kazda inna wiadomosc i wysylana jako
  ramka `0xC1` (RSV1=1), gdy kompresja faktycznie oszczedza bajty; male
  wiadomosci ida plain — identycznie jak zwykle wiadomosci.

---

## Maksymalna wielkosc

Modul **nie ma wlasnego limitu** — wiadomosc startowa idzie ta sama sciezka
zapisu i dostarczania co kazda publikowana wiadomosc. Zweryfikowane: 3 MB,
16 MB, 50 MB przechodza (POST `200`, streamowane z powrotem co do bajta).

Ograniczenia realne, po kolei:

**1. Upload publishera — `client_max_body_size` nginx (domyslnie 1 MB).**
Na locationie publishera. Body powyzej limitu → `413` z rdzenia nginx, zanim
modul cokolwiek zobaczy. Wieksze → podniesc `client_max_body_size` (ew.
`client_body_buffer_size`, zeby nie spoolowac na dysk).

**2. Pamiec wspoldzielona — `push_stream_shared_memory_size`.**
Przechowywana chunkami po 4 KB (`NGX_HTTP_PUSH_STREAM_CHUNK_SIZE`) — brak
problemu z fragmentacja / ciagla alokacja slab. Koszt na jedna wiadomosc
startowa:

```
raw  +  sformatowana kopia × (liczba push_stream_message_template + wariant eventsource + wariant websocket)  +  skompresowana kopia (WS, jesli oszczedza)
```

Trwala, **jedna na kanal**. Przy braku miejsca: `500` + log
`ngx_slab_alloc() failed: no memory`, **bez crasha**, stara wiadomosc startowa
nietknieta (nowa podmieniana dopiero po udanym zbudowaniu).

**3. Typy** — brak obciecia 32/64-bit; ramki WS uzywaja 64-bitowej dlugosci.

### Realny sufit — koszt fan-out

To ogranicza w praktyce, nie zaden hardcoded limit. Kazdy subskrybent laczacy
sie **bez `Last-Event-Id`** dostaje **cala** wiadomosc startowa zapisana do
bufora swojego polaczenia w momencie connectu. Przy skali dziesiatek tysiecy
WS masowy reconnect (deploy, `nginx -s reload`) × duzy snapshot = duzy chwilowy
skok pamieci i pasma, np. `40 000 × 200 KB ≈ 8 GB` przejsciowo.

**Rekomendacja:**

- trzymac snapshot maly — cel: pojedyncze KB do najwyzej kilkudziesieciu KB,
- polegac na gzip / permessage-deflate (dzialaja na wiadomosci startowej),
- stan z natury duzy → rozdzielic: snapshot = zwiezly „stan biezacy",
  szczegoly przez catch-up albo osobny fetch on-demand,
- nie wrzucac payloadow MB-skali „bo dziala" — dziala, ale mnozy sie przez
  liczbe reconnectow.

---

## Przyklad

```bash
# publikacje live (trafiaja do bufora + do biezacych subskrybentow)
curl -sS -X POST 'http://localhost/pub?id=matchA' -H 'Event-Id: goal_1' -d '{"score":"1:0"}'
curl -sS -X POST 'http://localhost/pub?id=matchA' -H 'Event-Id: goal_2' -d '{"score":"1:1"}'

# snapshot: publisher liczy stan uwzgledniajac wszystko do goal_2 i podaje
# ten sam Event-Id -> "aktualny na stan goal_2"
curl -sS -X POST 'http://localhost/pub?id=matchA' \
     -H 'Startup-Message: 1' -H 'Event-Id: goal_2' \
     -d '{"type":"snapshot","score":"1:1"}'

curl -sS -X POST 'http://localhost/pub?id=matchA' -H 'Event-Id: goal_3' -d '{"score":"2:1"}'

# nowy klient bez Last-Event-Id dostanie: snapshot(goal_2), goal_3, potem live
#   -> goal_1/goal_2 NIE (snapshot juz je zawiera), zadnej luki miedzy snapshotem a goal_3
# klient z Last-Event-Id: goal_3 dostanie: tylko live (bez snapshotu)

# kolejny snapshot z Event-Id: goal_3 -> granica przesuwa sie za goal_3

# wyczysc snapshot startowy
curl -sS -X POST 'http://localhost/pub?id=matchA' -H 'Startup-Message: 1' -d ''
```

```javascript
const ws = new WebSocket('ws://localhost/ws/matchA');
let lastEventId = null;

ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.subscribed || m.unsubscribed || m.error) return;
    lastEventId = m.event || lastEventId;   // 'snap_1' -> 'goal_1' -> 'goal_2' -> ...
    render(m);
};

// przy reconnect: URL / naglowek z Last-Event-Id = lastEventId
// -> serwer nie wysle ponownie wiadomosci startowej, tylko to co po lastEventId
```

---

## Logika dzialania — przeplyw

### A. Ustawianie / czyszczenie (publisher)

`ngx_http_push_stream_publisher_body_handler()`:

1. odczyt naglowka `Startup-Message` (`ngx_http_push_stream_get_header`).
2. **puste cialo** → dla kazdego wskazanego kanalu
   `ngx_http_push_stream_set_channel_startup_message(mcf, channel, NULL)` →
   `200` `Startup message cleared.` — **return** (przed generyczna walidacja
   pustego body).
3. **niepuste cialo** → po odczycie body i naglowkow `Event-Id` / `Event-Type`,
   dla kazdego kanalu:
   - pod `channel->mutex` odczyt `last_message_time` / `last_message_tag`
     (fallbackowa granica catch-up),
   - `ngx_http_push_stream_convert_char_to_msg_on_shared(... id = STARTUP_MESSAGE_ID (-4),
     event_id, event_type, since_time, since_tag ...)` — buduje wiadomosc w
     pamieci wspoldzielonej (raw chunked + sformatowane kopie per szablon +
     skompresowane warianty WS), tak samo jak zwykla wiadomosc,
   - `ngx_http_push_stream_set_channel_startup_message(mcf, channel, startup_msg)`
     — atomowy swap pod `channel->mutex`, `ngx_http_push_stream_free_message_memory`
     starej **po** zwolnieniu locka.
   - odpowiedz: `channel_info_on_publish` → JSON, inaczej `200`
     `Startup message set.` — **return** (nie wchodzi do `message_queue`,
     nie `broadcast`, nie liczy `published_messages`).

### B. Przechowywanie

`channel->startup_message` (`ngx_http_push_stream_msg_t *`) na strukturze kanalu
w pamieci wspoldzielonej — **poza** `channel->message_queue`. Init `NULL` w
`ngx_http_push_stream_get_channel()`. Zwolnienie w
`nxg_http_push_stream_free_channel_memory()`. Przezywa reload (struct kanalu nie
jest odtwarzana).

### C. Dostarczanie — punkty wpiecia

| Tryb | Funkcja | Miejsce wywolania |
|---|---|---|
| streaming, EventSource, WS initial | `ngx_http_push_stream_subscriber_assign_channel()` | po ewentualnym `send_old_messages` (klient z `Last-Event-Id`), **przed** `assing_subscription_to_channel()` |
| polling, long-polling | `ngx_http_push_stream_subscriber_polling_handler()` | petla detekcji: `startup_message != NULL` na swiezym kanale → `has_message_to_send = 1` + korekta `greater_message_time`/`tag`; petla wysylki: przed `send_old_messages` danego kanalu |
| WS dynamic `+channel` (bez `:event_id`) | `ngx_http_push_stream_websocket_handle_subscribe()` | galaz `else` po ACK `{"subscribed":...}` |

Warunek „swiezosci" w kazdym miejscu: `last_event_id == NULL &&
if_modified_since < 0 && backtrack_messages == 0`.

### D. `ngx_http_push_stream_send_startup_message(r, channel)`

1. szybki test bez locka: `channel == NULL || channel->startup_message == NULL` → `return 0`.
2. `ngx_shmtx_lock(channel->mutex)`; ponowny test `NULL || deleted` → unlock, `return 0`.
3. jeszcze pod lockiem: skopiowanie granicy —
   - `since_time` / `since_tag` z pol `time` / `tag` wiadomosci,
   - jesli `startup_message->event_id != NULL` → **kopia** tego stringa do
     `r->pool` (`boundary_event_id`), bo po zwolnieniu locka rownolegly
     `set_channel_startup_message` moze go zwolnic.
4. `ngx_http_push_stream_send_response_message(r, channel, channel->startup_message, 0, ctx->message_sent)`
   — wyslanie snapshotu (WS: ramka `0x81`/`0xC1`; EventSource: linie `id:`/`event:`).
5. `ngx_shmtx_unlock(channel->mutex)`.
6. replay bufora (`send_old_messages` sam bierze `channel->mutex`):
   - `boundary_event_id != NULL` → `send_old_messages(r, channel, 0, -1, -1, 0, -1, boundary_event_id)`
     — semantyka `Last-Event-Id`: znajdz w buforze wiadomosc z tym `event_id`,
     wyslij wszystko po niej; nie znaleziono → nic (sam snapshot),
   - inaczej → `send_old_messages(r, channel, 0, since_time, since_tag, 0, -1, NULL)`
     — wszystko z bufora o pozycji `> (since_time, since_tag)`.
7. `return 1`.

Kolejnosc w efekcie: **snapshot → catch-up z bufora → (subskrypcja) → live**.

---

## Zmienione pliki

| Plik | Zakres zmian |
|---|---|
| `include/ngx_http_push_stream_module.h` | pole `startup_message` w strukturze kanalu, naglowek `Startup-Message`, stringi odpowiedzi, wpis w `ALLOWED_HEADERS` (CORS) |
| `include/ngx_http_push_stream_module_utils.h` | `NGX_HTTP_PUSH_STREAM_STARTUP_MESSAGE_ID` (-4), deklaracja `set_channel_startup_message()` |
| `src/ngx_http_push_stream_rbtree_util.c` | inicjalizacja `channel->startup_message = NULL` |
| `src/ngx_http_push_stream_module_utils.c` | `ngx_http_push_stream_set_channel_startup_message()` (atomowy swap+free pod `channel->mutex`), zwolnienie w `nxg_http_push_stream_free_channel_memory()` |
| `src/ngx_http_push_stream_module_publisher.c` | obsluga naglowka `Startup-Message` w `publisher_body_handler` (set / clear); stemplowanie fallbackowej granicy (pozycja strumienia) w polach time/tag |
| `src/ngx_http_push_stream_module_subscriber.c` | `ngx_http_push_stream_send_startup_message()` — snapshot + replay bufora od granicy: `Event-Id` snapshotu (semantyka `Last-Event-Id`, kopiowany z shm pod lockiem) albo fallback na pozycje strumienia; wpiecia w `subscriber_assign_channel` (streaming/WS initial) i `subscriber_polling_handler` (detekcja + petla wysylki, korekta `Last-Modified`/`Etag`) |
| `src/ngx_http_push_stream_module_websocket.c` | wyslanie wiadomosci startowej przy `+channel` bez `event_id` |

---

## Uwagi implementacyjne

- **Wspolbieznosc**: `send_startup_message()` wysyla snapshot pod `channel->mutex`
  (jak `send_old_messages()`) i jeszcze pod lockiem kopiuje wszystko, czego
  potrzebuje replay — `Event-Id` granicy do puli requestu (bo stara wiadomosc
  startowa z tym id moze zostac zwolniona po zwolnieniu locka) albo pare
  time/tag. `set_channel_startup_message()` podmienia wskaznik pod tym samym
  mutexem i zwalnia stara wiadomosc dopiero po zwolnieniu locka — czytelnik na
  innym workerze widzi albo stara (i konczy jej wysylke), albo nowa, nigdy
  zwolnionej. Sam replay bufora (`send_old_messages`) bierze `channel->mutex`
  sam.
- **Determinizm ciecia**: granica po `Event-Id` (find-in-buffer) nie zalezy od
  chwili zegara ani od tego, ilu publisherow pisze rownolegle — publisher
  nazywa konkretna wiadomosc. Wariant fallback (time/tag) ma ~1 s okno
  nieprecyzji na wspolbieznych publikacjach w tej samej sekundzie; klient i tak
  deduplikuje po `event-id`.
- **Okno miedzy catch-up a subskrypcja**: w `subscriber_assign_channel` snapshot
  i replay leca **przed** `assing_subscription_to_channel` — tak samo jak
  istniejace `send_old_messages`; te same charakterystyki wyscigu co dotad.
- **Reload z inna konfiguracja szablonow**: sformatowane kopie wiadomosci
  startowej pochodza z generacji configu, ktora ja ustawila. Jak przy
  wiadomosciach w buforze, `get_formatted_message()` wykrywa niezgodnosc
  liczby szablonow po reloadzie i wraca do surowej tresci — brak crasha.
- Wiadomosc startowa nie jest liczona w statystykach `published_messages` /
  `stored_messages` — nie jest "opublikowana" w sensie bufora.
