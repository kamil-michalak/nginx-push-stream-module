# nginx-push-stream-module — Liczniki wymiarowe połączeń (push_stream_stat_counter)

## Opis

Natywne liczniki połączeń rozbite po dowolnych wymiarach (kraj, transport, kanał,
brand, klasa klienta…), z endpointem zwracającym surową mapę `klucz → {active, total}`
do samodzielnego parsowania.

Każda **grupa** liczników jest niezależna — własny szablon klucza, własny limit
kardynalności. Brak globalnego cross-productu: `country` ma ~200 kluczy, `transport`
3, `transport_country` ~600 — nie mnożysz ich przez siebie.

Dwa tryby zliczania (opcja `per=` na dyrektywie):

- **`per=connection`** (domyślnie) — jeden bump na **połączenie**, klucz z szablonu
  renderowany raz przy rejestracji połączenia. WS z 10 kanałami = 1 wpis.
  Dynamiczny `+kanal`/`-kanal` **nie rusza** — to atrybut połączenia.
- **`per=channel`** — jeden bump na **subskrypcję** (każdy kanał, do którego
  połączenie dołączy). WS na 3 kanały = +3. `+kanal`/`-kanal`
  (`push_stream_websocket_allow_resubscribe`) inkrementuje/dekrementuje.
  Token `~channel~` w szablonie zastępowany id konkretnego kanału.
  Dekrement przy: zamknięciu połączenia, `-kanal`, usunięciu kanału.

- `active` — podłączeni teraz (gauge). Live inc przy connekcie / dec przy
  rozłączeniu, dokładnie jak `channel->subscribers`. Dryfuje w górę tylko przy
  twardym crashu workera (brak dekrementu); `GET …?reset=active` zeruje wszystkie
  gauge.
- `total` — połączenia otwarte łącznie (monotoniczny, +1 na connect). Przeżywa
  `nginx -s reload`. `Δtotal` między scrapami = realny ruch (i jedyna wiarygodna
  miara dla long-pollingu, gdzie `active` to tylko polle zaparkowane w tej chwili).

Store żyje w pamięci współdzielonej na strukturze `shm_data` (tej samej, którą
gałąź reload `init_shm_zone` reużywa), przeżywa reload jak `ping_msgs_queue`.
Buckety nie są nigdy zwalniane — tylko dodawane. Po przekroczeniu limitu kluczy
grupy nowe klucze wpadają do jednego `__overflow__` per grupa.

---

## Dyrektywy

```
push_stream_stat_counter <grupa> <szablon-klucza> [max=N] [keys=N] [per=connection|channel];
```

**Kontekst:** `http`, `server`, `location`. **Wielokrotnie.** Dziedziczenie
**dodające** — lokacja zachowuje własne wpisy i dostaje wpisy rodzica dopisane
za nimi.

| pole | znaczenie |
|---|---|
| `<grupa>` | nazwa niezależnego licznika |
| `<szablon-klucza>` | **complex value** (`$zmienne`, literały, mix). `per=connection`: renderowany raz przy rejestracji połączenia. `per=channel`: renderowany per subskrypcja, token `~channel~` → id kanału |
| `max=N` | limit długości klucza w bajtach (domyślnie `NGX_HTTP_PUSH_STREAM_STAT_DEFAULT_KEY_LEN` = 96) |
| `keys=N` | limit odrębnych kluczy w tej grupie (domyślnie `push_stream_stat_max_keys`); nadmiar → `__overflow__` |
| `per=` | `connection` (domyślnie) lub `channel` — patrz „Dwa tryby zliczania" wyżej |

```
push_stream_stat_max_keys N;
```

**Kontekst:** `http`. Domyślny limit kluczy per grupa (`NGX_HTTP_PUSH_STREAM_STAT_DEFAULT_MAX_KEYS`
= 50000). Nadpisywany przez `keys=` w konkretnej grupie.

```
push_stream_stats_dimensions;
```

**Kontekst:** `location`, bez argumentów. Czyni lokację endpointem statystyk
wymiarowych. **Nie** wymaga `push_stream_channels_path`.

### Zmienna `$ps_transport`

Dostarczana przez moduł, wartość z typu lokacji subskrybenta:
`ws` | `lp` | `poll` | `stream` | `es` | `other`. Do składania w szablonach kluczy —
transport bez dyrektywy.

