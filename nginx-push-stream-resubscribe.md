# nginx-push-stream-module — Dynamic Subscribe/Unsubscribe przez WebSocket

## Opis

Rozszerzenie modulu o mozliwosc dynamicznego dodawania i usuwania kanalow
podczas trwania polaczenia WebSocket — bez potrzeby rozlaczania i laczenia ponownie.

Funkcjonalnosc jest sterowana nowa dyrektywa `push_stream_websocket_allow_resubscribe`,
niezalezna od istniejacego `push_stream_websocket_allow_publish`.

---

## Zmienione pliki

| Plik | Zakres zmian |
|---|---|
| `include/ngx_http_push_stream_module.h` | Nowe pola w `loc_conf` (`websocket_allow_resubscribe`, `websocket_max_channels_per_connection`), stale protokolu, stringi ACK/error |
| `src/ngx_http_push_stream_module_setup.c` | Rejestracja dyrektyw, init, merge |
| `src/ngx_http_push_stream_module_subscriber.c` | Sprawdzenie limitu kanalow z URL przy polaczeniu WebSocket |
| `src/ngx_http_push_stream_module_websocket.c` | Funkcje pomocnicze, handler wiadomosci przychodzacych, sprawdzenie limitu przy dynamic subscribe |

---

## Nowe dyrektywy

```nginx
push_stream_websocket_allow_resubscribe on | off;
```

**Domyslnie:** `off`  
**Kontekst:** `http`, `server`, `location`

```nginx
push_stream_websocket_max_channels_per_connection <liczba> | off;
```

**Domyslnie:** brak limitu  
**Kontekst:** `http`, `server`, `location`

Maksymalna laczna liczba kanalow na ktore moze byc zapisany jeden klient WebSocket
w danym momencie. Limit obejmuje **zarowno kanaly z URL jak i dynamicznie dodane**:

- przy polaczeniu: jesli URL zawiera wiecej kanalow niz limit → `403 Forbidden`
- przy `+channel`: jesli suma (kanaly z URL + dotychczasowe dynamic) >= limit → blad JSON

Gdy klient przekroczy limit przy dynamic subscribe:
```json
{"error":"max channels per connection reached","channel":"channel_name"}
```

### Kombinacje z `push_stream_websocket_allow_publish`

| `allow_resubscribe` | `allow_publish` | Komendy `+`/`-` | Normalna wiadomosc |
|---|---|---|---|
| off | off | ignorowane | ignorowane |
| **on** | off | **dzialaja** | ignorowane |
| off | on | ignorowane | **dziala** |
| on | on | **dzialaja** | **dziala** |

Wiadomosci zaczynajace sie od `+` lub `-` nigdy nie trafiaja do publish
nawet gdy `allow_publish on`. Normalna wiadomosc nigdy nie jest traktowana
jako komenda gdy `allow_resubscribe off`.

---

## Protokol

Klient wysyla przez WebSocket ramki tekstowe z prefiksem komendy:

```
+channel_name              subscribe bez historii
+channel_name:event_id     subscribe + historia od event_id
-channel_name              unsubscribe
```

Separator `:` rozdziela nazwe kanalu od `event_id` — tylko pierwsze
wystapienie jest separatorem, reszta jest czescia `event_id`.

### Odpowiedzi serwera

Serwer odpowiada ramka JSON po kazdej komendzie:

```json
{"subscribed":"channel_name"}
{"unsubscribed":"channel_name"}
{"error":"channel not found","channel":"channel_name"}
{"error":"already subscribed","channel":"channel_name"}
{"error":"not subscribed","channel":"channel_name"}
{"error":"max channels reached","channel":"channel_name"}
```

---

## Historia wiadomosci przy subscribe

Gdy klient poda `event_id`, serwer wywoluje `send_old_messages` przed
zarejestrowaniem subskrypcji — dzieki czemu historia jest dostarczona
zanim zaczna przychodzic nowe wiadomosci (brak podwojnego dostarczenia).

```
ws.send('+matchB:goal_5');
```

Zachowanie identyczne jak przy normalnym polaczeniu z `Last-Event-Id`:
- serwer szuka w buforze wiadomosci z danym `event_id`
- dostarcza wszystkie wiadomosci **po** niej
- jesli `event_id` nie istnieje w buforze (TTL wygasl) — brak historii,
  subskrypcja rejestrowana od teraz

Bez `event_id`:
```
ws.send('+matchB');
```
Tylko nowe wiadomosci — historia nie jest wysylana.

---

## Walidacja przy subscribe

Przed zarejestrowaniem subskrypcji sprawdzane sa:

1. Dlugosc `channel_id` wzgledem `push_stream_max_channel_id_length`
2. Istnienie kanalu (nie tworzy nowych — tylko `find_channel`, nie `get_channel`)
3. Czy klient juz jest zapisany na ten kanal (brak duplikatow)
4. Limit subskrybentow na kanal (`push_stream_max_subscribers_per_channel`)

---

## Nowe funkcje w `websocket.c`

