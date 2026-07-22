#!/usr/bin/env python3
"""
End-to-end test suite for ngx_http_upstream_healthcheck_module.

Drives the system nginx binary (found in PATH, or via the NGINX_BIN
environment variable — same knob the top-level Makefile uses) loading the
dynamic module from dist/ngx_http_upstream_healthcheck_module.so via
load_module, against a set of throwaway Python backends (plain HTTP and
HTTPS). Asserts on the healthcheck_status page and on real proxied
requests.

The suite is organized to exercise every implemented feature along the
dimensions that matter operationally:
  - with TLS/certificates (ssl, ssl_verify, ssl_name) and without (plain
    HTTP, no certificates involved at all)
  - with the health check active on an upstream, and without it (an
    upstream nginx proxies to normally, untouched by this module)
  - probing with a monitoring URL whose response body is inspected
    (healthcheck_match + regex) vs. a bare status-only check (no match=,
    default 2xx/3xx acceptance, no body parsing)
  - keepalive connection reuse, DNS re-resolution (resolve), shm sizing,
    and cross-upstream state isolation

Usage:
    make test          # from the repo root: builds dist/*.so, then runs this
    cd test && python3 test_healthcheck.py   # if dist/*.so is already built

Requires cert.pem/key.pem in this directory (see `make test` in the
top-level Makefile, which generates them before running this script).
"""

import http.client
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import textwrap
import threading
import time

TEST_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(TEST_DIR)
CERT = os.path.join(TEST_DIR, "cert.pem")
KEY = os.path.join(TEST_DIR, "key.pem")
MODULE_SO = os.path.join(ROOT_DIR, "dist",
                          "ngx_http_upstream_healthcheck_module.so")

BASE_PORT = 19300  # high, unlikely-to-collide range for this test run


def find_nginx_binary():
    nginx_bin = os.environ.get("NGINX_BIN", "nginx")
    path = shutil.which(nginx_bin)
    if not path:
        sys.exit(f"'{nginx_bin}' not found in PATH — install nginx (with "
                  f"--with-compat support for load_module) or set NGINX_BIN")
    return path


NGINX_BIN = find_nginx_binary()


# ------------------------------------------------------------- backends

# protocol_version = HTTP/1.1 is required for the backend to honor
# Connection: keep-alive at all (BaseHTTPRequestHandler defaults to
# HTTP/1.0, which always closes) — needed for the keepalive test below;
# harmless for every other test since our probe always sends its own
# explicit Connection header regardless of what the backend would default to.
BACKEND_CODE = textwrap.dedent("""
    import http.server, socketserver

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            body = {body!r}
            self.send_response({status_code})
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            log_file = {log_file!r}
            if log_file:
                with open(log_file, "a") as f:
                    f.write(str(self.client_address[1]) + "\\n")

        def log_message(self, *a):
            pass

    httpd = http.server.HTTPServer(("127.0.0.1", {port}), H)
    {tls_setup}
    httpd.serve_forever()
""")

TLS_SETUP = textwrap.dedent("""
    import ssl
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain({cert!r}, {key!r})
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
""")


class Backend:
    """A throwaway plain-HTTP or HTTPS backend serving a fixed body/status."""

    def __init__(self, port, body='{"status":"ok"}\n', status_code=200,
                 tls=True, log_file=None):
        self.port = port
        tls_setup = TLS_SETUP.format(cert=CERT, key=KEY) if tls else ""
        code = BACKEND_CODE.format(port=port, body=body.encode(),
                                    status_code=status_code,
                                    tls_setup=tls_setup, log_file=log_file)
        self.proc = subprocess.Popen(
            [sys.executable, "-c", code],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self._wait_listening()

    def _wait_listening(self, timeout=5):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.2)
                try:
                    s.connect(("127.0.0.1", self.port))
                    return
                except OSError:
                    time.sleep(0.05)
        raise RuntimeError(f"backend on port {self.port} never came up")

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


