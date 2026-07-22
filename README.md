# ngx_http_upstream_healthcheck_module

Active health checks for nginx open source. No patch to the nginx source required: the module hooks into the load-balancing chain (round robin, least_conn, ...) by wrapping `peer.init` / `peer.get` / `peer.free`.

Tested with Ubuntu 26/04 and nginx 1.28.3.

## Features

- Active, periodic HTTP (or HTTPS) probes against every server of the upstream, independent of real traffic
- `fall` / `rise` thresholds with state shared in shared memory across all workers, synchronized with `ngx_rwlock` and keyed by `(upstream, sockaddr)` (different upstreams with the same ip:port don't share state)
- Probing runs from a single worker (worker 0) so check traffic isn't multiplied
- Peers marked DOWN are excluded from selection; if all are down → 502 (`no live upstreams`)
- State preserved across reloads (`nginx -s reload`) if the peer set doesn't change
- Configurable shm zone size (`healthcheck_shm_size`, default 128 KB)
- **Optional keepalive** (`keepalive`): reuses the same TCP/TLS connection between one probe cycle and the next instead of reconnecting every time, with automatic, transparent reconnection if the backend closes it
- **Optional DNS re-resolution** (`resolve`): if a peer is configured with a hostname (not a literal IP), the module periodically re-resolves it and updates the actual address used for probes and real traffic, without `nginx -s reload` — requires `healthcheck_resolver` and `zone` on the upstream (see below)
- **TLS to backends**: the probe can establish a real TLS client connection (same mechanism as `proxy_pass` to an https upstream), with SNI support (`ssl_name=`), certificate verification (`ssl_verify=on` + `ssl_trusted_certificate=`) and explicit hostname verification when `ssl_name=` is set; the TLS session is cached per peer and replayed on subsequent probes to shorten the handshake
- **Body match**: a `healthcheck_match` block lets you require a narrower status range than the default and/or a regex against the response body (e.g. `{"status":"ok"}`)
- Plain-text status page (`healthcheck_status`)
- UP/DOWN transitions logged at `warn` level in the error log

## Installation

### Using the bundled Makefile (recommended)

```sh
git clone <this-repo-url> ngx_http_upstream_healthcheck_module
cd ngx_http_upstream_healthcheck_module
make
```

`make` auto-detects the nginx already installed on the system (version and
build options, via `nginx -V`, whichever way it got there: apt, dnf, brew,
a manual build), downloads a matching nginx source tree once (cached in
`build/`, untouched by subsequent builds), compiles the module as a dynamic
module against it, and copies the result to
`dist/ngx_http_upstream_healthcheck_module.so`.

Then load it in your `nginx.conf`, at the very top (before `events {}`):

```nginx
load_module /path/to/dist/ngx_http_upstream_healthcheck_module.so;
```

and reload:

```sh
sudo nginx -t && sudo nginx -s reload
```

See `make help` for the other targets (`test`, `clean`, `distclean`).

## Integration example

A minimal but complete `nginx.conf` wiring the module into a real reverse
proxy: two backends, an HTTP health probe with body matching, and a status
page.

```nginx
load_module /opt/nginx-modules/ngx_http_upstream_healthcheck_module.so;

worker_processes auto;
events { worker_connections 1024; }

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

    server {
        listen 80;

        location / {
            proxy_pass http://backend;
        }

        location /hc-status {
            healthcheck_status;
        }
    }
}
```

Start (or reload) nginx, then:

```sh
curl http://127.0.0.1/hc-status
```
```
upstream healthcheck status
---------------------------
backend  peer=10.0.0.1:8080  status=up  fails=0  last_fails=0
backend  peer=10.0.0.2:8080  status=up  fails=0  last_fails=0
```

If a backend goes down, `fall=3` consecutive failed probes mark it DOWN and
nginx stops routing traffic to it; `rise=2` consecutive successes bring it
back once it recovers — no reload needed either way.

## Configuration

### Simple health check (status code only)

```nginx
upstream backend {
    server 10.0.0.1:8080;
    server 10.0.0.2:8080;

    healthcheck interval=5000 timeout=2000 fall=3 rise=2 uri=/health;
}
```

### Health check with body match

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

### Health check with a reused connection (keepalive)

```nginx
upstream backend {
    server 10.0.0.1:8080;
    server 10.0.0.2:8080;

    healthcheck interval=5000 timeout=2000 fall=3 rise=2
                uri=/health keepalive;
}
```

The backend must respond with `Content-Length` and without `Connection: close`
for the connection to actually be reused; otherwise the module still works,
simply without the benefit of reuse (it behaves as if `keepalive` were absent
for that cycle).

### Health check with DNS re-resolution (resolve)

```nginx
http {
    healthcheck_resolver 10.0.0.53 valid=10s;

    upstream backend {
        zone backend_zone 256k;

        server backend.service.consul:8080;

        healthcheck interval=5000 timeout=2000 fall=3 rise=2
                    uri=/health resolve resolve_interval=15000;
    }
}
```

- `healthcheck_resolver` (http context, once) configures the DNS resolver
  used for `resolve`; it accepts the same syntax as the core nginx `resolver`
  directive (one or more addresses, optional `valid=`).
- `zone` is **mandatory** on the upstream when using `resolve`: without it,
  a peer's address would live in each worker's private memory and the update
  made by worker 0 (the only one running probes) would never be visible to
  the others — with `zone` the peer list lives in shared memory instead, so
  every worker sees the update the instant it happens.
- Works for peers configured with a **hostname** (`backend.service.consul:8080`);
  a peer with a literal IP (`10.0.0.1:8080`) has nothing to re-resolve and
  `resolve` has no effect on it (logged at `info` level, not an error).
- Updates the address **in place**: the number of peers stays whatever was
  fixed at parse time (one `server` = one slot), only the IP that slot points
  to changes. So it's not possible to add or remove backends at runtime, only
  to follow their IP changing — that's the real limitation of the OSS
  platform discussed further below.
- If DNS returns an address of a different family than the peer's original
  one (e.g. AAAA for a peer configured via an A record), the update is
  ignored and the last known compatible address stays in use.
- If a resolution fails (timeout, NXDOMAIN, unreachable resolver) the peer
  keeps its last known address; a warning is logged but it isn't treated as
  a probe failure.

### TLS health check to backends

```nginx
upstream backend {
    server 10.0.0.1:8443;
    server 10.0.0.2:8443;

    healthcheck interval=5000 timeout=2000 fall=3 rise=2
                uri=/health
                ssl ssl_verify=off ssl_name=backend.internal.example.com;
}
```

With certificate verification:

```nginx
healthcheck interval=5000 fall=3 rise=2 uri=/health
            ssl ssl_verify=on
            ssl_trusted_certificate=/etc/nginx/ca-backend.pem
            ssl_name=backend.internal.example.com;
```

### Everything together

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

### `healthcheck` directive (context: upstream)

| Parameter                      | Default | Meaning                                                         |
|--------------------------------|---------|------------------------------------------------------------------|
| `interval=`                    | 5000    | ms between one probe and the next (per peer)                    |
| `timeout=`                     | 2000    | ms timeout on connect/handshake/send/recv of the probe            |
| `fall=`                        | 3       | consecutive failures before marking DOWN                        |
| `rise=`                        | 2       | consecutive successes before marking UP again                   |
| `uri=`                         | /       | path of the probe GET request                                   |
| `match=`                       | —       | name of a `healthcheck_match` block (see below)                  |
| `keepalive`                    | off     | reuses the TCP connection between one cycle and the next (see below) |
| `resolve`                      | off     | periodically re-resolves via DNS peers configured with a hostname (see below) |
| `resolve_interval=`            | 30000   | ms between one re-resolution and the next (only with `resolve`) |
| `ssl`                          | off     | runs the probe over a TLS connection (requires `--with-http_ssl_module`) |
| `ssl_verify=on\|off`           | off     | verifies the backend's certificate                               |
| `ssl_name=`                    | —       | SNI / hostname sent in the ClientHello, and the hostname expected during certificate verification when `ssl_verify=on` |
| `ssl_trusted_certificate=`     | —       | path to the CA file for `ssl_verify=on`                          |

Without `keepalive`, the probe is a `GET <uri> HTTP/1.0` with `Connection: close`:
a new connection (and, with `ssl`, a TLS handshake, shortened when possible
thanks to the session cached from the previous cycle) is opened on every
interval. With `keepalive`, the probe becomes `GET <uri> HTTP/1.1` with
`Connection: keep-alive` and the TCP connection is kept open between one
cycle and the next, eliminating both the connect cost and the TLS handshake
cost — provided the backend responds with an explicit `Content-Length`
(needed to know where the response ends without waiting for the backend to
close) and doesn't itself ask for the connection to close
(`Connection: close`); if either condition isn't met, or if the backend
closes the connection anyway while it's idle between two probes, the module
notices and reconnects from scratch on the next cycle, with no manual
intervention needed. Without `match=`, any `2xx` or `3xx` status counts as
success.

### `healthcheck_match NAME { ... }` block (context: http)

```nginx
healthcheck_match NAME {
    status 200-299;      # or: status 200;  (single code)
    body ~ "PCRE regex";  # optional
}
```

- `status` accepts a single code (`status 200;`) or a range (`status 200-299;`). Default if omitted: `200-399`.
- `body ~ "regex"` is optional; if present, the response body must match the regex (PCRE) in addition to satisfying the status range.
- A `match` is referenced by one or more `healthcheck` directives via `match=NAME`.

### `healthcheck_status` directive (context: location)

Exposes the state as plain text:

```
upstream healthcheck status
---------------------------
backend  peer=10.0.0.1:8080  status=up    fails=0  last_fails=0
backend  peer=10.0.0.2:8080  status=DOWN  fails=17 last_fails=0
```

- `fails` counts failures **since the last success**: every successful probe
  resets it to zero. It isn't a historical total, but "how many consecutive
  failures has the peer accumulated in the current negative streak" (0 if
  the last probe went well).
