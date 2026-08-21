#!/usr/bin/env python3
"""Static file server for the vxs workbench, with caching turned off.

    python3 serve.py          # then open http://localhost:8000/web/index.html

Serve from the REPO ROOT, not from web/, so that /lib and /demos are
reachable — watch mode fetches them over HTTP.

Why this exists rather than `python3 -m http.server`: browsers cache
aggressively, and a stale app.js presents as a preset that silently does
nothing. Safari is the worst of it, and its hard-reload chord is
Cmd-Option-R rather than the Cmd-Shift-R every other browser uses, which
is its own small trap.

The failure mode that actually costs time is a cached vxs.wasm, because
that one is silent: the page loads, the Scheme runs, and a primitive added
in the last build is simply missing. No-store on everything removes the
whole class of problem for a few lines of code.
"""

import http.server
import socketserver
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def log_message(self, fmt, *args):
        # One line per request is noise when watch mode polls every second.
        if "GET /web/" in (fmt % args) or "GET / " in (fmt % args):
            super().log_message(fmt, *args)


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("", PORT), NoCacheHandler) as httpd:
    print(f"vxs workbench:  http://localhost:{PORT}/web/index.html")
    print(f"gpu demo page:  http://localhost:{PORT}/web/gpu.html")
    print("caching disabled — no hard reload needed.  ctrl-C to stop.")
    httpd.serve_forever()
