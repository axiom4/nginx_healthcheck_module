# ngx_http_upstream_healthcheck_module

Active health check per nginx open source. Nessuna patch al sorgente nginx richiesta: il modulo si aggancia alla catena di bilanciamento (round robin, least_conn, ...) wrappando `peer.init` / `peer.get` / `peer.free`.

Testato con nginx 1.26.2.

## Funzionalità

- Probe HTTP (o HTTPS) attivi e periodici verso ogni server dell'upstream, indipendenti dal traffico reale
- Soglie `fall` / `rise` con stato condiviso in shared memory tra tutti i worker
- Probing eseguito da un solo worker (worker 0) per non moltiplicare il traffico di check
- I peer marcati DOWN vengono esclusi dalla selezione; se tutti sono down → 502 (`no live upstreams`)
- Stato preservato attraverso i reload (`nginx -s reload`) se il set di peer non cambia
- **TLS verso i backend**: il probe può stabilire una connessione TLS client vera e propria (stesso meccanismo di `proxy_pass` verso un upstream https), con supporto a SNI (`ssl_name=`) e verifica del certificato (`ssl_verify=on` + `ssl_trusted_certificate=`)
- **Match del body**: un blocco `healthcheck_match` permette di richiedere un range di status code più stretto del default e/o una regex sul body della risposta (es. `{"status":"ok"}`)
- Pagina di stato in plain text (`healthcheck_status`)
- Transizioni UP/DOWN loggate a livello `warn` nell'error log

## Build

```sh
# richiede --with-http_ssl_module se si vuole usare "ssl" nella direttiva healthcheck
./configure --with-http_ssl_module --add-module=/path/to/ngx_healthcheck [altre opzioni...]
make && make install
```

Funziona anche come modulo dinamico:

```sh
./configure --with-http_ssl_module --add-dynamic-module=/path/to/ngx_healthcheck
make modules
# poi in nginx.conf:
# load_module modules/ngx_http_upstream_healthcheck_module.so;
```

Il body-match richiede la libreria PCRE compilata (viene inclusa automaticamente a meno che non si usi `--without-http_rewrite_module`, che la esclude).

Con il Makefile allegato, `make` compila già con `--with-http_ssl_module` di default.

## Configurazione

### Health check semplice (solo status code)

```nginx
upstream backend {
    server 10.0.0.1:8080;
    server 10.0.0.2:8080;

    healthcheck interval=5000 timeout=2000 fall=3 rise=2 uri=/health;
}
```

### Health check con match del body

```nginx
http {
    healthcheck_match ok {
        status 200-299;
        body ~ "\"status\"\s*:\s*\"ok\"";
    }

    upstream backend {
        server 10.0.0.1:8080;
        server 10.0.0.2:8080;

        healthcheck interval=5000 timeout=2000 fall=3 rise=2
                    uri=/health match=ok;
    }
}
```

### Health check TLS verso i backend

```nginx
upstream backend {
    server 10.0.0.1:8443;
    server 10.0.0.2:8443;

    healthcheck interval=5000 timeout=2000 fall=3 rise=2
                uri=/health
                ssl ssl_verify=off ssl_name=backend.internal.example.com;
}
```

Con verifica del certificato:

```nginx
healthcheck interval=5000 fall=3 rise=2 uri=/health
            ssl ssl_verify=on
            ssl_trusted_certificate=/etc/nginx/ca-backend.pem
            ssl_name=backend.internal.example.com;
```

### Tutto insieme

```nginx
http {
    healthcheck_match ok {
        status 200-299;
        body ~ "\"status\"\s*:\s*\"ok\"";
    }

    upstream backend {
        server 10.0.0.1:8443;
        server 10.0.0.2:8443;

        healthcheck interval=5000 timeout=2000 fall=3 rise=2
                    uri=/health match=ok
                    ssl ssl_verify=off ssl_name=backend.internal;
    }
}

server {
    listen 80;
    location / { proxy_pass https://backend; }
    location /hc-status { healthcheck_status; }
}
```

### Direttiva `healthcheck` (contesto: upstream)

| Parametro                     | Default | Significato                                                  |
|--------------------------------|---------|----------------------------------------------------------------|
| `interval=`                    | 5000    | ms tra un probe e il successivo (per peer)                    |
| `timeout=`                     | 2000    | ms di timeout su connect/handshake/send/recv del probe          |
| `fall=`                        | 3       | fallimenti consecutivi prima di marcare DOWN                   |
| `rise=`                        | 2       | successi consecutivi prima di rimarcare UP                     |
| `uri=`                         | /       | path della richiesta GET di probe                              |
| `match=`                       | —       | nome di un blocco `healthcheck_match` (vedi sotto)              |
| `ssl`                          | off     | esegue il probe su una connessione TLS (richiede `--with-http_ssl_module`) |
| `ssl_verify=on\|off`           | off     | verifica il certificato del backend                            |
| `ssl_name=`                    | —       | SNI / hostname inviato nel ClientHello                         |
| `ssl_trusted_certificate=`     | —       | path del file CA per `ssl_verify=on`                            |

