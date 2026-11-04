#!/usr/bin/env python3
"""Serve a build directory on localhost with the headers WebAssembly threads need.

SharedArrayBuffer (Emscripten pthreads, ADR-0002) is only available on
cross-origin isolated pages, so every response carries
Cross-Origin-Opener-Policy: same-origin and
Cross-Origin-Embedder-Policy: require-corp. WebGPU additionally needs a
secure context, which http://localhost is.

Only for local development: binds to 127.0.0.1, never serves game data
from a public host (project rule: builds are never hosted publicly).

    python3 tools/web/serve.py build/wasm/gpu/webgpu [--port 8000]
    -> http://localhost:8000/lgp_webgpu_demo.html
"""
import argparse
import functools
import http.server
import sys


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args(argv)
    handler = functools.partial(IsolatedHandler, directory=args.directory)
    with http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print(f"serving {args.directory} on http://localhost:{args.port}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
