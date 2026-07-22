# ngx_http_upstream_healthcheck_module

Active health checks for nginx open source. No patch to the nginx source required: the module hooks into the load-balancing chain (round robin, least_conn, ...) by wrapping `peer.init` / `peer.get` / `peer.free`.

Tested with nginx 1.26.2.

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

## Build

```sh
# requires --with-http_ssl_module if you want to use "ssl" in the healthcheck directive
./configure --with-http_ssl_module --add-module=/path/to/ngx_healthcheck [other options...]
make && make install
```

Also works as a dynamic module:

```sh
./configure --with-http_ssl_module --add-dynamic-module=/path/to/ngx_healthcheck
make modules
# then in nginx.conf:
# load_module modules/ngx_http_upstream_healthcheck_module.so;
```

Body matching requires PCRE to be built in (included automatically unless you use `--without-http_rewrite_module`, which excludes it).

With the bundled Makefile, `make` already builds with `--with-http_ssl_module` by default.

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

## Architecture (in brief)

1. **Parse config** — the `healthcheck` directive registers a shm zone
   (default size 128 KB, or whatever `healthcheck_shm_size` gave if it
   precedes the directive) and replaces `uscf->peer.init_upstream` with its
   own wrapper, saving the original. `healthcheck_match` uses nginx's
   standard technique for custom blocks (`cf->handler`/`cf->handler_conf`,
   the same one `types { }` uses in the core).