class MiniDNS:
    """
    Minimal UDP DNS stub used by the "resolve" test: answers A queries with
    whatever IP is currently set in .ip (mutable at runtime, to simulate a
    backend's address changing), and empty (no records) for anything else
    (notably AAAA, so it doesn't trip nginx's resolver validation, which
    rejects an A record answering a query it didn't ask an A question for).
    """

    def __init__(self, port, ip):
        self.port = port
        self.ip = ip
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.bind(("127.0.0.1", port))
        self._sock.settimeout(0.2)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    @staticmethod
    def _qtype(query):
        i = 12
        while query[i] != 0:
            i += 1 + query[i]
        i += 1
        qtype = (query[i] << 8) | query[i + 1]
        return qtype, i + 4

    def _build(self, query, ip):
        import struct
        tid = query[:2]
        qdcount = query[4:6]
        qtype, qend = self._qtype(query)
        question = query[12:qend]

        if qtype != 1:  # only answer A queries
            header = tid + b"\x81\x80" + qdcount + b"\x00\x00\x00\x00\x00\x00"
            return header + question

        header = tid + b"\x81\x80" + qdcount + b"\x00\x01\x00\x00\x00\x00"
        answer = (b"\xc0\x0c" + b"\x00\x01" + b"\x00\x01"
                  + struct.pack("!I", 1) + b"\x00\x04"
                  + socket.inet_aton(ip))
        return header + question + answer

    def _serve(self):
        while not self._stop.is_set():
            try:
                data, addr = self._sock.recvfrom(512)
            except socket.timeout:
                continue
            try:
                self._sock.sendto(self._build(data, self.ip), addr)
            except Exception:
                pass

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=2)
        self._sock.close()


# ------------------------------------------------------------------ nginx

class Nginx:
    """One throwaway nginx instance rooted at its own temp prefix dir."""

    def __init__(self, conf_body):
        self.dir = tempfile.mkdtemp(prefix="ngx_hc_test_")
        for sub in ("logs", "client_body_temp", "proxy_temp"):
            os.makedirs(os.path.join(self.dir, sub), exist_ok=True)

        conf_path = os.path.join(self.dir, "nginx.conf")
        with open(conf_path, "w") as f:
            f.write(conf_body)

        rc = subprocess.run([NGINX_BIN, "-p", self.dir, "-c", "nginx.conf",
                             "-t"],
                            capture_output=True, text=True)
        if rc.returncode != 0:
            self._cleanup()
            raise RuntimeError(f"nginx -t failed:\n{rc.stderr}")

        subprocess.run([NGINX_BIN, "-p", self.dir, "-c", "nginx.conf"],
                       check=True, capture_output=True)

    def error_log(self):
        try:
            with open(os.path.join(self.dir, "logs", "error.log")) as f:
                return f.read()
        except OSError:
            return ""

    def stop(self):
        subprocess.run([NGINX_BIN, "-p", self.dir, "-c", "nginx.conf",
                        "-s", "stop"],
                       capture_output=True)
        self._cleanup()

    def _cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)


def http_get(port, path="/"):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        conn.request("GET", path)
        resp = conn.getresponse()
        return resp.status, resp.read().decode(errors="replace")
    finally:
        conn.close()


def wait_until(predicate, timeout=6, interval=0.2):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    return last


def wait_for_status(port, want):
    """want(status_body) -> truthy when the awaited condition is met."""
    return wait_until(
        lambda: (lambda s: s if want(s) else None)(http_get(port, "/hc-status")[1]))


def peer_line(status_body, needle):
    """The single healthcheck_status line whose peer=... contains needle."""
    for line in status_body.splitlines():
        if needle in line:
            return line
    return None


def hc_status_conf(listen_port, upstream_extra, healthcheck_line,
                    proxy_scheme="https", http_extra="", match_block=True):
    match = (
        'healthcheck_match ok {\n'
        '                status 200-299;\n'
        '                body ~ "\\"status\\"\\s*:\\s*\\"ok\\"";\n'
        '            }'
    ) if match_block else ""

    return textwrap.dedent(f"""
        load_module {MODULE_SO};

        worker_processes 1;
        error_log logs/error.log info;
        pid nginx.pid;
        events {{ worker_connections 128; }}

        http {{
            access_log off;
            {http_extra}

            {match}

            upstream backend {{
                {upstream_extra}
                {healthcheck_line}
            }}

            server {{
                listen 127.0.0.1:{listen_port};
                location / {{
                    proxy_pass {proxy_scheme}://backend;
                    proxy_ssl_verify off;
                }}
                location /hc-status {{
                    healthcheck_status;
                }}
            }}
        }}
    """)


# --------------------------------------------------------------- harness

_results = []


