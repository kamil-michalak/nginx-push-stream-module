# nginx-push-stream-module — Wiadomosc startowa z URL (lazy fetch + TTL + single-flight)

> Status: **feasibility study**. Dokument roboczy. Nadbudowa nad juz
> zaimplementowana wiadomoscia startowa — patrz [nginx-push-stream-startup-message.md](nginx-push-stream-startup-message.md).

---

## 1. Cel

Dzis wiadomosc startowa kanalu to **tresc inline** — publisher liczy snapshot i
POST-uje jego cialo przez `POST /pub  Startup-Message: 1`. Wady przy duzym,
wolno zmieniajacym sie snapshocie:

- publisher musi **proaktywnie** wypychac swiezy snapshot (na zmianie / w petli),
- kazdy re-POST przechodzi przez shm (`convert_char_to_msg_on_shared`: raw
  chunked + kopie per szablon + warianty WS) — koszt nawet gdy nikt nie slucha.

Docelowo: publisher podaje **URL + TTL**, a nginx sam **leniwie** pobiera snapshot
z tego URL, **cache'uje** go przez TTL i odswieza dopiero gdy wygasnie i przyjdzie
nowy subskrybent. Do tego **single-flight**: gdy jeden pobiera, kolejni czekaja
(maks ~5 s) az skonczy, zamiast kazdy odpalac wlasny fetch (thundering herd).

---

## 2. Jak dziala dzis wiadomosc startowa (punkt wyjscia)

| Element | Stan obecny |
|---|---|
| Przechowywanie | `channel->startup_message` (`ngx_http_push_stream_msg_t *`) w shm, poza `message_queue`, przezywa reload |
| Ustawianie | `publisher_body_handler`: `Startup-Message: 1` + niepuste body → `convert_char_to_msg_on_shared` → `set_channel_startup_message` (atomowy swap + free starej po zwolnieniu locka) |
| Dostarczanie | `ngx_http_push_stream_send_startup_message(r, channel)` — **synchroniczne**: pod `channel->mutex` bierze `workers_ref_count++` i kopiuje granice, zwalnia locka, `send_response_message(snapshot)`, `workers_ref_count--`, potem `send_old_messages` (catch-up od `Event-Id` snapshotu / pozycji strumienia) |
| Punkty wpiecia | `subscriber_assign_channel` (streaming / WS initial), `subscriber_polling_handler` (poll / long-poll), `websocket_handle_subscribe` (`+channel` bez `event_id`) — wszystkie **wywoluja `send_startup_message` synchronicznie w petli po kanalach** |
| Lifetime (fix `1f6d12c` / `test24`) | `startup_message` mial UAF przy burzy reconnectow; sendy wychodza spod `channel->mutex` dzieki `workers_ref_count` + trash-TTL |

---

## 3. Kluczowa obserwacja — catch-up sam sie leczy

`send_startup_message` po snapshocie **odtwarza z bufora wszystko po jego
`Event-Id`** (semantyka `Last-Event-Id`). Wniosek:

> **Snapshot kilka sekund / minut nieaktualny + catch-up = ten sam stan koncowy**,
> dopoki `Event-Id` snapshotu nie wyrotowal z bufora
> (`push_stream_message_buffer_size` / `push_stream_message_ttl`).

To zmienia wymagania:

- **twarde „czekaj az pierwszy dociagnie" nie jest potrzebne** w normalnym
  przypadku — wystarczy „serwuj stary, odswiez w tle",
- TTL wiadomosci startowej tylko musi byc **< retencja bufora**,
- 5-sekundowa bariera single-flight ma sens tylko dla **pierwszego** snapshotu
  kanalu (gdy nie ma jeszcze zadnego starego do podania).

Kazdy z wariantow ponizej korzysta z tej wlasciwosci w innym stopniu.

---

## 4. Wariant 1 — wrapper w Lua (rekomendowany, ~0 zmian w C)

### 4.1 Zaloznia

Lua + `ndk` sa **wkompilowane** w wasz nginx
(`containers/nginx/build/Dockerfile`: `ENABLED_MODULES="… ndk lua …"`,
`lua-cjson` przez luarocks, `lua-resty-core` w `lua_package_path`). Trzeba
domiescic `lua-resty-lock` (`luarocks install lua-resty-lock`).

### 4.2 Idea

`access_by_lua` na locationie `ws/` / `lp/` — **przed** content handlerem
push-stream. Snapshot dalej trzymany jest jako **tresc inline** na kanale
(`Startup-Message: 1`), Lua tylko dba, zeby byl swiezy, re-publikujac go przez
wewnetrzny `/pub`.