2. **Init upstream** — the wrapper calls the original init (round robin by
   default), resolves `match=NAME` to the matching `healthcheck_match` block,
   creates the SSL client context (`ngx_ssl_create` with `NGX_SSL_CLIENT`) if
   `ssl` is active; if `resolve` is active, it checks that the upstream has
   `zone` (otherwise a config error) and, for every peer with a hostname (not
   a literal IP) in the original text of `server`, extracts host and port for
   periodic re-resolution. It then enumerates the peers and registers them in
   a global array (each entry also carries a pointer to the srv_conf of the
   upstream it belongs to, used later to isolate state between different
   upstreams, and pointers to the peer's round-robin structures, used by
   `resolve`'s lock); it also replaces `us->peer.init`.
3. **Init shm zone** — allocates an array of states (`down`, fall/rise
   counters, stats, plus an `ngx_rwlock` per entry) in the zone's slab; on
   reload it copies the previous state if the number of peers matches.
4. **Worker 0** — in `init_process` it arms a timer per peer (with a random
   initial offset to spread out the probes). On each expiry: if `keepalive`
   is active and the previous cycle left an open, idle connection, it's
   reused directly (moving straight to sending the request); otherwise
   `ngx_event_connect_peer` → (if `ssl`, asynchronous TLS handshake with
   `ngx_ssl_handshake`, the same mechanism `proxy_pass` uses toward an https
   backend, replaying the TLS session cached from the previous cycle to
   shorten the handshake, and optional hostname verification via
   `ngx_ssl_check_host`) → send GET → read the response, which with
   `keepalive` stops as soon as headers + `Content-Length` have been fully
   received instead of waiting for the connection to close → evaluate
   against the match (status range + body regex) → update counters/flags in
   shm under `ngx_rwlock_wlock`. At the end of the cycle: with `keepalive`
   and a response correctly framed via `Content-Length` (and without
   `Connection: close`), the connection stays open with a dedicated handler
   that closes it if the backend interrupts it while idle; otherwise (or
   without `keepalive`) the connection is closed and, if `ssl`, the
   negotiated TLS session is saved for the next (new) handshake.
5. **Request time** — the wrapped `peer.get` calls the original get and, if
   the returned peer is marked down in shm (read under `ngx_rwlock_rlock`,
   and only among entries of the same upstream), discards it with
   `free(NGX_PEER_FAILED)` and retries, up to `NGX_BUSY` if none are left.
6. **Worker 0, re-resolution** — if `resolve` is active, a second per-peer
   timer (independent of the probe one) invokes `ngx_resolve_start`/
   `ngx_resolve_name` on the original hostname. On response, among the
   returned addresses the first one of the same family as the peer's current
   one is chosen; if different, only the address bytes (not the whole
   struct, not a pointer) are overwritten in place under the round-robin
   peer's lock (`ngx_http_upstream_rr_peer_lock`) — since that memory lives
   in the `zone`'s shm, every worker sees it updated immediately, with no
   need to propagate it explicitly. A resolution failure (timeout, NXDOMAIN)
   simply leaves the last known address in use.

## Known limitations

- Doesn't support dynamically adding/removing peers from an upstream: nginx
  open source offers no native mechanism to change the *number* of backends
  without `nginx -s reload` (that remains a limitation of the OSS platform,
  not of this module). What **is** supported, via `resolve` (see above and
  the "Resolved limitations" section), is following the *address* change of
  an already-existing peer when it's configured with a hostname instead of a
  literal IP — the most common practical case of a "dynamic upstream"
  (a backend behind a cloud load balancer, a k8s service, etc.).

### Limitations resolved in this revision

- **Isolation between different upstreams**: a peer's state is now keyed on
  `(upstream, sockaddr)` instead of just sockaddr, so two different upstreams
  with the same ip:port no longer share health state.
- **Synchronization of state in shm**: every entry has its own `ngx_rwlock`
  (the same primitive nginx's core uses for shared upstream state); readers
  (peer selection, status page) and the writer (worker 0) are now formally
  synchronized.
- **TLS hostname verification**: with `ssl_verify=on` and `ssl_name=` set,
  besides chain verification it's also checked that the backend's
  certificate actually covers that hostname (via `ngx_ssl_check_host`, the
  same mechanism `proxy_pass` uses toward an https upstream). Without
  `ssl_name` there's no meaningful hostname to check against (the peer is
  identified by ip:port) and only chain verification is done, as before.
- **TLS session resumption**: when `keepalive` is absent, the session
  negotiated by an `ssl` probe is cached per-peer (in worker 0's memory, not
  in shm) and replayed on the next cycle via `SSL_set_session`; if the
  backend accepts it, the next handshake is abbreviated instead of full.
  With `keepalive` this isn't even needed: there's no new handshake at all,
  because it's the same TLS connection that stays open.
- **Resizable shm zone**: no longer fixed at 128 KB; the new
  `healthcheck_shm_size` directive lets you grow it for upstreams with many
  peers. It must precede any `healthcheck` directive in the config file,
  because the zone is created at the first occurrence encountered while
  parsing.
- **Reusing the TCP connection between one cycle and the next**: with the
  new `keepalive` directive, the probe sends `Connection: keep-alive` and,
  if the backend responds with an explicit `Content-Length` and without
  `Connection: close`, the connection (TCP and, with `ssl`, TLS too) stays
  open and is reused on the next cycle — no new connect, no new handshake. A
  dedicated handler watches the connection while it's idle: if the backend
  closes it anyway (idle timeout, restart, etc.) the module notices and
  reconnects from scratch on the next probe, marking that single cycle as
  failed if needed but without requiring manual intervention. It remains
  opt-in (default `off`) because it changes the probe's semantics: a backend
  that's down only on the connect path (closed port, firewall) may stay
  temporarily invisible until the reused connection is actually closed by
  someone.
- **DNS re-resolution of peers** (`resolve` + `healthcheck_resolver`): a peer
  configured with a hostname is periodically re-resolved (every 30s by
  default, `resolve_interval=`) using `ngx_resolver`, the same asynchronous
  resolver used for `proxy_pass` toward a variable name. If the address
  changes, it's updated **in place** in the upstream's shared memory (hence
  the `zone` requirement), visible to every worker the instant it happens —
  without reallocating or moving pointers, so as not to reintroduce the
  cross-worker visibility problem `zone` solves. It doesn't add/remove peers
  (that limitation remains, see above): a `server` still amounts to one
  fixed slot, re-resolution only updates its contents. Verified end-to-end
  with a test DNS resolver: a peer goes from UP to DOWN and back exactly
  following the address change returned by DNS, including the safe fallback
  when the new answer is of a different family (IPv4/IPv6) than the peer's
  original one.

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