def check(name, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    _results.append((name, condition))
    print(f"[{status}] {name}" + (f" — {detail}" if detail and not condition
                                   else ""))


# =====================================================================
# 1. WITH certificates (TLS/ssl) vs WITHOUT (plain HTTP)
# =====================================================================

def test_tls_basic_up():
    """ssl + match: healthy HTTPS backend reaches status=up."""
    port = BASE_PORT
    b = Backend(port + 1, tls=True)
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};",
            "healthcheck interval=300 timeout=400 fall=2 rise=1 uri=/health "
            "match=ok ssl ssl_verify=off;"))
        status_line = wait_for_status(port, lambda s: "status=up" in s)
        check("tls: healthy https backend reaches status=up",
              status_line is not None and "status=up" in status_line,
              status_line)

        code, body = http_get(port, "/")
        check("tls: real request proxied successfully over https",
              code == 200 and "ok" in body, f"code={code} body={body!r}")
    finally:
        if ngx:
            ngx.stop()
        b.stop()


def test_plain_http_no_certificates():
    """No ssl at all: plain HTTP backend, no certificates involved."""
    port = BASE_PORT + 1
    b = Backend(port + 1, tls=False)
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};",
            "healthcheck interval=300 timeout=400 fall=2 rise=1 "
            "uri=/health match=ok;",
            proxy_scheme="http"))
        status_line = wait_for_status(port, lambda s: "status=up" in s)
        check("plain http: healthy backend (no certs) reaches status=up",
              status_line is not None and "status=up" in status_line,
              status_line)

        code, body = http_get(port, "/")
        check("plain http: real request proxied successfully",
              code == 200 and "ok" in body, f"code={code} body={body!r}")
    finally:
        if ngx:
            ngx.stop()
        b.stop()


# =====================================================================
# 2. WITH the health check active vs WITHOUT (upstream untouched)
# =====================================================================

def test_down_detection():
    """With the check active: an unreachable backend is marked DOWN."""
    port = BASE_PORT + 10
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};",
            "healthcheck interval=200 timeout=200 fall=2 rise=1 uri=/health "
            "match=ok ssl ssl_verify=off;"))
        status_line = wait_for_status(port, lambda s: "status=DOWN" in s)
        check("down: unreachable backend is marked DOWN",
              status_line is not None and "status=DOWN" in status_line,
              status_line)

        code, _ = http_get(port, "/")
        check("down: request to all-down upstream returns 502",
              code == 502, f"code={code}")
    finally:
        if ngx:
            ngx.stop()


def test_no_healthcheck_baseline():
    """Without the check: an upstream with no healthcheck directive at all
    must keep proxying normally, and the module must report nothing to
    monitor rather than getting in the way."""
    port = BASE_PORT + 11
    b = Backend(port + 1, tls=False)
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};",
            "",  # no healthcheck directive on this upstream at all
            proxy_scheme="http"))

        code, body = http_get(port, "/")
        check("no-check: upstream without healthcheck proxies normally",
              code == 200 and "ok" in body, f"code={code} body={body!r}")

        _, status_body = http_get(port, "/hc-status")
        check("no-check: status page reports nothing monitored",
              "no monitored peers" in status_body, status_body)
    finally:
        if ngx:
            ngx.stop()
        b.stop()


# =====================================================================
# 3. Monitoring URL + output (body) analysis vs bare status-only check
# =====================================================================

def test_body_match_rejects_wrong_body():
    """With match=: a monitoring URL whose body doesn't match the regex is
    rejected even though the status code alone would have passed."""
    port = BASE_PORT + 20
    good = Backend(port + 1, body='{"status":"ok"}\n')
    bad = Backend(port + 2, body='{"status":"ok2"}\n')  # doesn't match regex
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};\n                server 127.0.0.1:{port + 2};",
            "healthcheck interval=200 timeout=300 fall=2 rise=1 uri=/health "
            "match=ok ssl ssl_verify=off;"))

        status_body = wait_for_status(
            port, lambda s: s.count("status=up") == 1
            and s.count("status=DOWN") == 1)
        check("body match: mismatched body is marked DOWN, matching body up",
              status_body is not None, status_body)
    finally:
        if ngx:
            ngx.stop()
        good.stop()
        bad.stop()


def test_status_only_no_match_block():
    """Without match=: a bare status-only check accepts any 2xx/3xx body
    with no output analysis at all (the response here isn't even the JSON
    shape healthcheck_match would require elsewhere in this suite)."""
    port = BASE_PORT + 21
    b = Backend(port + 1, body="whatever, not inspected\n", tls=False)
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};",
            "healthcheck interval=300 timeout=400 fall=2 rise=1 uri=/health;",
            proxy_scheme="http"))
        status_line = wait_for_status(port, lambda s: "status=up" in s)
        check("status-only: unmatched body still passes a bare status check",
              status_line is not None and "status=up" in status_line,
              status_line)
    finally:
        if ngx:
            ngx.stop()
        b.stop()


