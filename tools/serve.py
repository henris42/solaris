#!/usr/bin/env python3
"""Serves a folder to a browser for the WebAssembly game: serve.py <folder> <port>

With the two headers a page with threads needs (COOP, COEP) and with Range requests,
which GameApi uses to fetch a file in blocks.
"""
import http.server, os, sys

root = os.path.abspath(sys.argv[1])
kinds = {".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm"}


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def answer(self, status, body, extra=()):
        self.send_response(status)
        for k, v in (("Cross-Origin-Opener-Policy", "same-origin"), ("Cross-Origin-Embedder-Policy", "require-corp"),
                     ("Cache-Control", "no-store"), ("Accept-Ranges", "bytes"), ("Content-Length", str(len(body))), *extra):
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        full = os.path.normpath(os.path.join(root, path.lstrip("/")))
        if os.path.isdir(full):
            full = os.path.join(full, "index.html")
        if not full.startswith(root) or not os.path.isfile(full):
            return self.answer(404, b"not found")
        data = open(full, "rb").read()
        kind = ("Content-Type", kinds.get(os.path.splitext(full)[1], "application/octet-stream"))
        rng = self.headers.get("Range", "")
        if rng.startswith("bytes="):
            a, _, b = rng[6:].partition("-")
            a = int(a or 0)
            b = min(int(b) if b else len(data) - 1, len(data) - 1)
            return self.answer(206, data[a:b + 1], (kind, ("Content-Range", f"bytes {a}-{b}/{len(data)}")))
        self.answer(200, data, (kind,))


http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), H).serve_forever()