- `last_fails` keeps the value `fails` had right before that last reset: it
  answers "how many consecutive failures were there in the last negative
  streak, before the peer started responding well again". It stays `0` until
  the peer has failed at least once since startup.

### `healthcheck_shm_size` directive (context: http)

```nginx
http {
    healthcheck_shm_size 1m;   # default: 128k

    upstream backend { ... healthcheck ...; }
}
```

Sizes the shared memory zone used for peer state (default 128 KB, enough for
several thousand peers). It must be placed **before** any `healthcheck`
directive, because the zone is created at the first occurrence of
`healthcheck` encountered while parsing; if it appears after, parsing fails
with an explicit error instead of being silently ignored.

## Known limitations

- Doesn't support dynamically adding/removing peers from an upstream: nginx
  open source offers no native mechanism to change the *number* of backends
  without `nginx -s reload` (that remains a limitation of the OSS platform,
  not of this module). What **is** supported, via `resolve`, is following
  the *address* change of an already-existing peer when it's configured
  with a hostname instead of a literal IP — the most common practical case
  of a "dynamic upstream" (a backend behind a cloud load balancer, a k8s
  service, etc.).

## Quick test (plain HTTP)

```sh
python3 -m http.server 9001 &
python3 -m http.server 9002 &
nginx -c test/nginx.conf
curl http://127.0.0.1:8888/hc-status
kill %2          # after fall*interval, peer 9002 becomes DOWN
curl http://127.0.0.1:8888/   # all traffic goes to 9001
```

## Quick test (TLS + body match)

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

`curl http://.../hc-status` should show `status=up` for peer 9201.

## License

BSD-2-Clause. See [LICENSE](LICENSE).