# =====================================================================
# 4. TLS certificate verification variants
# =====================================================================

def test_ssl_verify_valid_chain():
    """ssl_verify=on with the matching CA: chain checks out, no ssl_name
    so only the chain (not the hostname) is verified."""
    port = BASE_PORT + 30
    b = Backend(port + 1, tls=True)
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};",
            "healthcheck interval=300 timeout=400 fall=2 rise=1 uri=/health "
            f"match=ok ssl ssl_verify=on ssl_trusted_certificate={CERT};"))
        status_line = wait_for_status(port, lambda s: "status=up" in s)
        check("ssl_verify=on: valid chain, no ssl_name, reaches status=up",
              status_line is not None and "status=up" in status_line,
              status_line)
    finally:
        if ngx:
            ngx.stop()
        b.stop()


def test_ssl_verify_hostname_match():
    """ssl_verify=on + ssl_name matching the certificate's CN: hostname
    verification passes."""
    port = BASE_PORT + 31
    b = Backend(port + 1, tls=True)
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};",
            "healthcheck interval=300 timeout=400 fall=2 rise=1 uri=/health "
            f"match=ok ssl ssl_verify=on ssl_trusted_certificate={CERT} "
            "ssl_name=backend.internal;"))
        status_line = wait_for_status(port, lambda s: "status=up" in s)
        check("ssl_verify=on: matching ssl_name reaches status=up",
              status_line is not None and "status=up" in status_line,
              status_line)
    finally:
        if ngx:
            ngx.stop()
        b.stop()


def test_ssl_verify_hostname_mismatch():
    """ssl_verify=on + ssl_name NOT matching the certificate's CN: hostname
    verification must reject it even though the chain itself is valid."""
    port = BASE_PORT + 32
    b = Backend(port + 1, tls=True)
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};",
            "healthcheck interval=200 timeout=300 fall=2 rise=1 uri=/health "
            f"match=ok ssl ssl_verify=on ssl_trusted_certificate={CERT} "
            "ssl_name=totally.wrong.example;"))
        status_line = wait_for_status(port, lambda s: "status=DOWN" in s)
        check("ssl_verify=on: mismatched ssl_name is marked DOWN",
              status_line is not None and "status=DOWN" in status_line,
              status_line)

        log = ngx.error_log()
        check("ssl_verify=on: hostname mismatch is logged explicitly",
              "doesn't cover hostname" in log, log[-400:])
    finally:
        if ngx:
            ngx.stop()
        b.stop()


# =====================================================================
# 5. keepalive: TCP/TLS connection reuse across probe cycles
# =====================================================================

def test_keepalive_reuses_connection():
    port = BASE_PORT + 40
    with tempfile.TemporaryDirectory() as d:
        log_file = os.path.join(d, "conns.log")
        b = Backend(port + 1, tls=True, log_file=log_file)
        ngx = None
        try:
            ngx = Nginx(hc_status_conf(
                port,
                f"server 127.0.0.1:{port + 1};",
                "healthcheck interval=150 timeout=300 fall=2 rise=1 "
                "uri=/health match=ok ssl ssl_verify=off keepalive;"))

            wait_for_status(port, lambda s: "status=up" in s)
            time.sleep(1.2)  # let several probe cycles happen

            with open(log_file) as f:
                client_ports = [line.strip() for line in f if line.strip()]

            check("keepalive: several probe cycles completed",
                  len(client_ports) >= 3, f"{len(client_ports)} cycles")
            check("keepalive: all cycles reused the same TCP connection",
                  len(set(client_ports)) == 1,
                  f"distinct source ports seen: {set(client_ports)}")
        finally:
            if ngx:
                ngx.stop()
            b.stop()


# =====================================================================
# 6. resolve: periodic DNS re-resolution
# =====================================================================