```nginx
http {
    lua_package_path  "/usr/local/lib/lua/?.lua;;";
    lua_shared_dict   sm_meta   1m;    # {channel -> fetched_at}
    lua_shared_dict   sm_locks  1m;    # resty.lock

    server {
        location = /_pub_internal {          # tylko z 127.0.0.1
            internal;
            push_stream_publisher admin;
            push_stream_channels_path $arg_id;
        }

        location ~ /f24/liveHtmlXml/ws/(?<lid>…)$ {
            set $sm_channel  "liveHtmlXml_$lid";
            set $sm_url      "http://snapshot_backend/snapshots/$lid";
            set $sm_ttl      30;
            access_by_lua_file  conf/lua/startup_refresh.lua;

            push_stream_subscriber websocket;
            push_stream_channels_path $sm_channel;
            # … reszta jak dzis
        }
    }
}
```

`startup_refresh.lua` (szkic):

```lua
local ch  = ngx.var.sm_channel
local ttl = tonumber(ngx.var.sm_ttl)
local meta = ngx.shared.sm_meta
local now  = ngx.now()

local last = meta:get(ch) or 0
if now - last < ttl then return end          -- swieze -> przepusc

local lock = require("resty.lock"):new("sm_locks", { timeout = 5, exptime = 10 })
local ok = lock:lock(ch)                      -- jeden wchodzi, reszta czeka <=5 s
if not ok then return end                     -- timeout -> i tak przepusc (stary snapshot)

last = meta:get(ch) or 0
if now - last >= ttl then                     -- double-check pod lockiem
    local snap = ngx.location.capture(ngx.var.sm_url)
    if snap.status == 200 then
        ngx.location.capture("/_pub_internal?id=" .. ch, {
            method  = ngx.HTTP_POST,
            body    = snap.body,
            headers = {
                ["Startup-Message"] = "1",
                ["Event-Id"]        = snap.header["X-Event-Id"],
            },
        })
        meta:set(ch, ngx.now())
    end
end
lock:unlock()
```

Po fazie `access` content handler push-stream serwuje juz **swiezy**
`channel->startup_message` (jest w shm, bo `/_pub_internal` → `set_channel_startup_message`
konczy sie synchronicznie w fazie access).

### 4.3 Wlasciwosci

- **single-flight cross-worker + 5 s**: `resty.lock` trzyma stan w
  `lua_shared_dict` (wspoldzielony miedzy workerami). Dokladnie semantyka
  „jeden pobiera, reszta czeka maks 5 s".
- **TTL**: porownanie `ngx.now()` z `sm_meta`, lub `meta:set(ch, now, ttl)` z
  wygasaniem klucza.
- **WS bezpieczne**: `access_by_lua` biegnie **przed** upgrade 101 — to zwykly
  request HTTP, `ngx.location.capture` dziala normalnie.