### Sanityzacja klucza

Wyrenderowany klucz przepuszczany przez alfabet `[0-9A-Za-z_.:|/-]` (bezpieczny
w JSON i jako separator), reszta znaków → `_`, obcięcie do `max`, nigdy pusty
(pusty → `_`). Per-zmiennowe fallbacki (np. `??` dla brakującego kraju) rób
mapą po stronie nginx.

---

## Endpoint

```nginx
location = /stats/live {
    push_stream_stats_dimensions;
    allow 10.0.0.0/8;
    allow 127.0.0.1;
    deny all;
}
```

`GET /stats/live` →

```jsonc
{
  "generated": 1788956018,
  "counters": {
    "country":            { "PL": {"a":41230,"t":9912004}, "DE": {"a":12980,"t":...} },
    "transport":          { "ws": {"a":91200,"t":...}, "lp": {"a":5100,"t":...} },
    "transport_country":  { "ws|PL": {"a":33110,"t":...}, "lp|PL": {"a":210,"t":...} },
    "chan":               { "liveHtmlXml_20": {"a":60110,"t":...}, "__overflow__": {"a":2,"t":880} }
  }
}
```

Klucze posortowane (grupa, potem klucz). `a` = active (teraz), `t` = total (suma).

`GET /stats/live?reset=active` → `{"reset":"active"}` — zeruje wszystkie `active`,
zostawia `total`. **Uwaga:** zeruje też wkład aktualnie podłączonych — wrócą do
licznika dopiero przy reconnekcie (dla WS z `subscriber_connection_ttl 4h` to
wolno). Rób w oknie serwisowym albo z rolling-reconnectem.

Tylko `GET`. Inne metody → `405`.

---

## Przykład (f24)

```nginx
# http { }
map $host $brand {
    default                other;
    ~*futbol24\.com$       futbol24;
    ~*endirect24\.com$     endirect24;
    ~*calcio24\.com$       calcio24;
    # ...
}
map $http_user_agent $client_class {
    default                                             web-desktop;
    ""                                                  unknown;
    "~*bot|crawl|spider|slurp|curl|wget|python|monitor" bot;
    "~*okhttp|cfnetwork|dalvik|futbol24app"             app;
    "~*mobile|android|iphone|ipad|ipod"                 web-mobile;
}
map $arg_cc $cc { default "??"; "~^[A-Za-z]{2}$" $arg_cc; }

push_stream_stat_max_keys 50000;

push_stream_stat_counter country            "$cc";
push_stream_stat_counter transport          "$ps_transport";
push_stream_stat_counter transport_country  "$ps_transport|$cc"      max=40;
push_stream_stat_counter brand_country      "$brand|$cc";
push_stream_stat_counter client_country     "$client_class|$cc";
push_stream_stat_counter channel            "$arg_ch"                keys=200;

# per subskrypcja - łapie też dynamiczne +kanal (resubscribe)
push_stream_stat_counter sub_channel        "~channel~"              per=channel keys=5000;
push_stream_stat_counter sub_channel_country "~channel~|$cc"         per=channel keys=20000;
```

Lokacje `/api/live/ws|lp|stream` — **zero linii** o statystykach, wszystko
dziedziczone. Wyrzuć `wch_info_conn_*_country_*` z `push_stream_channels_path`.

---

## Zmienione pliki