def test_resolve_dns_update():
    port = BASE_PORT + 50
    dns_port = port + 5
    backend_port = port + 1

    b = Backend(backend_port, tls=False)
    dns = MiniDNS(dns_port, ip="127.0.0.1")
    ngx = None
    try:
        conf = textwrap.dedent(f"""
            load_module {MODULE_SO};

            worker_processes 1;
            error_log logs/error.log info;
            pid nginx.pid;
            events {{ worker_connections 128; }}

            http {{
                access_log off;
                healthcheck_resolver 127.0.0.1:{dns_port};

                upstream backend {{
                    zone backend_zone 256k;
                    server localhost:{backend_port};
                    healthcheck interval=200 timeout=300 fall=2 rise=1
                                uri=/health resolve resolve_interval=300;
                }}

                server {{
                    listen 127.0.0.1:{port};
                    location / {{ proxy_pass http://backend; }}
                    location /hc-status {{ healthcheck_status; }}
                }}
            }}
        """)
        ngx = Nginx(conf)

        # "localhost" resolves at config-parse time (system resolver) to
        # both 127.0.0.1 and ::1: only the IPv4 peer (displayed as
        # "127.0.0.1:<port>", the address resolved at parse time — the
        # display name doesn't change even after "resolve" repoints the
        # underlying connection) can ever go up here, since our backend and
        # DNS stub are both IPv4-only.
        needle = f"127.0.0.1:{backend_port}"

        up = wait_for_status(
            port, lambda s: "status=up" in (peer_line(s, needle) or ""))
        check("resolve: peer starts up while DNS points at the backend",
              up is not None, peer_line(up, needle) if up else None)

        dns.ip = "127.0.0.3"  # routable, nothing listening there
        down = wait_for_status(
            port, lambda s: "status=DOWN" in (peer_line(s, needle) or ""))
        check("resolve: peer follows the DNS change and goes DOWN",
              down is not None, peer_line(down, needle) if down else None)

        log = ngx.error_log()
        check("resolve: address change is logged",
              "resolved to a new address" in log, log[-400:])

        dns.ip = "127.0.0.1"  # point back at the real backend
        up_again = wait_for_status(
            port, lambda s: "status=up" in (peer_line(s, needle) or ""))
        check("resolve: peer recovers once DNS points back at the backend",
              up_again is not None,
              peer_line(up_again, needle) if up_again else None)
    finally:
        if ngx:
            ngx.stop()
        dns.stop()
        b.stop()


# =====================================================================
# 7. Cross-upstream state isolation (same backend, different verdicts)
# =====================================================================

def test_upstream_isolation_same_backend():
    """Two upstreams pointing at the very same ip:port, with different
    match requirements against the one real backend, must not share
    health state — this is what (upstream, sockaddr) keying guarantees."""
    port = BASE_PORT + 60
    backend_port = port + 1
    b = Backend(backend_port, body='{"status":"ok"}\n')
    ngx = None
    try:
        conf = textwrap.dedent(f"""
            load_module {MODULE_SO};

            worker_processes 1;
            error_log logs/error.log info;
            pid nginx.pid;
            events {{ worker_connections 128; }}

            http {{
                access_log off;

                healthcheck_match ok {{
                    status 200-299;
                    body ~ "\\"status\\"\\s*:\\s*\\"ok\\"";
                }}
                healthcheck_match impossible {{
                    status 200-299;
                    body ~ "this-will-never-appear";
                }}

                upstream backend_a {{
                    server 127.0.0.1:{backend_port};
                    healthcheck interval=200 timeout=300 fall=2 rise=1
                                uri=/health match=ok ssl ssl_verify=off;
                }}
                upstream backend_b {{
                    server 127.0.0.1:{backend_port};
                    healthcheck interval=200 timeout=300 fall=2 rise=1
                                uri=/health match=impossible ssl ssl_verify=off;
                }}

                server {{
                    listen 127.0.0.1:{port};
                    location /hc-status {{ healthcheck_status; }}
                }}
            }}
        """)
        ngx = Nginx(conf)

        status_body = wait_for_status(
            port, lambda s: "backend_a" in s and "backend_b" in s
            and s.count("status=up") == 1 and s.count("status=DOWN") == 1)
        check("isolation: same ip:port scores differently per upstream",
              status_body is not None, status_body)

        if status_body:
            a_line = next(l for l in status_body.splitlines()
                          if l.startswith("backend_a"))
            b_line = next(l for l in status_body.splitlines()
                          if l.startswith("backend_b"))
            check("isolation: backend_a (match=ok) is up",
                  "status=up" in a_line, a_line)
            check("isolation: backend_b (match=impossible) is DOWN",
                  "status=DOWN" in b_line, b_line)
    finally:
        if ngx:
            ngx.stop()
        b.stop()


