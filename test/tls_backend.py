#!/usr/bin/env python3
"""Test HTTPS backend for the module: replies {"status":"ok"} on any path.

Usage: tls_backend.py PORT
"""
import http.server
import ssl
import sys


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} PORT", file=sys.stderr)
        sys.exit(1)

    port = int(sys.argv[1])

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = b'{"status":"ok"}\n'
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    httpd = http.server.HTTPServer(("127.0.0.1", port), H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain("cert.pem", "key.pem")
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
