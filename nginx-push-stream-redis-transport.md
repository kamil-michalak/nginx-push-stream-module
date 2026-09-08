# nginx-push-stream-module — Transport przez Redis (studium wykonywalnosci)

> Status: **wczesna analiza / feasibility study**. Dokument roboczy, projekt jeszcze
> nie zamrozony. Sekcja „Pytania otwarte" zbiera decyzje do podjecia przed PoC.

---

## 1. Cel

Dzis modul publikuje przez HTTP `POST /pub`. Publikacja trafia do pamieci
wspoldzielonej (`shm`) jednej instancji nginx i przez IPC do subskrybentow na
pozostalych workerach **tej samej instancji**. Przy kilku replikach nginx kazda
replika ma wlasny `shm` — nie ma wspolnego stanu.

Docelowo:

1. **Publikacja tylko przez Redis.** Zadnych `POST /pub` w sciezce produkcyjnej.
   Kazda replika nginx czyta te same wiadomosci z Redis i dostarcza je swoim
   subskrybentom.
2. **Jedna wiadomosc → wiele kanalow.** Pojedynczy wpis w Redis moze wskazywac
   liste kanalow docelowych.
3. **Rejestr kanalow.** Istnieje lista wszystkich kanalow z propsami. Kanal spoza
   listy **nie istnieje** (nie tworzymy kanalow na zadanie). Zmiany listy sa
   propagowane przez **Redis stream** (bez pollingu), z aplikacji i z nginx.
4. **Leniwa aktywacja kanalu z potwierdzeniem API.** Gdy na kanal wejdzie
   **pierwszy** subskrybent: nginx (a) publikuje zdarzenie na stream, (b) oznacza
   kanal jako „aktywujacy sie" na liscie, (c) **wstrzymuje wysylke do usera** do
   momentu `confirm` z API. Powiadomienie do API leci **dokladnie raz**.
5. **Publikacja przez czytanie jednej lub wielu list/streamow.**

---

## 2. Jak modul dziala dzis (punkt wyjscia)

| Element | Stan obecny | Znaczenie dla projektu |
|---|---|---|
| Model procesu | nginx: N workerow, single-thread event-loop, wspolny `shm` + IPC po `socketpair` | Redis I/O **musi** byc asynchroniczne (zero blokujacych wywolan) |
| Publikacja | `ngx_http_push_stream_publisher_body_handler` → `ngx_http_push_stream_add_msg_to_channel` (zapis do `shm`) → `ngx_http_push_stream_broadcast` (alert IPC do workerow z subskrybentami) | **Ta sciezka jest do ponownego uzycia** — konsument Redis wywoluje dokladnie te funkcje |
| Wiele kanalow na publikacje | juz wspierane (`id=a,b,c`), z deduplikacja surowej tresci (`shared_raw_content`, commit `5b06960`) | Wymaganie #2 juz spelnione na poziomie `shm` |
| Tworzenie kanalow | `get_channel()` tworzy na zadanie; `find_channel()` tylko wyszukuje. Jest `authorized_channels_only` (dzis: patrzy na `stored_messages`) | Hook istnieje, ale semantyke „tylko z rejestru" trzeba dopisac |
| Zdarzenia cyklu zycia | `events_channel` emituje `channel_created` / `channel_destroyed` / `client_subscribed` / `client_unsubscribed` przez `ngx_http_push_stream_send_event()` | Gotowy szkielet detekcji „pierwszy subskrybent" / „kanal pusty" |
| Timery | `ngx_http_push_stream_timer_set/reset` (cleanup pamieci, bufora, ping, disconnect) | Wzorzec do timera reconnectu Redis / timeoutu aktywacji |
| Stan przezywajacy reload | `channel->startup_message`, `channel_deleted_message`, `message_ttl` leza na strukturze kanalu w `shm` i przezywaja `nginx -s reload` | Rejestr kanalow i stan aktywacji tez powinny lezec w `shm` i przezyc reload |
| Zaleznosci zewnetrzne | tylko opcjonalnie `zlib` (permessage-deflate) i SHA1. Brak klienta TCP, brak `hiredis` | Klienta Redis trzeba dostarczyc |
| Wersja bazowa | fork `0.5.5` (`3bc016f`) | — |

Kluczowa obserwacja: **warstwa dostarczania (`shm` + IPC + fan-out do
subskrybentow) juz robi to, czego potrzebujemy.** Projekt dotyczy glownie
**warstwy wejscia** (skad biora sie wiadomosci) i **rejestru + lifecycle kanalu**.

---

## 3. Komponenty do zbudowania

```
                    ┌──────────────────────────── Redis ────────────────────────────┐
                    │  streams:   msgs:*          (wiadomosci do publikacji)         │
                    │             channels:events (delty rejestru + aktywacje)       │
                    │  keys:      channels        (SET/HASH id → props)              │
                    │             channel:<id>    (HASH: props + active flag)        │
                    └───────────────────────────────────────────────────────────────┘
                          ▲  XADD (activate/deactivate)      │  XREAD(GROUP) BLOCK
                          │                                  ▼
   ┌───────────────────────────────────────────────────────────────────────────────┐
   │ nginx (replika R)                                                              │
   │                                                                               │
   │  [C1] Async Redis client        — RESP2, connect/AUTH, reconnect+backoff       │
   │  [C2] Konsument streamow msgs:*  — XREADGROUP loop → add_msg_to_channel(...)    │
   │  [C3] Rejestr kanalow w shm      — bootstrap load + apply delt z channels:events│
   │  [C4] Bramka tworzenia kanalow   — sub/pub tylko dla kanalow z rejestru        │
   │  [C5] Maszyna stanu aktywacji    — INACTIVE→ACTIVATING→ACTIVE, hold subskr.    │
   │  [C6] Producent zdarzen do Redis — XADD activate/deactivate (dokladnie raz)    │
   │  [C7] Konfiguracja + statystyki + docs + testy                                 │
   └───────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Analiza wykonalnosci — komponent po komponencie

### C1. Asynchroniczny klient Redis w workerze nginx

**Problem:** worker nginx nie moze sie blokowac. Potrzebujemy: `SUBSCRIBE`/`XREAD
BLOCK` (dlugo wiszace), `XADD` (krotkie, moga isc pipeline), reconnect z backoff,
obsluga zerwania w polowie komendy.

**Opcje:**

| Opcja | Opis | Ocena |
|---|---|---|
| **A. `hiredis` async + wlasny adapter event-loop** | `hiredis` ma API async z pluginami event-loop (libev/libevent/libuv). Adapter do nginx = ~150 LOC: `addRead/delRead/addWrite/delWrite/cleanup` zmapowane na `ngx_handle_read_event`/`ngx_handle_write_event` na `ngx_connection_t` owijajacym `fd` hiredisa. | Szybciej do PoC. Dorzuca zaleznosc `.so`/build. Parser RESP3, pub/sub, reconnect — gotowe. |
| **B. Wlasny minimalny klient RESP2** | `ngx_event_connect_peer` + wlasny parser RESP (trivial: `+ - : $ *`). Pelna kontrola, **zero zaleznosci** — zgodne z etosem modulu (dzis tylko opcjonalnie zlib). ~400–800 LOC. | Wiecej kodu, ale prostszy w utrzymaniu i budowaniu w obrazie f24. RESP2 wystarcza (`XADD`/`XREADGROUP`/`XACK`/`HGETALL`). |
| **C. Sidecar (proces-most Redis↔`POST /pub` na localhoscie)** | Zero zmian w publikacji w module. | **Odrzucone:** nie realizuje handshake „hold subskrybenta", dorzuca ruchomy element per host, nie spelnia „publikacja tylko po redis" w duchu. |

**Rekomendacja:** **Opcja B** dla wersji docelowej (spójna z modulem, latwy build
w obrazie). Ewentualnie **A** na szybki PoC, potem przepisanie.

**Wykonalnosc:** ✅ realne. Precedensy: `ngx_http_redis_module`, `redis2-nginx-module`
(OpenResty), `lua-resty-redis` — wszystkie robia async Redis w modelu nginx.
Ryzyko: poprawnosc przy partial-read / backpressure na `XADD` / utracie polaczenia
w trakcie komendy. Mitygacja: pokryc testami integracyjnymi (toxiproxy/`iptables`).

### C2. Konsument streamow wiadomosci

**Ktory worker czyta?** Trzy warianty:

| Wariant | Mechanika | Plusy / minusy |
|---|---|---|
| **2a. Kazdy worker w consumer-group** | `XREADGROUP GROUP <grupa-repliki> <consumer=worker-slot> BLOCK 0 COUNT n STREAMS msgs:* >`. Redis rozdziela wpisy miedzy workery repliki, kazdy wpis przetworzony **raz na replike**. Po przetworzeniu `XACK`. Crash workera → `XAUTOCLAIM` przez zywe workery. | Idealnie pasuje do „kazdy worker moze publikowac" (tak jak dzis `/pub`). N polaczen/replike. Naturalny load-balance i failover. |
| **2b. Jeden dedykowany worker czyta, reszta przez istniejace IPC** | Plain `XREAD` od kursora trzymanego w `shm`. Fan-out i tak idzie przez `shm`+`broadcast` (dziala na wszystkie workery). | 1 polaczenie/replike, trywialna deduplikacja (jeden czytelnik, kursor w `shm`). Crash → inny worker wznawia od kursora w `shm`. SPOF w obrebie repliki (nginx respawnuje). |
| **2c. Proces uprzywilejowany / agent** | nginx 1.19.1+ dedykowany proces. | Overkill na ten zakres. |

**Rekomendacja:** na PoC **2b** (najprostsza poprawnosc: jeden kursor w `shm` na
replike). Docelowo **2a (consumer groups)** — kluczowy argument: **last-delivered-id
grupy trzyma Redis**, wiec restart nginx (utrata `shm`) automatycznie wznawia od
miejsca sprzed restartu, a `XAUTOCLAIM` odbiera wpisy dostarczone-ale-nie-`XACK`
sprzed crasha. Przy 2b kursor w `shm` ginie z restartem — trzeba by go zrzucac do
Redis samodzielnie. Patrz sekcja „Restart nginx a bufor".

**Sciezka publikacji po odczycie wpisu:**

```
wpis streamu  →  parse pol {id, data, eventid, eventtype, ttl, startup}
              →  dla kazdego kanalu z listy `id`:
                    kanal w rejestrze?  nie → licznik `skipped_unknown_channel`, log, dalej
                    tak → ngx_http_push_stream_add_msg_to_channel(... shared_raw_content ...)
              →  (jeden shared_raw_content na wpis — dedup jak dzis w publisherze)
              →  aktualizacja kursora w shm (2b) lub XACK (2a)
