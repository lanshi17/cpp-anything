"""A tiny HTTP server: the server-mode fixture for `cpp-anything app record --http`."""
import http.server
import json
import os
import sys
from urllib.parse import parse_qs, urlparse


def slug(text: str) -> str:
    out = []
    dash = False
    for ch in text.strip().lower():
        if ch.isascii() and ch.isalnum():
            out.append(ch)
            dash = False
        elif not dash and out:
            out.append("-")
            dash = True
    return "".join(out).strip("-")


class Handler(http.server.BaseHTTPRequestHandler):
    def _send(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/health":
            self._send(200, {"ok": True})
            return
        if parsed.path == "/slug":
            query = parse_qs(parsed.query)
            text = query.get("text", [""])[0]
            self._send(200, {"slug": slug(text)})
            return
        self._send(404, {"error": "not found"})

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        if self.path == "/echo":
            self._send(200, {"echo": body, "length": len(body)})
            return
        if self.path == "/slug":
            try:
                text = json.loads(body).get("text", "")
            except json.JSONDecodeError:
                self._send(400, {"error": "invalid json"})
                return
            self._send(200, {"slug": slug(text)})
            return
        self._send(404, {"error": "not found"})

    def log_message(self, *args) -> None:  # keep the recorded stdout clean
        pass


def main() -> int:
    port = int(os.environ.get("PORT", "8080"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"listening on {port}", flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