| Plik | Zakres |
|---|---|
| `src/ngx_http_push_stream_module_stats.c` | **nowy** — cała logika: rbtree bucketów w shm, per-grupa key-count, sanityzacja, register/unregister, `$ps_transport`, parsery dyrektyw, endpoint |
| `include/ngx_http_push_stream_module.h` | `stat_counter_t`, `stat_bucket_t`, `stat_group_t`; pola w `shm_data` (tree/queue/groups/mutex), `subscriber_t` (`stat_buckets`), `loc_conf_t` (`stat_counters`), `main_conf_t` (`stat_max_keys`); tryb `STATS_DIMENSIONS_MODE`, stałe `STAT_DEFAULT_*` |
| `include/ngx_http_push_stream_module_setup.h` | deklaracje parserów dyrektyw |
| `include/ngx_http_push_stream_module_utils.h` | deklaracje `stat_register` / `stat_unregister` / `stats_init_shm` / `stats_add_variables` |
| `src/ngx_http_push_stream_module.c` | `#include` nowego pliku (po `_utils.c`) |
| `src/ngx_http_push_stream_module_setup.c` | 3 dyrektywy w tablicy; init `stat_max_keys` / `stat_counters`; additive merge; `stats_init_shm` w `init_shm_zone`; `stats_add_variables` w preconfig; `STATS_DIMENSIONS_MODE` omija check `channels_path` |
| `src/ngx_http_push_stream_module_subscriber.c` | hook `stat_register` w `registry_subscriber` (per=connection); hook `stat_subscription_register` na końcu `assing_subscription_to_channel` (per=channel, obejmuje `+kanal`) |
| `src/ngx_http_push_stream_module_utils.c` | hook `stat_unregister` w `worker_subscriber_cleanup`; `stat_subscription_unregister` w pętli per-kanał `worker_subscriber_cleanup` **i** w `respond_to_subscribers` (kanał usunięty) |
| `src/ngx_http_push_stream_module_websocket.c` | `stat_subscription_unregister` w `websocket_handle_unsubscribe` (`-kanal`) |

---

## Uwagi implementacyjne / ograniczenia

- **Blokowanie:** dedykowany `d->stats_mutex` (nie globalny `shpool->mutex`).
  Ścieżka common (bucket istnieje) = lock + 2 inkrementy. Alokacja tylko przy
  pierwszym widzeniu klucza; kolejność locków `stats_mutex → shpool->mutex`
  (nic nie bierze ich odwrotnie).
- **Bez census:** `active` utrzymywane wyłącznie live inc/dec. Reload obsłużony
  bez rebuildu (buckety przeżywają, stare workery dekrementują przy zamykaniu
  połączeń, nowe inkrementują). Dryf tylko po `SIGKILL`/`SIGSEGV` workera →
  `?reset=active`. To (nieco gorsza) charakterystyka niż `channel->subscribers`,
  które dostaje korektę census przy reloadzie.
- **`per=connection` + wymiar kanału** = kanał startowy z `?ch=` / ścieżki, nie
  realne subskrypcje po `+kanal`. Jak chcesz realne — `per=channel` z `~channel~`.
- **`per=channel`**: `~channel~` podmieniane per subskrypcja (complex value nadal
  renderowany per subskrypcja — koszt: 1 eval na subskrypcję, tanie). `total`
  liczy każde dołączenie do kanału (`+kanal` po `-kanal` = +1 do total).
  `+kanal` działa tylko na istniejące kanały (`find_channel`, moduł nie tworzy) —
  nieistniejący kanał = brak subskrypcji = brak licznika.
- **Kardynalność:** `keys=` per grupa + `__overflow__` chronią shm lokalnie.
  Sanityzacja `$arg_ch` do `_` zbiera skanery w jeden bucket.
- **Binary upgrade** (SIGUSR2, nowe `.so`): `d->stats_tree.insert` to wskaźnik
  na funkcję w binarce — jak istniejące `d->tree` w module, nie obsługiwane
  inaczej niż reszta modułu.

---

## Testy (nginx 1.27.4, `-W -Wall -Wpointer-arith -Werror`)

- kompilacja czysta z/bez modułu w buildzie
- `-t`: walidacja dyrektyw (brak argów, złe `max=`/`keys=`, pusta grupa, `bogus=`)
- streaming / WS / long-polling: `active`/`total` poprawne w trakcie i po rozłączeniu
- `keys=5` per grupa: 5 kluczy + `__overflow__`, inne grupy nietknięte
- `?reset=active`: gauge → 0, total zachowane
- reload: `total` zachowane, nowe połączenia trafiają do istniejących bucketów
- **`per=channel`**: WS URL-sub + 2× `+kanal` → 3 buckety; `-kanal` → `a:0 t:1`;
  multi-kanał URL (`/ws/a/b`) → +1 na każdym; rozłączenie → wszystkie `a:0`
- soak per=connection: 30 wątków, ~1600 conn/disc + 2× reload → `active`=0 wszędzie
- soak per=channel: 25 wątków, multi-kanał + `+`/`-` + 2× reload → `active`=0 wszędzie,
  totale spójne, zero warn/error (alerty „open socket left" przy reload+WS
  występują **identycznie bez** tej funkcji — pre-existing, forced kill po
  worker_shutdown_timeout)