Il probe è un `GET <uri> HTTP/1.0` con `Connection: close`. Senza `match=`, qualunque status `2xx` o `3xx` conta come successo.

### Blocco `healthcheck_match NAME { ... }` (contesto: http)

```nginx
healthcheck_match NAME {
    status 200-299;      # oppure: status 200;  (singolo codice)
    body ~ "regex PCRE";  # opzionale
}
```

- `status` accetta un singolo codice (`status 200;`) o un range (`status 200-299;`). Default se omesso: `200-399`.
- `body ~ "regex"` è opzionale; se presente, il body della risposta deve matchare la regex (PCRE) oltre a rispettare il range di status.
- Un `match` viene referenziato da una o più direttive `healthcheck` tramite `match=NAME`.

### Direttiva `healthcheck_status` (contesto: location)

Espone lo stato in plain text:

```
upstream healthcheck status
---------------------------
backend  peer=10.0.0.1:8080  status=up    checks=1042  fails=0
backend  peer=10.0.0.2:8080  status=DOWN  checks=1042  fails=17
```

## Architettura (in breve)

1. **Parse config** — la direttiva `healthcheck` registra una shm zone (128 KB) e
   sostituisce `uscf->peer.init_upstream` con il proprio wrapper, salvando l'originale.
   `healthcheck_match` usa la tecnica standard di nginx per blocchi custom
   (`cf->handler`/`cf->handler_conf`, la stessa di `types { }` nel core).
2. **Init upstream** — il wrapper chiama l'init originale (round robin di default),
   risolve `match=NAME` nel blocco `healthcheck_match` corrispondente, crea il
   contesto SSL client (`ngx_ssl_create` con `NGX_SSL_CLIENT`) se `ssl` è attivo,
   poi enumera i peer e li registra in un array globale; sostituisce anche `us->peer.init`.
3. **Init shm zone** — alloca un array di stati (`down`, contatori fall/rise, statistiche)
   nella slab della zona; su reload copia lo stato precedente se il numero di peer coincide.
4. **Worker 0** — in `init_process` arma un timer per peer (con offset iniziale casuale
   per distribuire i probe). Ogni scadenza: `ngx_event_connect_peer` → (se `ssl`,
   handshake TLS asincrono con `ngx_ssl_handshake`, stesso meccanismo usato da
   `proxy_pass` verso backend https) → invio GET → valutazione della risposta
   contro il match (status range + regex sul body) → aggiornamento contatori/flag in shm.
5. **Request time** — il `peer.get` wrappato chiama il get originale e, se il peer
   restituito è marcato down in shm, lo scarta con `free(NGX_PEER_FAILED)` e riprova,
   fino a `NGX_BUSY` se non resta nessuno.

## Limiti noti

- Confronto stato peer per sockaddr: se due upstream diversi contengono lo stesso ip:porta,
  condividono lo stato di salute (spesso è ciò che si vuole, ma va saputo)
- Non supporta upstream con `resolve`/servizi dinamici: il set di peer è quello del parse
- Lettura del flag `down` senza lock: è una singola word aggiornata da un solo writer
  (worker 0), accettabile su piattaforme con store atomici naturali, ma non formalmente
  sincronizzata
- La shm zone è unica e dimensionata staticamente (128 KB ≈ diverse migliaia di peer)
- Il probe non usa keepalive; una nuova connessione (e, se `ssl`, un nuovo handshake
  completo senza session resumption) viene aperta a ogni ciclo
- `ssl_verify=on` verifica solo la catena del certificato contro la CA indicata; non
  fa hostname verification esplicita oltre a quella implicita di OpenSSL con SNI

## Test rapido (HTTP semplice)

```sh
python3 -m http.server 9001 &
python3 -m http.server 9002 &
nginx -c test/nginx.conf
curl http://127.0.0.1:8888/hc-status
kill %2          # dopo fall*interval il peer 9002 diventa DOWN
curl http://127.0.0.1:8888/   # tutto il traffico va su 9001
```

## Test rapido (TLS + body match)

```sh
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
    -days 30 -nodes -subj "/CN=backend.internal"

python3 - <<'PY'
import http.server, ssl
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'{"status":"ok"}\n'
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
httpd = http.server.HTTPServer(("127.0.0.1", 9201), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain("cert.pem", "key.pem")
httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
httpd.serve_forever()
PY
```

```nginx
healthcheck_match ok {
    status 200-299;
    body ~ "\"status\"\s*:\s*\"ok\"";
}

upstream backend {
    server 127.0.0.1:9201;
    healthcheck interval=1000 timeout=800 fall=2 rise=2 uri=/
                match=ok ssl ssl_verify=off;
}
```

`curl http://.../hc-status` deve mostrare `status=up` per il peer 9201.