```

Pola wpisu = odpowiedniki dzisiejszych naglowkow publishera (`Event-Id`,
`Event-Type`, `Message-TTL`, `Startup-Message`) — **semantyka bez zmian**, tylko
transport.

**Wiele streamow (`msgs:*`):** `XREADGROUP ... STREAMS msgs:a msgs:b ... > > ...`
czyta wiele naraz. Sharding np. per dyscyplina / per priorytet — unika
head-of-line blocking (jeden wielki snapshot nie blokuje reszty). Klucz shardingu
= **pytanie otwarte**.

**Gwarancje:** Redis streams = trwalosc + porzadek per stream. Dostarczenie
**at-least-once** (crash w oknie deliver↔ACK → redelivery). Duplikaty tolerowane:
klient deduplikuje po `event-id` (patrz `nginx-push-stream-resubscribe.md` i
`-startup-message.md`). Kolejnosc **per stream** zachowana; miedzy shardami — brak
gwarancji globalnej (akceptowalne, bo kanaly nie przeplataja sie miedzy shardami).

**Wykonalnosc:** ✅ realne, niskie ryzyko — to w praktyce „drugie zrodlo" dla
funkcji, ktora juz istnieje.

---

### C2a. Format publikacji wiadomosci (propozycja)

**Nosnik: Redis Stream, jeden `XADD` = jedna operacja publikacji.**

Dlaczego stream, nie pub/sub: trwalosc (replika chwilowo odlaczona nie traci
wiadomosci), replay po restarcie, kursor/consumer-group, backpressure widoczny.
Pub/sub = fire-and-forget, odpada przy wymogu „kilka replik, niezawodnie".

```
XADD msgs:main * \
  ch      "matchA,matchB,leagueX"     # 1..N kanalow (lub aliasow) po przecinku
  data    "{\"score\":\"1:0\",\"min\":37}"   # opaque body (zwykle JSON)
  eid     "goal_42"                   # opc. = Event-Id (token wznawiania klienta)
  etype   "goal"                      # opc. = Event-Type
  ttl     "600"                       # opc. = Message-TTL (s), nadpisuje default kanalu
  startup "1"                         # opc. = Startup-Message: data to snapshot,
                                      #        nie idzie do bufora, tylko do przyszlych subskr.