- **zero zmian w C**, zero async w sciezce subscribe modulu.
- pierwszy klient po wygasnieciu placi latencje fetcha (albo do 5 s jesli jest
  „przegranym" locka); reszta idzie szybka sciezka.

### 4.4 Koszty / ryzyka

- Lua na hot-path — ale gdy cache swiezy to jeden `shdict:get` (nanosekundy).
- `lua_shared_dict` + `lua_package_path` trzeba dodac do `http{}`
  **push-frontendu** (dzis lua jest uzywana na `nginx_server_r.conf`, nie na
  froncie WS — do sprawdzenia czy ten sam proces).
- `lua-resty-lock` do doinstalowania.
- `/_pub_internal` musi byc `internal` + `allow 127.0.0.1` (nie wystawiac).
- Subtelnosc: jesli `push_stream_authorized_channels_only on`, kanal musi
  istniec — POST do `/_pub_internal` go utworzy (`get_channel`), co jest OK.

---

## 5. Wariant 2 — natywnie w C, cache oparty o `proxy_cache`

### 5.1 API publikacji

Dwa nowe naglowki (spojne z `Startup-Message` / `Event-Id` / `Message-TTL`):

```
POST /pub?id=matchA
Startup-Message: 1
Startup-Message-Url: /snapshots/matchA      # SCIEZKA, rozwiazywana wg configu (nie dowolny URL)
Startup-Message-Ttl: 30                      # sekundy — jak dlugo pobrane body jest swieze
Event-Id: goal_5                             # granica catch-up — bez zmian
                                            # body puste (opcja: body = fallback do 1. udanego fetcha)
```

- `Startup-Message: 1` + `Startup-Message-Url` → wiadomosc startowa kanalu to
  **referencja URL**, nie tresc inline.
- czyszczenie jak dzis (`Startup-Message: 1` + puste body + brak URL).

### 5.2 Przechowywanie na kanale

Obok `channel->startup_message` (inline `msg *`) — maly, statyczny wariant:

```c
struct ngx_http_push_stream_startup_ref_s {
    ngx_str_t   url;        /* kopia w shm */
    ngx_str_t   event_id;   /* granica catch-up, kopia w shm */
    time_t      ttl;
    /* NIC wiecej — pobrane body i jego swiezosc zyja w proxy_cache */
};
```

Kanal trzyma **albo** `startup_message` (inline) **albo** `startup_ref`.
`send_startup_message` rozgalezia sie na tym, co ustawione. Pobrane body
celowo **nie** ląduje w shm — proxy_cache juz to robi (keyed, cross-worker,
TTL, lock).

### 5.3 Fetch — wewnetrzny subrequest do locationa z `proxy_cache`

Config operatora:

```nginx
proxy_cache_path /var/cache/nginx/startup keys_zone=startup_cache:16m
                 inactive=10m max_size=512m;

location /_startup_fetch/ {
    internal;
    proxy_pass                http://snapshot_backend/;
    proxy_cache               startup_cache;
    proxy_cache_key           $uri;
    proxy_cache_lock          on;          # <<< single-flight
    proxy_cache_lock_timeout  5s;          # <<< "reszta czeka maks 5 s"
    proxy_cache_lock_age      5s;
    proxy_cache_valid         200 30s;     # albo honoruj Cache-Control z backendu (per-URL TTL)
    proxy_cache_use_stale     updating error timeout;
}
```

Nowa dyrektywa na locationie subskrybenta:

```nginx
push_stream_startup_message_fetch_pass /_startup_fetch/;
```

**`proxy_cache_lock on` + `proxy_cache_lock_timeout 5s` to doslownie ta
funkcja** — pierwszy request na dany klucz idzie do backendu, rownolegle
czekaja (<=5 s) az cache sie zapelni, potem serwuja z cache. Cross-worker
(strefa w shm). ~15 lat w produkcji.

### 5.4 Strona modulu — `send_startup_message` staje sie asynchroniczne

Dzis: synchroniczne (patrz §2). Z URL-ref body nie ma w rece:

1. `channel->startup_ref` ustawione → trzeba body.
2. skopiuj `url` i `event_id` do `r->pool` (jak dzis kopiujemy `event_id`).
3. `ngx_http_subrequest(r, url, NULL, &sr, psr, NGX_HTTP_SUBREQUEST_IN_MEMORY)`
   + `psr->handler` = post-subrequest callback.
4. zwroc „pending" do sciezki subscribe — subskrypcja **zawieszona**.
5. subrequest: `/_startup_fetch/` → proxy_cache serwuje z cache albo pobiera
   z backendu; `proxy_cache_lock` koordynuje rownolegle miss-y.
6. post-subrequest handler:
   - sprawdz `r->connection->error` / `c->destroyed` → jesli klient zniknal, wyjdz,
   - wyciagnij body z `sr` (`IN_MEMORY` → `sr->out` / body filter),
   - zbuduj **przejsciowy** `msg` z body (`convert_char_to_msg_on_shared` do
     puli requestu — NIE na kanal, to jednorazowe),
   - `send_response_message(msg)` — ramka snapshotu,
   - `send_old_messages(catch-up)` z zapamietanym `event_id`,
   - **wznow subskrypcje**: `assing_subscription_to_channel` + dokoncz
     (streaming: trzymaj polaczenie; poll: finalize; WS: kontynuuj).

### 5.5 Gdzie robi sie naprawde trudno

**A. Multi-channel subscribe.** `push_stream_channels_path "a/b/c"` — kazdy kanal
z URL-ref to osobny subrequest. Serializacja (subreq 1 → wznow → subreq 2 …)
albo fan-out + join. Modul nie ma helpera „join N subrequestow". Nowy automat
stanowy w funkcji, ktora dzis jest zwykla petla `for` — plus kursor w `ctx`
(„ktory kanal wznawiam, co zostalo").

**B. WebSocket, po 101.** Gdy `subscriber_assign_channel` rusza dla WS,
naglowki 101 sa juz wyslane, polaczenie „upgraded". `ngx_http_subrequest`
z requestu w tym stanie to **nietestowana sciezka** — subrequesty zakladaja
normalny cykl zycia (naglowki niesfinalizowane, drzewo requestow przechodzalne).
Moze zadziala (obiekt requestu WS istnieje, niesfinalizowany), ale:
   - odpowiedz subrequestu nie moze wyciec w strumien bajtow WS
     (`IN_MEMORY` tlumi output — pomaga),
   - swap write handlera na czytanie WS i posted requests moga kolidowac,
   - realnie: pewnie trzeba wystrzelic fetch **przed** wyslaniem 101
     (przeniesc do handlera handshake'u).
   **Wymaga spike'a.**

**C. Konflikt z fixami lifetime (`1f6d12c`).** `startup_message` mial UAF przy
burzy reconnectow; fix = `workers_ref_count` + trash-TTL, sendy poza
`channel->mutex`. Dochodzi subrequest zyjacy do 5 s, w czasie ktorego
polaczenie moze pa:
   - subskrybent rozlacza sie w trakcie → post-handler musi to wykryc i wyjsc
     bez dotykania zwolnionego `r`,
   - `startup_ref` moze zostac podmienione/wyczyszczone → `url` + `event_id`
     kopiowane do `r->pool` przy starcie (OK),
   - **burza reconnectow**: 40k subskrybentow, kazdy wystawia subrequest →
     `proxy_cache_lock` zbija do 1 hitu backendu + **~40k requestow
     zaparkowanych do 5 s**, kazdy trzyma pamiec requestu + strukture
     subrequestu + timer. Skok pamieci dokladnie wtedy, gdy juz jest ciezko.
     - mitygacja: `proxy_cache_use_stale updating` → „przegrani" dostaja stare
       body natychmiast → szybka sciezka wraca… ale wtedy blokujace 5 s traci
       sens (i wracamy do wlasciwosci z §3).

**D. Klucz cache vs URL.** Absolutny URL do dowolnego hosta → `proxy_pass` nie
wezmie runtime-owego hosta bez resolvera → budowa HTTP-klienta w C. Praktyczne
ograniczenie: URL to **sciezka** wobec jednego skonfigurowanego upstreamu
(`proxy_pass http://snapshot_backend$startup_path;`). Snapshot-serwis jest wasz,
wiec akceptowalne — ale to nie „fetch dowolnego URL".

**E. Bledy.** Backend 500 / timeout / refused → serwuj nic (tylko catch-up +
live) albo last-known-good z `proxy_cache_use_stale error timeout`. Subskrypcja
**musi** wznowic sie niezaleznie — nieudany fetch nie moze zablokowac polaczenia.

### 5.6 Bilans wariantu 2

| | |
|---|---|
| single-flight + 5 s wait | ✅ za darmo, `proxy_cache_lock`/`_lock_timeout`, cross-worker |
| TTL | ✅ `proxy_cache_valid` / `Cache-Control` z backendu |
| serve-stale-while-refresh | ✅ `proxy_cache_use_stale updating` |
| bez lua | ✅ |
| czysta dyrektywa push-stream, bez wspolpracy API | ✅ |
| nowy C: async subrequest w sciezce subscribe | ❌ ~200–400 linii, automat stanowy w `subscriber_assign_channel` + 3 wejscia |
| subrequest WS po 101 | ❌ nieudowodnione, spike, moze wymusic fetch przed 101 |
| burza reconnectow | 🟡 ~40k requestow zaparkowanych do 5 s, chyba ze `use_stale` (co czyni blokowanie bezcelowym) |
| dowolny URL | ❌ tylko sciezka wobec jednego upstreamu |

---

## 6. Wariant 3 — event na events-channel → API re-publikuje

Bez HTTP-klienta w nginx, bez async w C, bez lua.

Modul trzyma na kanale `{ttl, last_set}` (dwa pola, statyczne). Gdy laczy sie
**swiezy** subskrybent, snapshot jest starszy niz `ttl` **i** kanal ma
subskrybentow → modul emituje `startup_stale:<channel>` na events-channel
(`push_stream_events_channel_id`).

API jest subskrybentem events-channel (wg planu
[nginx-push-stream-redis-transport.md](nginx-push-stream-redis-transport.md) i
tak ma nasluchiwac) → przelicza snapshot → `POST /pub  Startup-Message: 1`.

Laczacy sie klient dostaje **biezacy** (moze lekko stale) snapshot teraz;
swiezy laduje dla nastepnego. Single-flight naturalny — jeden event obsluzony
raz przez API (debounce po stronie API, np. „nie czesciej niz co X s na kanal").

### 6.1 Wlasciwosci

- **zero HTTP-klienta / async / lua w nginx** — modul tylko emituje event
  (juz to potrafi: `ngx_http_push_stream_send_event`).
- opiera sie na wlasciwosci z §3 — stale + catch-up = poprawny stan, wiec
  „serve stale, odswiez dla nastepnego" jest **poprawne**, nie tylko „wystarczajace".
- TTL to jedno porownanie `time_t` w `send_startup_message`.
- brak twardej bariery 5 s — jesli chcesz „nastepny klient dostanie juz swiezy",
  to naturalnie wychodzi (2. event w tym samym oknie API zdebounce'uje).

### 6.2 Koszty / ryzyka

- wymaga **wspolpracy API** (subskrypcja events-channel + endpoint przeliczajacy).
  Ale to i tak jest w planie Redis-transport.
- „pierwszy klient po wygasnieciu" dostaje stary snapshot (akceptowalne dzieki §3;
  jesli kanal nigdy nie mial snapshotu — dostaje nic, jak dzis bez tej funkcji).
- debounce/idempotencja po stronie API (jeden event → jedno przeliczenie).
- event leci **z instancji nginx, ktora obsluzyla subskrybenta** — przy wielu
  replikach kazda replika moze wyemitowac swoj; API debounce'uje po `channel`.

---

## 7. Porownanie

| Kryterium | W1 (Lua) | W2 (C + proxy_cache) | W3 (event → API) |
|---|---|---|---|
| zmiany w C modulu | brak | ~200–400 linii, async subscribe | ~30 linii (emisja eventu + TTL) |
| async w sciezce subscribe | nie | **tak** | nie |
| ryzyko przy WS | brak (faza access) | **wysokie** (subrequest po 101) | brak |
| konflikt z fixami lifetime `test24` | brak | **tak** | minimalny |
| single-flight + „czekaj 5 s" | ✅ resty.lock | ✅ proxy_cache_lock | 🟡 debounce API (bez twardej bariery) |
| TTL | ✅ shdict | ✅ proxy_cache | ✅ porownanie time_t |
| serve-stale-while-refresh | ✅ (przepusc stary) | ✅ use_stale | ✅ (z natury) |
| HTTP-klient w nginx | nie (`ngx.location.capture`) | nie (subrequest → proxy_pass) | **nie** |
| wspolpraca API | nie | nie | **tak** |
| lua na hot-path | tak (tani gdy swieze) | nie | nie |
| burza reconnectow | OK (shdict get + lock skip) | 🟡 ~40k parked / use_stale | OK (event + debounce) |
| dowolny URL | ✅ | ❌ sciezka/1 upstream | ✅ (API decyduje) |

---

## 8. Rekomendacja

1. **Jesli API i tak ma nasluchiwac events-channel** (Redis-transport) →
   **wariant 3**. Najtanszy, najbezpieczniejszy, korzysta z self-healing catch-up,
   zero async/lua w nginx.
2. **Jesli chcesz to samowystarczalne w nginx i mozesz dodac `lua_shared_dict`
   do push-frontendu** → **wariant 1**. Sprawdzone prymitywy (`resty.lock`),
   dokladna semantyka 5 s, zero C.
3. **Wariant 2** tylko jesli koniecznie jako dyrektywa push-stream, bez lua,
   bez wspolpracy API — ale finansujesz refaktor async-subscribe + spike na
   subrequest WS + akceptujesz interakcje z fixami lifetime z `test24`.
   W momencie gdy wlaczysz `proxy_cache_use_stale updating` (by przezyc burze
   reconnectow), blokujace 5 s przestaje miec znaczenie — i wtedy W3 daje ten
   sam efekt taniej.

---

## 9. Pytania otwarte

- Czy push-frontend (host-networked, per
  [firewall-conntrack-ws-limit](../../docker-setup/docs/notes/firewall.md)) to
  ten sam proces nginx co `nginx_server_r.conf` z lua, czy osobny? (decyduje o
  koszcie W1 — dodanie `lua_shared_dict` + package path).
- Format odpowiedzi snapshot-serwisu: samo body + `X-Event-Id` w naglowku? czy
  koperta JSON `{event_id, body}`? (W1/W2 wolą naglowek — mniej parsowania).
- TTL: staly per-location czy per-kanal (z naglowka `Startup-Message-Ttl`)?
- Czy „pierwszy klient po wygasnieciu" MUSI dostac swiezy snapshot (twarde 5 s),
  czy stale + catch-up wystarczy (§3)? To rozstrzyga miedzy W3 a W1/W2.
- W3: gdzie debounce — API czy modul (per-kanal „nie emituj czesciej niz co X s")?