# =====================================================================
# 8. Configurable shm zone size
# =====================================================================

def test_shm_size_custom():
    port = BASE_PORT + 70
    b = Backend(port + 1, tls=False)
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};",
            "healthcheck interval=300 timeout=400 fall=2 rise=1 "
            "uri=/health match=ok;",
            proxy_scheme="http",
            http_extra="healthcheck_shm_size 32k;"))
        status_line = wait_for_status(port, lambda s: "status=up" in s)
        check("shm_size: custom healthcheck_shm_size still works",
              status_line is not None and "status=up" in status_line,
              status_line)
    finally:
        if ngx:
            ngx.stop()
        b.stop()


def test_last_fails_reset():
    port = BASE_PORT + 80
    ngx = None
    b = None
    try:
        # start down (no backend) to accumulate a few failures
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};",
            "healthcheck interval=150 timeout=150 fall=2 rise=1 uri=/health "
            "match=ok ssl ssl_verify=off;"))

        wait_for_status(port, lambda s: "status=DOWN" in s)
        time.sleep(0.6)  # accumulate a handful of consecutive failures

        b = Backend(port + 1)

        recovered = wait_for_status(port, lambda s: "status=up" in s)
        check("last_fails: peer recovers to status=up", recovered is not None,
              recovered)

        if recovered:
            fails_zero = "fails=0" in recovered
            last_fails_nonzero = "last_fails=0" not in recovered
            check("last_fails: fails resets to 0 on recovery", fails_zero,
                  recovered)
            check("last_fails: last_fails captures the prior failure streak",
                  last_fails_nonzero, recovered)
    finally:
        if ngx:
            ngx.stop()
        if b:
            b.stop()


def test_failover():
    port = BASE_PORT + 90
    b1 = Backend(port + 1)
    b2 = Backend(port + 2)
    ngx = None
    try:
        ngx = Nginx(hc_status_conf(
            port,
            f"server 127.0.0.1:{port + 1};\n                server 127.0.0.1:{port + 2};",
            "healthcheck interval=200 timeout=300 fall=2 rise=1 uri=/health "
            "match=ok ssl ssl_verify=off;"))

        wait_for_status(port, lambda s: f"127.0.0.1:{port + 1}" in s
                        and "status=up" in s)

        b2.stop()

        status_body = wait_for_status(
            port, lambda s: s.count("status=up") == 1
            and s.count("status=DOWN") == 1)
        check("failover: exactly one peer goes DOWN after being stopped",
              status_body is not None, status_body)

        oks = 0
        for _ in range(5):
            code, body = http_get(port, "/")
            if code == 200 and "ok" in body:
                oks += 1
        check("failover: all subsequent requests served by the survivor",
              oks == 5, f"{oks}/5 succeeded")
    finally:
        if ngx:
            ngx.stop()
        b1.stop()
        b2.stop()


def main():
    if not (os.path.exists(CERT) and os.path.exists(KEY)):
        sys.exit(f"missing {CERT} / {KEY} — generate them first "
                  f"(see `make test`)")

    if not os.path.exists(MODULE_SO):
        sys.exit(f"missing {MODULE_SO} — run `make dynamic` (or `make test`, "
                  f"which builds it) first")

    tests = [
        # 1. with / without certificates
        test_tls_basic_up,
        test_plain_http_no_certificates,
        # 2. with / without the check active
        test_down_detection,
        test_no_healthcheck_baseline,
        # 3. body analysis vs status-only
        test_body_match_rejects_wrong_body,
        test_status_only_no_match_block,
        # 4. TLS certificate verification variants
        test_ssl_verify_valid_chain,
        test_ssl_verify_hostname_match,
        test_ssl_verify_hostname_mismatch,
        # 5. keepalive
        test_keepalive_reuses_connection,
        # 6. resolve
        test_resolve_dns_update,
        # 7. cross-upstream isolation
        test_upstream_isolation_same_backend,
        # 8. shm sizing
        test_shm_size_custom,
        # regression coverage carried over
        test_last_fails_reset,
        test_failover,
    ]

    for t in tests:
        try:
            t()
        except Exception as exc:
            check(t.__name__, False, f"raised {exc!r}")

    failed = [name for name, ok in _results if not ok]
    print()
    print(f"{len(_results) - len(failed)}/{len(_results)} checks passed")
    if failed:
        print("Failed:")
        for name in failed:
            print(f"  - {name}")
        sys.exit(1)


if __name__ == "__main__":
    main()