### `ngx_http_push_stream_websocket_send_ack`
Wysyla ramke JSON do klienta. Format `fmt` przyjmuje jeden argument `%V` (channel id).

### `ngx_http_push_stream_websocket_handle_subscribe`
Obsluguje komende `+channel_name[:event_id]`:
- walidacja channel_id i limitow
- `ngx_http_push_stream_find_channel` — wyszukanie kanalu
- sprawdzenie duplikatu w `ctx->subscriber->subscriptions`
- `ngx_http_push_stream_send_old_messages` — historia jesli podano event_id
- `ngx_http_push_stream_assing_subscription_to_channel` — rejestracja
- wyslanie ACK `{"subscribed":"..."}`

### `ngx_http_push_stream_websocket_handle_unsubscribe`
Obsluguje komende `-channel_name`:
- iteracja po `ctx->subscriber->subscriptions`
- pod `channel->mutex`: dekrementacja `channel->subscribers` i `channel_worker_sentinel->subscribers`, usuniecie z kolejek
- `ngx_http_push_stream_send_event` z typem `CLIENT_UNSUBSCRIBED`
- wyslanie ACK `{"unsubscribed":"..."}`

---

## Konfiguracja nginx

```nginx
http {
    push_stream_shared_memory_size 32M;

    server {
        listen 80;

        # Tylko odbieranie + dynamic sub/unsub, bez publish
        location ~ /sub/(.*) {
            push_stream_subscriber websocket;
            push_stream_channels_path $1;
            push_stream_websocket_allow_resubscribe             on;
            push_stream_websocket_allow_publish                 off;
            push_stream_websocket_max_channels_per_connection   20;
            push_stream_message_template '{"id":~id~,"channel":"~channel~","text":~text~,"event":"~event-id~"}';
        }

        # Pelna dwukierunkowosc
        location ~ /sub-full/(.*) {
            push_stream_subscriber websocket;
            push_stream_channels_path $1;
            push_stream_websocket_allow_resubscribe on;
            push_stream_websocket_allow_publish     on;
        }
    }
}
```

---

## Przyklad uzycia w JavaScript

```javascript
const ws = new WebSocket('ws://localhost/sub/matchA');

ws.onopen = () => {
    // subscribe do matchB z historia od ostatniego event_id
    ws.send('+matchB:goal_5');

    // subscribe do matchC bez historii
    ws.send('+matchC');

    // opusc matchA
    ws.send('-matchA');
};

ws.onmessage = (e) => {
    const data = JSON.parse(e.data);

    if (data.subscribed) {
        console.log('Dodano kanal:', data.subscribed);
        return;
    }
    if (data.unsubscribed) {
        console.log('Usunieto kanal:', data.unsubscribed);
        return;
    }
    if (data.error) {
        console.warn('Blad:', data.error, data.channel);
        return;
    }

    // normalna wiadomosc
    console.log('[' + data.channel + ']', data.text);
    lastEventId[data.channel] = data.event;
};

// dynamiczne zarzadzanie kanalami w czasie trwania polaczenia
function subscribe(channel, fromEventId) {
    ws.send(fromEventId ? '+' + channel + ':' + fromEventId : '+' + channel);
}

function unsubscribe(channel) {
    ws.send('-' + channel);
}
```

---

## Statystyki

Kazda subskrypcja dodana przez `+channel` jest liczona w statystykach
identycznie jak subskrypcja z URL — `channel->subscribers` i
`worker->subscribers` sa inkrementowane w `assing_subscription_to_channel`.
Unsubscribe przez `-channel` dekrementuje te liczniki natychmiast.

Endpoint `/channels-stats?id=matchB` pokazuje aktualna liczbe subskrybentow
po kazdej operacji sub/unsub.

---

## Pliki patcha

```
resubscribe_patch/
  resubscribe_module_h.patch         patch dla include/ngx_http_push_stream_module.h
  resubscribe_setup_c.patch          patch dla src/ngx_http_push_stream_module_setup.c
  resubscribe_subscriber_c.patch     patch dla src/ngx_http_push_stream_module_subscriber.c
  resubscribe_websocket_c.patch      patch dla src/ngx_http_push_stream_module_websocket.c
  ngx_http_push_stream_module.h                (gotowy plik)
  ngx_http_push_stream_module_setup.c          (gotowy plik)
  ngx_http_push_stream_module_subscriber.c     (gotowy plik)
  ngx_http_push_stream_module_websocket.c      (gotowy plik)
```

### Aplikowanie

```bash
cd nginx-push-stream-module
patch -p1 include/ngx_http_push_stream_module.h            < resubscribe_patch/resubscribe_module_h.patch
patch -p1 src/ngx_http_push_stream_module_setup.c          < resubscribe_patch/resubscribe_setup_c.patch
patch -p1 src/ngx_http_push_stream_module_subscriber.c     < resubscribe_patch/resubscribe_subscriber_c.patch
patch -p1 src/ngx_http_push_stream_module_websocket.c      < resubscribe_patch/resubscribe_websocket_c.patch
```
