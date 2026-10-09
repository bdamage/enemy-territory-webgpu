#!/usr/bin/env python3
"""Development server: serves dist/ at / and data/ at /data/."""
import functools, http.server, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, 'dist')
DATA = os.path.join(ROOT, 'data')


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        '.wasm': 'application/wasm',
        '.js': 'text/javascript',
        '.pk3': 'application/octet-stream',
    }

    def translate_path(self, path):
        path = path.split('?', 1)[0].split('#', 1)[0]
        if path.startswith('/data/'):
            rel = os.path.normpath(path[len('/data/'):]).lstrip('/')
            if rel.startswith('..'):
                return os.path.join(DIST, '404')
            return os.path.join(DATA, rel)
        return super().translate_path(path)

    def end_headers(self):
        if not self.path.startswith('/data/'):
            self.send_header('Cache-Control', 'no-cache')
        super().end_headers()

    def log_message(self, fmt, *args):
        if '/data/' not in self.path:
            super().log_message(fmt, *args)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    handler = functools.partial(Handler, directory=DIST)
    with http.server.ThreadingHTTPServer(('127.0.0.1', port), handler) as httpd:
        print(f'Serving http://localhost:{port}/  (dist/ + data/)')
        httpd.serve_forever()


if __name__ == '__main__':
    main()