```

- **Nazwy pol = 1:1 z dzisiejszymi naglowkami publishera** (`Event-Id`,
  `Event-Type`, `Message-TTL`, `Startup-Message`) → semantyka bez zmian, tylko
  transport. Kod `add_msg_to_channel` / startup-message reuzyty bez zmian logiki.
- **`ch` = lista** → „jedna wiadomosc na wiele kanalow": nginx buduje **jeden**
  `shared_raw_content` i fan-out (dokladnie dzisiejsza sciezka multi-channel z
  dedup, commit `5b06960`). Alias w `ch` rozwijany do kanonicznego id przed
  fan-outem (patrz C3 → aliasy).
- **Producent (aplikacja) = czysty `XADD`.** Zero HTTP. Wiele instancji aplikacji
  moze `XADD` rownolegle — Redis nadaje porzadek total per stream.
- **Dedup / kursor:** id wpisu streamu (`1699…-3`) jest kluczem. nginx trzyma
  „ostatnie zastosowane id per stream". Redelivery po crashu → pomija `<= last`.
  W praktyce exactly-once, w najgorszym razie at-least-once (klient i tak
  deduplikuje po `eid`).
- **Sharding:** maly staly zestaw streamow wg przeznaczenia (`msgs:main`,
  `msgs:live`, ew. `msgs:<sport>`), **nie** dynamiczny per kanal.
  **Twardy warunek: dany kanal zawsze publikowany na tym samym streamie** —
  streamy gwarantuja porzadek tylko w obrebie jednego streamu. nginx czyta
  wszystkie skonfigurowane: `XREADGROUP … STREAMS msgs:main msgs:live > >`.
- **Odpowiedz/ACK dla publishera:** brak (fire-and-XADD). Jesli aplikacja
  potrzebuje potwierdzenia „nginx dostarczyl" — osobny mechanizm, poza zakresem.

Wariant odrzucony: jeden wielki stream `msgs` na wszystko — head-of-line blocking
(jeden snapshot MB blokuje reszte) i brak izolacji porzadku.

---

### C3. Rejestr kanalow w `shm` + synchronizacja delt

**Uklad w Redis — klucze, typy, wlasciciel:**

| Klucz | Typ Redis | Wlasciciel (pisze) | Zawartosc / komendy |
|---|---|---|---|
| `ps:channels` | **SET** | aplikacja | id wszystkich kanalow. `SADD` / `SREM` / `SMEMBERS` / `SISMEMBER` / `SCARD` |
| `ps:channel:<id>` | **HASH** | aplikacja | pola = propsy: `sport`, `buffer_size`, `ttl`, `hot`, `alias_of`, `stream`, `created`… `HSET` / `HGETALL` / `HDEL` |
| `ps:channels:events` | **STREAM** | app **i** nginx (rozlaczne `op`) | delty katalogu + zdarzenia obecnosci. `XADD` / `XREAD` / `XRANGE` / `XLEN` / `XTRIM` |
| `ps:msgs:main`, `ps:msgs:live`, `ps:msgs:<shard>` | **STREAM** | aplikacja | wiadomosci do publikacji. `XADD` / `XREADGROUP` / `XACK` / `XAUTOCLAIM` |
| `ps:ch:<id>:replicas` | **SET** | nginx | id replik obslugujacych `<id>` (indeks-podpowiedz). `SADD` / `SREM` / `SMEMBERS` |
| `ps:ch:<id>:r:<replica>` | **STRING** + TTL | nginx | `"<state>:<since_ts>:<subs~>"`, `state ∈ activating\|active`. `SET … EX <2..3× heartbeat>`. **String** bo potrzebny per-klucz TTL (HASH nie ma per-pole TTL < Redis 7.4 / `HEXPIRE`) — znika sam po smierci repliki = backstop na crash |
| `ps:grp:<replica>` (grupa konsumencka na `ps:msgs:*`) | — | nginx | `XGROUP CREATE ps:msgs:main ps:grp:<replica> $ MKSTREAM` |

Opcjonalnie: `ps:nginx:<replica>:cursor:<stream>` (STRING) — kursor dla wariantu
2b bez grup; `ps:msgs:dlq` (STREAM) — dead-letter dla nieznanych kanalow.

**Zdarzenia w `ps:channels:events`** (pola wpisu): `op`, `id`, `replica`, `ts`,
opcjonalnie `props`. `op ∈`:
- `upsert` / `delete` / `reload` — app → nginx (katalog),
- `activated` — app → nginx (confirm aktywacji),
- `activate` / `deactivate` / `unknown-channel` — nginx → app (obecnosc / sygnal).

---

**Izolacja: dedykowany numer DB czy prefiks kluczy?**

| Sytuacja (zalezy od pytania #3 — topologia Redis) | Rekomendacja |
|---|---|
| **Dedykowana instancja Redis** dla push-stream | DB `0`, izolacja juz jest z granicy instancji. Prefiks `ps:` i tak zostaw (czytelnosc w `MONITOR`/`SCAN`). |
| **Wspoldzielona instancja, single-node / Sentinel** | **Dedykowany numer DB** (`SELECT <n>`, np. 3) — lekki namespace + `FLUSHDB` czysci **tylko** stan push-stream bez ruszania reszty f24 + `DBSIZE`/monitoring w zakresie. Plus prefiks `ps:`. |
| **Redis Cluster** | ⚠️ **Numery DB niedostepne** — Cluster ma tylko DB 0. Izolacja **wylacznie prefiksem** `ps:`. Do tego **hash-tag** dla kluczy, ktore musza byc w jednym slocie do `MULTI`/Lua: `ps:{<id>}:channel`, `ps:{<id>}:ch:replicas`, `ps:{<id>}:ch:r:<replica>`. **Uwaga:** `MULTI` z sekcji 4c dotyka `ps:channels` + `ps:channel:<id>` + `ps:channels:events` — trzy rozne sloty → w Cluster transakcja **nie przejdzie**; trzeba zejsc na pojedyncze komendy + idempotentny `upsert` po stronie nginx (i tak zaprojektowany). |

Numerowane DB to funkcja „w odwrocie" w ekosystemie Redis (brak w Cluster, brak
per-DB limitow pamieci/ACL, jeden wolny commit blokuje cala instancje) — daje
**namespace i scoped `FLUSHDB`**, nie prawdziwa izolacje zasobow. Dlatego:
**prefiks `ps:` zawsze** (przenosny miedzy wszystkimi wariantami), **numer DB
dodatkowo** tam gdzie nie ma Cluster. Konfigurowalny:

```nginx
push_stream_redis_address    10.0.0.5:6379;
push_stream_redis_db         3;        # SELECT po connect; 0 dla Cluster / dedyk. instancji
push_stream_redis_key_prefix "ps:";
```

**Kanaly „od razu na hot" (`hot: 1` w props):** przy bootstrapie `registry_state`
inicjowany na `ACTIVE`, handshake pierwszego subskrybenta **pominiety**, brak
`held`. Zadnego `XADD activate` — API i tak wie, ze sa aktywne (albo dostaje je
raz przy starcie repliki jednym zbiorczym zdarzeniem). Deaktywacja wylaczona
(kanal nie schodzi do `INACTIVE` mimo 0 subskrybentow). Opcjonalnie nginx
pre-warmuje ich bufor / pobiera startup-message z Redis przy starcie.

**Aliasy nazw (`alias_of: <kanoniczny_id>`):** ✅ mozliwe, tanio. Alias **nie**
dostaje wlasnej struktury kanalu — to czysta rezolucja nazwy w warstwie rejestru:
nginx buduje w `shm` mape `alias → kanoniczny id`. Rozwijana:
- przy subskrypcji (`GET /sub/<alias>` → subskrypcja na kanonicznym; `channel->id`
  w szablonach = kanoniczny, chyba ze chcemy zachowac zadana nazwe do wyswietlenia),
- przy publikacji (`ch` we wpisie streamu moze zawierac alias) — przed fan-outem,
- `Last-Event-Id` / catch-up / statystyki — wszystko per kanoniczny (alias
  raportowany pod kanonicznym).

Wiele aliasow → jeden kanal: OK. Alias do aliasu: zabroniony (walidacja przy
`upsert`, tylko jeden poziom). Ryzyko: nikle — brak dodatkowych liczników/struktur.

**W nginx:**

- **Bootstrap:** przy starcie workera (lub raz na replike, wynik w `shm`) —
  `SCAN`/`SMEMBERS channels` + `HGETALL channel:<id>` → wypelnienie rejestru w
  `shm`. Rejestr: nowe pole na strukturze kanalu (`registered`, `props`,
  `registry_state`) **albo** osobny rbtree id→wpis. Rejestr lezy w `shm` i
  przezywa reload (jak `startup_message`).
- **Delty:** ciagly `XREAD BLOCK` na `channels:events` od ostatniego zastosowanego
  id (kursor w `shm`). `upsert` → utworz/zaktualizuj wpis + props; `delete` →
  oznacz nieistniejacy, rozlacz subskrybentow (jest juz `channel_destroyed` +
  `push_stream_channel_inactivity_delete_with_subscribers`).
- **Bez pollingu** — zgodnie z wymaganiem.

**Rozjazd bootstrap vs delty:** zapamietac `LASTID` streamu *przed* bootstrapem,
potem odtwarzac delty od tego id → brak luki, idempotentne `upsert`.

**Wykonalnosc:** ✅ realne. Koszt: pamiec `shm` na rejestr (przy dużej liczbie
kanalow — patrz „Pytania otwarte", pkt 8). Ryzyko: spojnosc przy reloadzie z inna
konfiguracja (juz rozwiazane wzorcowo dla szablonow wiadomosci — fallback do
surowej tresci).

### C4. Bramka tworzenia kanalow

Dzis `subscriber_prepare_request` / `publisher_handler` wolaja `get_channel()`
(tworzy). Zmiana: **rozstrzyganie „czy kanal istnieje" dwustopniowo:**

1. jest w rejestrze `shm`? → tak: dalej; nie: krok 2,
2. `HGETALL channel:<id>` w Redis (zrodlo prawdy) — istnieje? → dociagnij do `shm`
   i dalej; nie: **`403` (sub) / skip (pub)**, `get_channel()` **nie** wolane.

Krok 2 lapie wyscig „`msgs:*`/subskrypcja wyprzedzila `channels:events`" i wolna
replike — bez niego nowo utworzony kanal bylby przez chwile „nieistniejacy".
Reuzyc flage w duchu `authorized_channels_only`, ale z warunkiem „jest w rejestrze
(shm ∪ Redis)" zamiast „ma `stored_messages > 0`".

Miejsca do dotkniecia:
`ngx_http_push_stream_subscriber.c:517` (find), `:557` (get — do zablokowania),
`ngx_http_push_stream_publisher.c:100`/`:110`.

**Wykonalnosc:** ✅ proste, punktowe.

### C5. Maszyna stanu aktywacji + wstrzymanie subskrybenta

**Najtrudniejszy komponent — dotyka goracej sciezki dostarczania.**

Stan per kanal w `shm`: `registry_state ∈ { INACTIVE, ACTIVATING, ACTIVE, DEACTIVATING }`.

**Zasada powiadamiania — NIE co subskrybent, tylko na krawedziach 0↔1:**

nginx powiadamia API **wylacznie** przy zmianie „na tej replice sa / nie ma
subskrybentow" — czyli `0 → 1` (`activate`) i `1 → 0` (`deactivate`, po grace).
Wejscie 2., 3., … 10 000. subskrybenta i wyjscie kazdego z nich **nie generuje
zadnego zdarzenia ani zapisu do Redis** (poza opcjonalnym odswiezeniem
przyblizonej liczby przy heartbeacie — patrz nizej). To jest caly sens tego
projektu: zero write-storm / pollingu przy ruchu userow.

**Obecnosc trzymana per replika, przypisana do kanalu** (zgodnie z uwaga —
`ch:<id>:r:<replica>`, patrz uklad w C3):

```
assing_subscription_to_channel: channel->subscribers 0 → 1
  pod channel->mutex:
    if registry_state == INACTIVE:
        registry_state = ACTIVATING           ← CAS: tylko pierwszy przechodzi
        zaplanuj (C6):
          SADD ch:<id>:replicas <replica>
          SET  ch:<id>:r:<replica> "activating:<ts>:1"  EX <2..3× heartbeat>
          XADD channels:events {op:activate, id, replica, ts}
        subskrybent oznaczony `held = 1`
  → „dokladnie raz" gwarantuje CAS pod channel->mutex
    (miedzy workerami tej samej repliki; miedzy replikami — kazda liczy siebie,
     pytanie #1)
```

`channel->subscribers` (licznik w `shm`, na cala replike) sluzy tylko do
**wykrycia krawedzi** `0↔1` — nie jest wypychany do Redis przy kazdej zmianie.

**Subskrybent w stanie `held`:**
- polaczenie utrzymane (naglowki HTTP/handshake WS wyslane — inaczej klient widzi
  zwloke w nawiazaniu),
- **zero danych aplikacyjnych** do usera: `respond_to_subscribers` / startup-message
  / catch-up **pomijaja** subskrybentow `held`,
- wiadomosci live i tak trafiaja do bufora `shm` (`add_msg_to_channel` zapisuje),
  wiec po zwolnieniu nie ma luki — robimy catch-up z bufora.

**`confirm` z API:** wpis `channels:events {op: activated, id}`. Konsument delt (C3):

```
registry_state ACTIVATING → ACTIVE
SET ch:<id>:r:<replica> "active:<ts>:<subs>"  EX <2..3× heartbeat>    (C6)
dla kazdego subskrybenta held na tym kanale (na tym workerze):
    held = 0
    wyslij startup-message + catch-up z bufora (istniejaca maszyneria)
    → od teraz normalny live
```

Miedzy workerami: potrzebny alert IPC „zwolnij held na kanale X" (rozszerzenie
`ngx_http_push_stream_alert_worker_*`, dzis jest 4 komendy — dodajemy 5.).

**Timeout potwierdzenia:** brak `confirm` w `push_stream_redis_activation_timeout`
(np. 5 s) → **decyzja produktowa** (pytanie #2): fail-open (zwolnij i tak + log)
albo trzymaj dalej / rozlacz. Rekomendacja: fail-open + metryka, zeby zawieszone
API nie blokowalo userow.

**Deaktywacja — „wszyscy poszli" (lustro aktywacji):**

Tak, pilnowane — i tez **tylko na krawedzi `1 → 0`**, nie co wychodzacy subskrybent.
`channel->subscribers` to licznik **na cala replike** (w `shm`, wspolny dla
workerow repliki), dekrementowany pod `channel->mutex` w `worker_subscriber_cleanup`
(`utils.c:2469`), w sciezce IPC (`utils.c:512`) i przy WS `-channel`.

```
dekrement channel->subscribers, pod channel->mutex:
  if subscribers == 0 && registry_state == ACTIVE && !hot:
      registry_state = DEACTIVATING
      channel->deactivate_at = now + push_stream_redis_deactivation_delay   (grace)

okresowy sweep (piggyback na collect_expired_..._empty_channels, bez per-kanal timera):
  if registry_state == DEACTIVATING && subscribers == 0 && deactivate_at <= now:
      registry_state → INACTIVE                    ← CAS pod mutexem: „raz"
      DEL  ch:<id>:r:<replica>
      SREM ch:<id>:replicas <replica>
      XADD channels:events {op:deactivate, id, replica, ts}

subskrybent wraca w oknie grace (dowolny worker, stan widoczny w shm):
  assing_subscription_to_channel widzi DEACTIVATING → z powrotem ACTIVE,
  bez XADD, bez held (kanal dla API caly czas byl aktywny) → brak flappingu
```

- **Dokladnie raz na cykl** — CAS `ACTIVE→DEACTIVATING→INACTIVE` pod `channel->mutex`.
  Kolejne wejscie/wyjscie subskrybentow nie generuje kolejnych `deactivate`, dopoki
  nie wroci pelny cykl przez `ACTIVE`.
- **Grace period** (`deactivation_delay`, np. 10–30 s) tlumi migotanie przy szybkim
  reconnekcie klienta — analogicznie do dzisiejszego `channel_inactivity_time`.
- **Stale timer bezpieczny** — sweep zawsze re-sprawdza `subscribers == 0 &&
  state == DEACTIVATING` pod mutexem, wiec spozniony przebieg po prostu nic nie robi.
- **Zakres = per replika** (jak aktywacja, pytanie #1): kazda replika osobno mowi
  „u mnie wszyscy poszli". API sumuje po replikach.

**Heartbeat + backstop na crash repliki:** jesli cala replika padnie, subskrybenci
znikaja **bez** `XADD deactivate` — API myslaloby, ze kanal wiecznie aktywny na tej
replice. Dlatego wpis `ch:<id>:r:<replica>` ma **TTL** (`EX 2..3× heartbeat`),
a nginx w jednym okresowym przebiegu (co `push_stream_redis_heartbeat_interval`,
pipeline dla wszystkich swoich aktywnych kanalow):

```
dla kazdego kanalu ACTIVE na tej replice:
    SET ch:<id>:r:<replica> "active:<since>:<subs_przyblizone>"  EX <2..3× hb>
    (subs_przyblizone = biezacy odczyt channel->subscribers z shm — TANI,
     bez pushowania przy kazdej zmianie)
```

Smierc repliki → wszystkie jej wpisy `ch:*:r:<replica>` wygasaja → API widzi
zejscie bez zadnego zdarzenia. Stream `{op:deactivate}` = szybka sciezka (sekundy),
TTL = gwarancja ostateczna (kilkadziesiat s). API ma pelny obraz z:
`SMEMBERS ch:<id>:replicas` + `MGET ch:<id>:r:<r1> ch:<id>:r:<r2> …` (albo
`SCAN ch:<id>:r:*`) → ktore serwery, w jakim stanie, ile mniej wiecej userow.

**Deaktywacja ≠ usuniecie z rejestru.** „Wszyscy poszli" → kanal nadal istnieje na
liscie, tylko `INACTIVE`. Usuniecie (`{op:delete}`) to osobna, swiadoma decyzja
aplikacji (patrz 4b pkt 1).

**Wykonalnosc:** ⚠️ realne, ale to **glowne zrodlo ryzyka**:
- modyfikacja hot-path fan-out (`respond_to_subscribers`) o warunek `held`,
- wspolbieznosc: CAS stanu, IPC „release", race „confirm przychodzi zanim
  wszystkie workery zarejestrowaly held",
- edge: subskrybent rozlacza sie w trakcie `ACTIVATING` (ostatni held odchodzi
  zanim przyszedl `confirm`) → po grace `DEACTIVATING`; ale trzeba zdecydowac czy
  wysylamy `{op:deactivate}` skoro API moze wlasnie przetwarza `activate`
  (sekwencja `activate`→`deactivate` musi byc dla API jednoznaczna — stad `replica`
  + monotoniczny `ts`/id wpisu w kazdym zdarzeniu),
- reload w trakcie `ACTIVATING` (stan w `shm` przezywa; held-flagi na subskrybentach
  starych workerow gina razem z polaczeniami — nowe polaczenia ida od nowa).

### C6. Producent zdarzen nginx → Redis

Trzy rodzaje zapisu, wszystkie pipeline, nieblokujaco:

| Kiedy | Komendy | Czestotliwosc |
|---|---|---|
| krawedz `0→1` (aktywacja) | `SADD ch:<id>:replicas` + `SET ch:<id>:r:<replica> …EX` + `XADD …activate` | rzadko (raz na cykl kanalu) |
| krawedz `1→0` (deaktywacja, po grace) | `DEL ch:<id>:r:<replica>` + `SREM ch:<id>:replicas` + `XADD …deactivate` | rzadko |
| heartbeat | `SET ch:<id>:r:<replica> …EX` dla wszystkich ACTIVE kanalow repliki (jeden pipeline) | co `heartbeat_interval` |

**Nigdy** zapisu per subskrybent. Gdy Redis wolny/niedostepny: **bounded queue** +
polityka (drop najstarsze + licznik / retry z backoff). Heartbeat, ktory nie
zdazyl → wpisy wygasna po TTL, API zobaczy „replika niepewna" — to jest
akceptowalny, bezpieczny tryb degradacji.

**Rozdzial wlasnosci kluczy (zero konfliktu zapisu):**
- **aplikacja** pisze wylacznie `channels` + `channel:<id>` (katalog, props),
- **nginx** pisze wylacznie `ch:<id>:replicas` + `ch:<id>:r:<replica>` (obecnosc),
- `channels:events` — obie strony `XADD`, ale **rozlaczne `op`** (app: `upsert`/
  `delete`/`activated`; nginx: `activate`/`deactivate`).

Zaden klucz nie ma dwoch pisarzy → brak race'ow, brak potrzeby `WATCH`/Lua.

**Wykonalnosc:** ✅ realne, uwaga na backpressure.

### C7. Konfiguracja, statystyki, testy, dokumentacja

Nowe dyrektywy (szkic):

```nginx
push_stream_redis_address           127.0.0.1:6379;   # lub lista / sentinel
push_stream_redis_password          <secret>;
push_stream_redis_db                3;                 # SELECT po connect; 0 dla Cluster
push_stream_redis_key_prefix        "ps:";             # przed kazdym kluczem
push_stream_redis_replica_id        "fe-1";            # tozsamosc repliki (replica w kluczach/zdarzeniach)
push_stream_redis_message_streams    msgs:main msgs:live;   # jedna lub wiele (bez prefiksu)
push_stream_redis_channels_key       channels;
push_stream_redis_events_stream      channels:events;
push_stream_redis_consumer_group     grp:fe-1;              # wariant 2a
push_stream_redis_activation_timeout  5s;    # brak confirm → fail-open (pyt. #2)
push_stream_redis_deactivation_delay  15s;   # grace „wszyscy poszli" przed XADD deactivate
push_stream_redis_heartbeat_interval  10s;   # odswiezanie ps:ch:<id>:r:<replica> (backstop na crash)
push_stream_redis_reconnect_interval  1s;    # backoff do max
```

- `/channels-stats` rozszerzyc o: stan Redis (connected/lag), `registry_state`
  per kanal, kursory streamow, liczniki `skipped_unknown_channel`,
  `activation_pending`, `activation_timeout`.
- Testy: suite Ruby (`misc/spec`) + osobny zestaw integracyjny z realnym Redis
  (docker) + wstrzykiwanie awarii sieci.
- `/pub` — zostawic opcjonalnie jako `push_stream_publisher admin` do debugu/testow
  czy usunac zupelnie? (pytanie #5).

---

## 4a. Restart nginx a bufor wiadomosci

Dwa rozne przypadki — dzis i po zmianie:

| Zdarzenie | `shm` (bufor + kanaly + startup-msg) | Dzis | Po Redis |
|---|---|---|---|
| **`nginx -s reload`** | **zachowany** — `init_shm_zone` wykrywa istniejaca strefe (`data != NULL`, `setup.c:1197`) i reuzuje ja; kanaly/bufory/startup-msg przezywaja | bufor OK, klient wznawia z `Last-Event-Id` normalnie | bez zmian — dodatkowo kursory streamow w `shm` tez przezyja |
| **Pelny restart** (`systemctl restart`, recreate kontenera, `stop`+`start`) | **zniszczony** — segment `shm` kasowany i tworzony od zera; **bufor tracony** | **bufor przepada.** Klient laczy sie z `Last-Event-Id` → `unknown event id` / brak historii, luka w danych | **bufor odtwarzany ze streamu** |

**Odpowiedz na pytanie: po pelnym restarcie nginx NIE odczyta bufora z `shm`
(bo go nie ma), ale przy transporcie Redis odbudowuje go czytajac `msgs:*` od
zapamietanego kursora.** To jest **wyrazna poprawa wzgledem dzis.**

Gdzie kursor przezywa restart:
- **2a (consumer groups) — zalecane:** last-delivered-id grupy trzyma **Redis**.
  Restart → `XREADGROUP … >` kontynuuje automatycznie; `XAUTOCLAIM` odbiera wpisy
  dostarczone-ale-nie-`XACK` sprzed crasha. Zero pracy po stronie nginx.
- **2b (kursor w `shm`):** ginie z restartem → nginx musialby okresowo / przy
  shutdownie zrzucac `SET nginx:<replika>:cursor:<stream> <id>` i czytac przy
  starcie. Dziala, ale to reczne trzymanie stanu.

**Warunki, zeby odtworzenie bylo bezstratne:**
1. Retencja streamow `msgs:*` (`MAXLEN ~`/`MINID`) > maks. przewidywany downtime
   repliki + okno reconnectu klientow. Za krotka retencja → luka mimo wszystko.
2. `startup-message` po pelnym restarcie **znika z `shm`** — musi byc ponownie
   opublikowany przez API (albo nginx pobiera per-kanal snapshot z Redis przy
   (re)aktywacji kanalu). Do ustalenia w kontrakcie z API.
3. Subskrybenci i tak reconnectuja z wlasnym `Last-Event-Id` → dostaja catch-up
   z (juz odbudowanego) bufora. Bez luki, o ile pkt 1 spelniony.

---

## 4b. Ochrona list przed przepelnieniem

Cztery „listy" rosna niezaleznie — kazda ma inna strategie:

### 1. Rejestr kanalow (`channels` + `channel:<id>`)

Rosnie, gdy aplikacja tworzy kanaly i nie kasuje (mecz sie konczy, kanal zostaje).

| Warstwa | Mechanizm |
|---|---|
| **Wlascicielstwo = aplikacja** (podstawa) | App tworzy i **kasuje** kanaly. Koniec meczu + grace → `XADD channels:events {op:delete}`. „Kanal spoza listy nie istnieje" ⇒ tylko app moze go usunac — nginx nie jest wlascicielem. |
| **TTL + reaper na `channel:<id>`** (backstop) | `EXPIRE channel:<id>` odswiezany przy aktywnosci; `channel:<id>` = zrodlo prawdy o istnieniu. `channels` SET to indeks — okresowy job `SCAN channels` → jak `channel:<id>` nie istnieje → `SREM` + `{op:delete}`. Lapie kanaly, ktore app „zapomniala". |
| **Twardy limit w Redis** | `op:upsert` nowego kanalu przy `SCARD channels >= max` → odrzucenie (blad do aplikacji). Ew. limity per-kategoria/dyscyplina. |
| **Twardy limit w nginx** (ostatnia linia) | `push_stream_max_number_of_channels` juz istnieje (`NGX_HTTP_PUSH_STREAM_NUMBER_OF_CHANNELS_EXCEEDED`). Ponad limit nginx **nie rejestruje** + metryka `registry_rejected`. Chroni `shm` niezaleznie od stanu Redis. |
| **nginx jako sygnalizator GC** | nginx juz wykrywa „0 subskrybentow + brak wiadomosci przez `channel_inactivity_time`" → `XADD {op:deactivate}`. Moze tez emitowac `{op:gc-candidate}` po dluzszym czasie — ale **decyzje o usunieciu podejmuje app**. |

### 2. Stream wiadomosci `msgs:*`

- **`XADD ... MAXLEN ~ N`** przy kazdej publikacji (obowiazek producenta), ew.
  okresowy `XTRIM MINID ~ <ts>`. `~` = trim przyblizony (tanszy).
- Rozmiar `N` / okno `MINID` = **max downtime repliki + okno reconnectu klienta**
  (patrz 4a pkt 1). Za maly → luka przy restarcie repliki.
- Monitoring: `XLEN`, `XINFO GROUPS` → `lag` per grupa-repliki. Alert gdy
  konsument zostaje w tyle (wolna replika / zerwany klient Redis).

### 3. Stream delt `channels:events`

- Tez **`MAXLEN ~`**, ale **mozna trimowac agresywnie** — bo pelny stan zawsze
  odtwarzalny z `channels`/`channel:<id>` (bootstrap-snapshot).
- Replika offline dluzej niz retencja: wykrywa `moj LASTID < najstarszy id w
  streamie` → robi **pelny re-bootstrap** zamiast aplikowac delty (czysty
  fallback), zamiast zgubic zmiany po cichu.

### 4. `shm` nginx (rejestr + bufory w pamieci)

- `push_stream_shared_memory_size` — staly; brak miejsca → `ngx_slab_alloc failed:
  no memory`, **bez crasha** (jak dzis dla startup-message).
- `push_stream_max_number_of_channels`, `push_stream_max_messages_stored_per_channel`,
  `push_stream_message_ttl` — ograniczaja odpowiednio liczbe kanalow i bufor/kanal.
- Props kanalu: **budzet na rozmiar** (cap na blob props, odrzuc nadmiar). Mapa
  aliasow: tez cap.
- `XADD` z nginx (aktywacje/deaktywacje): **bounded queue** w workerze/`shm` +
  coalescing (nie deaktywuj od razu przy 0 subskr. — czekaj `inactivity_time`;
  CAS stanu gwarantuje 1 `activate` na cykl). Gdy Redis wolny: drop najstarsze +
  licznik, nigdy nieograniczony wzrost.
- Monitoring przez `/channels-stats`: wykorzystanie `shm`, liczba kanalow,
  liczniki odrzucen.

**Zasada przekrojowa:** limity Redis chronia Redis, limity nginx chronia `shm` —
**obie warstwy niezaleznie**, zeby awaria/przepelnienie jednej nie przewrocila
drugiej. Przekroczenie = odrzucenie + metryka, nigdy crash.

---

## 4c. Kontrakt API ↔ Redis — przyklady komend

Pelny wykaz pol kazdej struktury: **sekcja 4d**. Ponizej przyklady operacji.

Skladnia `redis-cli`. Przyklady bez prefiksu `ps:` i bez `SELECT <db>` dla
zwiezlosci — w praktyce klient robi `SELECT <db>` po connect i kazdy klucz ma
prefiks (`ps:channels`, `ps:channel:<id>`, …); patrz „Izolacja" w C3.
Wszystkie operacje katalogu owija sie w `MULTI/EXEC`, zeby nginx nigdy nie
zobaczyl zdarzenia `channels:events` przed danymi, ktorych dotyczy (w Redis
Cluster — patrz C3 — `MULTI` przez rozne sloty odpada, zejdz na pojedyncze
komendy + idempotentny `upsert`).

### Utworzenie / ustawienie kanalu (upsert)

```redis
MULTI
HSET  channel:match:12345  sport football  buffer_size 50  ttl 600  hot 0  created 1757320000
SADD  channels  match:12345
XADD  channels:events  *  op upsert  id match:12345
EXEC
```

- `HSET` — propsy (pola dowolne wg kontraktu; patrz pytanie #9 — ktore nginx honoruje).
- `SADD channels` — dopisanie do listy „wszystkich kanalow".
- `XADD channels:events {op:upsert}` — delta dla nginx (bez pollingu). nginx po
  odbiorze robi `HGETALL channel:match:12345` i wpisuje/aktualizuje rejestr w `shm`.
  (Wariant: wsadzic propsy inline we wpisie streamu — mniej RTT, ale limit rozmiaru
  wpisu i duplikacja; domyslnie `HGETALL`.)

**Kanal „hot" (aktywny od startu, bez handshake):**
```redis
MULTI
HSET  channel:ticker:live  hot 1  sport all
SADD  channels  ticker:live
XADD  channels:events  *  op upsert  id ticker:live
EXEC
```

**Alias:**
```redis
MULTI
HSET  channel:epl:12345  alias_of match:12345
SADD  channels  epl:12345
XADD  channels:events  *  op upsert  id epl:12345
EXEC
```

**Wsadowe zaladowanie calej listy** (np. po czyszczeniu Redis) — pipeline bez
zdarzen per kanal; nginx i tak robi pelny bootstrap (`SMEMBERS channels` +
`HGETALL`) przy starcie / po wykryciu luki:
```redis
SADD channels  match:1 match:2 match:3 ...
HSET channel:match:1  sport football  ...
HSET channel:match:2  sport football  ...
XADD channels:events  *  op reload            # opcjonalny „przeladuj wszystko"
```

### Modyfikacja kanalu

Ta sama komenda co utworzenie — `upsert` = insert-or-update:
```redis
MULTI
HSET  channel:match:12345  buffer_size 100  ttl 1200
XADD  channels:events  *  op upsert  id match:12345
EXEC
```

Usuniecie pojedynczego propsa:
```redis
MULTI
HDEL  channel:match:12345  ttl
XADD  channels:events  *  op upsert  id match:12345
EXEC
```

nginx po `upsert`: nadpisuje propsy w rejestrze `shm`. Co honoruje „w locie"
(`buffer_size`, `ttl`) a co wymaga cyklu deaktywacja→aktywacja — pytanie #9.

### Skasowanie kanalu

```redis
MULTI
SREM  channels  match:12345
DEL   channel:match:12345
XADD  channels:events  *  op delete  id match:12345
EXEC
```

nginx po `delete`:
1. rozlacza subskrybentow (istniejacy `channel_destroyed` + `channel_deleted_message`),
2. usuwa kanal z rejestru `shm`, czysci bufor,
3. sprzata **swoje** klucze obecnosci: `DEL ch:match:12345:r:<replica>`,
   `SREM ch:match:12345:replicas <replica>` (albo zostawia — i tak wygasna po TTL).

Aliasy wskazujace na skasowany kanal: API kasuje je osobno (jak wyzej, po `id`
aliasu); nginx napotkany osierocony alias loguje i pomija.

### Wiadomosc na stream dla kanalu, ktorego nie ma

```redis
XADD msgs:main  *  ch match:99999  data '{"score":"1:0"}'  eid e1
```

nginx czyta wpis, dla kazdego id z `ch` sprawdza rejestr:

- **`match:99999` nie ma w rejestrze `shm`** → sprawdza `źrodlo prawdy`:
  `HGETALL channel:match:99999` (jeden RTT).
  - **istnieje w Redis, brak w `shm`** (wyscig: `msgs:*` wyprzedzil
    `channels:events`, albo wolna replika) → nginx **dociaga** kanal do rejestru
    `shm` i **dostarcza** wiadomosc. „Kanal spoza listy nie istnieje" = spoza listy
    **w Redis**, nie spoza cache'u. Wyscig miedzy dwoma streamami rozwiazany.
  - **nie istnieje tez w Redis** → wiadomosc **pominieta**:
    `ngx_http_push_stream_add_msg_to_channel` NIE wolane, `skipped_unknown_channel++`,
    log `WARN`, opcjonalnie `XADD channels:events * op unknown-channel id match:99999`
    (sygnal do API, ze ktos publikuje w prozne). **Kanal NIE jest tworzony.**
    Publisher nie dostaje bledu (fire-and-forget).
- **Kursor / `XACK` przesuwany normalnie** — nieznany kanal nie zawiesza
  konsumpcji streamu.
- **`ch` z wieloma kanalami** (`match:12345,match:99999`): znane dostaja
  wiadomosc, nieznane pominiete — czesciowa dostawa + log.

---

## 4d. Schemat danych — pola kazdej struktury

Legenda „pisze": **A** = aplikacja, **N** = nginx. „wym." = wymagane.

### `ps:channels` — SET

| Element | Typ wartosci | Pisze | Opis |
|---|---|---|---|
| *member* | string | A | id kanalu — **kanoniczne oraz aliasy** (obie postaci sa „istniejacym id" dla bramki C4). Ograniczenia: dlugosc ≤ `push_stream_max_channel_id_length`; bez `,` (separator w polu `ch`), bez bialych znakow, bez prefiksu wildcard (`broad_`). |

### `ps:channel:<id>` — HASH (propsy kanalu)

| Pole | Typ | Pisze | wym. | Opis / uzycie w nginx |
|---|---|---|---|---|
| `alias_of` | string (id) | A | — | jesli ustawione → wpis jest **aliasem**, wartosc = id kanoniczne. Brak/`""` → kanal realny. Jeden poziom (alias→alias zabroniony). |
| `stream` | string | A | — | ktory `ps:msgs:<shard>` niesie wiadomosci tego kanalu. **Musi byc stabilny** (porzadek). Default `msgs:main`. |
| `type` | string | A | — | taksonomia app: `match`\|`league`\|`ticker`\|`system`… (nginx ignoruje). |
| `hot` | `0`\|`1` | A | — | `1` → kanal `ACTIVE` od bootstrapu, **bez handshake**, bez deaktywacji. |
| `buffer_size` | int | A | — | nadpisuje `push_stream_max_messages_stored_per_channel` dla tego kanalu. |
| `message_ttl` | int (s) | A | — | nadpisuje `push_stream_message_ttl` (mapuje na istniejace `channel->message_ttl`). |
| `activation` | enum | A | — | `handshake` (default) \| `none` (emituj `activate`, nie czekaj na confirm) \| `hot`. |
| `startup_from_redis` | `0`\|`1` | A | — | przy (re)aktywacji nginx pobiera snapshot z `ps:channel:<id>:startup` i instaluje jako startup-message. |
| `rev` | int | A | — | monotoniczna rewizja propsow — idempotencja `upsert` (nginx stosuje tylko `rev` wyzsze). |
| `created` | unix ts | A | — | metadana; nginx ignoruje. |
| `updated` | unix ts | A | — | ostatnia zmiana propsow. |
| `expires_at` | unix ts | A | — | podpowiedz dla reapera (twarde usuniecie i tak = jawny `delete`). |
| `sport` / `match_id` / `league_id` / `season` | string | A | — | domenowe klucze biznesowe app. |
| `label` | string | A | — | nazwa czytelna dla dashboardow ops. |
| `meta` | JSON string | A | — | worek na dowolne dane app — nginx **nigdy** nie parsuje. |

### `ps:channel:<id>:startup` — HASH (opcjonalne, snapshot stanu)

| Pole | Typ | Pisze | Opis |
|---|---|---|---|
| `body` | bytes/JSON | A | tresc wiadomosci startowej (snapshot). |
| `eid` | string | A | Event-Id snapshotu = granica catch-up (semantyka jak `Last-Event-Id`, patrz `nginx-push-stream-startup-message.md`). |
| `etype` | string | A | Event-Type snapshotu. |
| `updated` | unix ts | A | kiedy snapshot zbudowano. |

### `ps:channels:events` — STREAM (delty + zdarzenia obecnosci)

Pola **wspolne** kazdego wpisu:

| Pole | Typ | Opis |
|---|---|---|
| `op` | enum | `upsert`\|`delete`\|`reload` (A→N) · `activated` (A→N) · `activate`\|`deactivate`\|`unknown-channel` (N→A) |
| `id` | string | id kanalu (brak dla `reload`) |
| `ts` | unix ms | zegar producenta (porzadek autorytatywny = id wpisu streamu) |
| `src` | `app`\|`nginx` | kto wyemitowal |

Pola **zalezne od `op`**:

| `op` | Dodatkowe pola | Kierunek | Znaczenie |
|---|---|---|---|
| `upsert` | `props` (JSON, opc.), `rev` (int) | A→N | kanal utworzony/zmieniony; `props` inline oszczedza `HGETALL`, inaczej nginx dociaga |
| `delete` | `reason` (`match_finished`\|`manual`\|`gc`\|`ttl`) | A→N | kanal usuniety z katalogu → rozlacz subskrybentow, wyczysc `shm` |
| `reload` | — | A→N | „przeladuj caly katalog" (pelny bootstrap `SMEMBERS`+`HGETALL`) |
| `activate` | `replica`, `since` (ts) | N→A | pierwszy subskrybent na tej replice — **czeka na `activated`** |
| `activated` | `replica` (lub `*`), `startup_rev` (opc.) | A→N | confirm — nginx zwalnia `held`, przechodzi `ACTIVE` |
| `deactivate` | `replica`, `subs_peak` (int, opc.), `duration` (s, opc.) | N→A | ostatni subskrybent wyszedl (po grace) |
| `unknown-channel` | `replica`, `stream`, `count` (int), `sample_eid` | N→A | ktos publikuje do nieistniejacego kanalu (sygnal diagnostyczny, moze byc coalesced) |

### `ps:msgs:<shard>` — STREAM (wiadomosci do publikacji)

| Pole | Typ | Pisze | wym. | Opis / mapowanie |
|---|---|---|---|---|
| `ch` | string | A | ✅ | 1..N id kanalow/aliasow po przecinku (`matchA,matchB`) |
| `data` | bytes/JSON | A | ✅ | tresc wiadomosci (opaque) |
| `eid` | string | A | — | = `Event-Id` (token wznawiania klienta + granica catch-up) |
| `etype` | string | A | — | = `Event-Type` (nazwa eventu SSE / token `~event-type~`) |
| `ttl` | int (s) | A | — | = `Message-TTL` — override TTL tej wiadomosci |
| `startup` | `0`\|`1` | A | — | `1` → zapis jako startup-message (nie do bufora, nie broadcast) |
| `pub_ts` | unix ms | A | — | znacznik publishera — metryki latencji |
| `src` | string | A | — | ktora instancja/podsystem app opublikowal — diagnostyka |
| `key` | string | A | — | klucz idempotencji, jesli app chce dedupu ponad id wpisu streamu |
| *id wpisu* | `<ms>-<seq>` | Redis | — | **porzadek + kursor dedupu** (nginx trzyma „ostatnie zastosowane") |

### `ps:ch:<id>:replicas` — SET (indeks replik obslugujacych kanal)

| Element | Typ | Pisze | Opis |
|---|---|---|---|
| *member* | string | N | id repliki (`fe-1`, `edge-waw-3`). `SADD` przy `0→1`, `SREM` przy czystym `1→0`. **Indeks-podpowiedz** — prawda = istnienie `ps:ch:<id>:r:<replica>` (czlonek moze byc nieaktualny po crashu → reconcile). |

### `ps:ch:<id>:r:<replica>` — STRING + TTL (obecnosc jednej repliki na kanale)

Wartosc = pakowany string `state:since:subs` (albo maly JSON). TTL = `2..3 ×
heartbeat_interval`, odswiezany `SET … EX` co heartbeat.

| „Pole" (segment) | Typ | Pisze | Opis |
|---|---|---|---|
| `state` | `activating`\|`active` | N | faza — `activating` = czeka na `activated`; `active` = zwolniony |
| `since` | unix ts | N | kiedy ta replika aktywowala kanal |
| `subs` | int | N | **przyblizona** liczba subskrybentow na tej replice (odczyt `channel->subscribers` z `shm` przy heartbeacie — nie per-subskrybent) |
| `hb` | unix ts | N | (opc.) ostatni heartbeat — redundantne z TTL, wygodne dla dashboardu |
| `workers` | int | N | (opc.) ilu workerow nginx trzyma subskrybentow — diagnostyka |

### `ps:nginx:<replica>:cursor` — HASH (tylko wariant 2b, bez consumer groups)

| Pole | Typ | Pisze | Opis |
|---|---|---|---|
| `msgs:main` | stream-id | N | ostatni przetworzony wpis tego streamu |
| `msgs:live` | stream-id | N | j.w. per shard |
| `channels:events` | stream-id | N | ostatnia zastosowana delta katalogu |

*(Wariant 2a: kursor trzyma Redis w consumer-grupie — `XINFO GROUPS ps:msgs:main`
→ `last-delivered-id`, `pending`, `lag`. Ten klucz wtedy nie istnieje.)*

### `ps:msgs:dlq` — STREAM (opcjonalny dead-letter)

| Pole | Typ | Pisze | Opis |
|---|---|---|---|
| `ch` / `data` / `eid` | — | N | kopia oryginalnych pol wpisu |
| `reason` | enum | N | `unknown_channel` \| `add_msg_failed` \| … |
| `replica` | string | N | ktora replika odrzucila |
| `orig_id` | stream-id | N | id wpisu w zrodlowym `ps:msgs:<shard>` |
| `ts` | unix ms | N | kiedy trafilo do DLQ |

### `ps:stats:<replica>` — HASH (opcjonalny zrzut dla fleet-dashboardu)

| Pole | Typ | Pisze | Opis |
|---|---|---|---|
| `channels_active` | int | N | kanaly w stanie `ACTIVE` na tej replice |
| `subscribers_total` | int | N | `data->subscribers` |
| `skipped_unknown` | int | N | licznik wiadomosci do nieistniejacych kanalow |
| `redis_reconnects` | int | N | ile razy klient Redis sie wznawial |
| `shm_used_pct` | int | N | wykorzystanie `push_stream_shared_memory_size` |
| `updated` | unix ts | N | znacznik zrzutu |

---

## 5. Szacunek nakladu i ryzyka

| Komponent | Naklad | Ryzyko |
|---|---|---|
| C1 klient RESP async + reconnect | 1–2 tyg | średnie (poprawnosc event-model) |
| C2 konsument streamow → add_msg_to_channel | ~1 tyg | niskie |
| C3 rejestr w shm + bootstrap + delty | 1–2 tyg | średnie (pamiec, spojnosc) |
| C4 bramka tworzenia kanalow | 2–3 dni | niskie |
| C5 maszyna stanu aktywacji + hold + IPC release | 2–3 tyg | **wysokie** (hot-path, wspolbieznosc) |
| C6 producent XADD + backpressure | 3–5 dni | średnie |
| C7 config + stats + testy + docs | 1–2 tyg | średnie |
| **Razem** | **~7–11 tyg** do wersji produkcyjnej; **PoC ~2–3 tyg** | — |

**Ryzyka przekrojowe:**

1. **Async Redis w modelu nginx** — partial read, backpressure `XADD`, utrata
   polaczenia w trakcie komendy, `XREAD BLOCK` a `nginx -s reload`.
2. **Backpressure**: tempo publikacji > tempo dostarczania → presja na bufor `shm`
   (chunked, jest cleanup — ale trzeba zmierzyc). `XADD` z nginx gdy Redis wolny.
3. **Head-of-line blocking** na jednym streamie — mitygacja: wiele streamow +
   `COUNT` + pipeline.
4. **Redis failover** (Sentinel/Cluster): modul musi podazyc za primary; Cluster =
   obsluga `MOVED`/`ASK`. Prawdopodobnie celujemy w single primary + Sentinel
   (pytanie #3).
5. **Latencja aktywacji** — user widzi „polaczony, cisza" do `confirm`. Potrzebny
   timeout + jasny kontrakt UX.
6. **Utrata `/pub`** — narzedzia admin/test/statystyki na nim oparte.
7. **Duplikaty przy at-least-once** — OK dla klienta (dedup po `event-id`), ale
   fan-out jednego wpisu na N kanalow przy redelivery = N duplikatow; do
   udokumentowania.
8. **Split-brain replik** — kazda replika konsumuje niezalznie; wolna replika
   nadrabia z retencji streamu (`MAXLEN`/`MINID` musi byc dobrany).

---

## 6. Rekomendacja — etapowanie

**Etap 0 — PoC (2–3 tyg), cel: udowodnic C1 + C2.**
- Opcja A (`hiredis`) na skróty albo od razu minimalny RESP.
- Jeden worker (2b), plain `XREAD`, kursor w `shm`.
- Publikacja z `msgs:main` → `add_msg_to_channel`. Bez rejestru (kanaly jak dzis),
  bez aktywacji. `/pub` nadal dziala rownolegle.
- Metryka: przepustowosc, latencja, zachowanie przy `kill -9` Redis / reload.

**Etap 1 — rejestr kanalow (C3 + C4).**
- Bootstrap + delty z `channels:events`. Bramka: sub/pub tylko z rejestru.
- `/pub` przechodzi w tryb admin-only.

**Etap 2 — aktywacja z potwierdzeniem (C5 + C6).**
- Maszyna stanu, hold subskrybenta, IPC release, timeout, deaktywacja.
- Kontrakt z API na `channels:events` (op activate/activated/deactivate).

**Etap 3 — hardening.**
- Consumer groups (2a) jesli potrzeba przepustowosci, Sentinel, wiele streamow,
  statystyki, pelne testy awaryjne, usuniecie `/pub` z produkcji.

Decyzja go/no-go po Etapie 0.

---

## 7. Pytania otwarte (do decyzji przed PoC)

1. **Zakres aktywacji:** per replika (kazda replika osobno powiadamia API, gdy u
   niej pojawi sie 1. subskrybent) czy globalnie (jedna aktywacja na caly klaster,
   koordynowana lockiem w Redis)? API potrzebuje wiedziec „ktore repliki" czy
   tylko „czy ktos"?
2. **Timeout `confirm`:** fail-open (dostarcz mimo braku potwierdzenia) czy trzymaj
   / rozlacz? Jaki czas?
3. **Topologia Redis:** single primary + Sentinel czy Cluster? AUTH? TLS?
   Dedykowana instancja czy wspoldzielona z reszta f24? (przesadza o izolacji:
   numer DB tylko bez Cluster, inaczej sam prefiks + hash-tagi i `MULTI` przez
   sloty odpada — patrz C3 „Izolacja").
4. **Gwarancja dostarczenia:** akceptujemy at-least-once + dedup po `event-id` po
   stronie klienta? Czy jest wymog exactly-once?
5. **`POST /pub`:** zostaje jako admin/debug czy usuwamy calkowicie?
6. **Repliki:** wszystkie na jednym Redis? Lista kanalow identyczna dla wszystkich?
7. **Wiele streamow wiadomosci:** klucz shardingu (dyscyplina? priorytet? hash id
   kanalu?). Ile shardow?
8. **Skala:** liczba kanalow w rejestrze (łacznie / jednoczesnie aktywnych),
   tempo publikacji (msg/s), liczba subskrybentow, rozmiar wiadomosci. Wymiaruje
   `shm` rejestru i przepustowosc konsumenta.
9. **Propsy kanalu:** jakie pola i czy nginx **dziala** na nich (per-kanal rozmiar
   bufora / TTL / auth) czy tylko przechowuje i wystawia w `/channels-stats`?
10. **Model obecnosci per replika:** czy `ch:<id>:r:<replica>` ma niesc
    **przyblizona liczbe** subskrybentow (odswiezana heartbeatem) czy wystarczy
    sama obecnosc + stan? Czy API potrzebuje dokladnego licznika na zadanie
    (pull z `/channels-stats`)? Format wartosci wpisu.
11. **Retencja streamow** (`MAXLEN`/`MINID`) — jak dlugo wpisy zyja, zeby wolna /
    restartowana replika nadrobila bez luki? (patrz 4a i 4b)
12. **Limit liczby kanalow** — jaki `max`? Kto go egzekwuje przy `upsert`
    (aplikacja przez `SCARD` czy osobny gatekeeper)? Limity per kategoria?
13. **Reaper martwych kanalow** — kto go uruchamia (aplikacja / cron / nginx
    sygnalizuje `gc-candidate`) i po jakim czasie bezczynnosci kanal znika?
    (patrz 4b pkt 1)

---

## 8. Wstepny werdykt

**Wykonalne.** Najwieksza wartosc: warstwa dostarczania (`shm` + IPC + fan-out,
w tym multi-channel z dedup, startup-message, catch-up po `event-id`) **juz
istnieje i jest do ponownego uzycia** — konsument Redis wpina sie w te same
funkcje co dzisiejszy `POST /pub`.

Glowne nowe rzeczy: (1) asynchroniczny klient Redis w modelu event-loop nginx —
znane, sa precedensy; (2) maszyna stanu aktywacji z wstrzymaniem subskrybenta —
najtrudniejsza, bo dotyka goracej sciezki i wspolbieznosci miedzy workerami.

Rekomendacja: **PoC Etapu 0** (klient + konsument, bez rejestru i aktywacji),
decyzja go/no-go na jego podstawie, potem rejestr, na koncu aktywacja.
