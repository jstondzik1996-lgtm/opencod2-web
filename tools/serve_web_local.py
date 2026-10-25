#!/usr/bin/env python3
"""Local web client + Join Game proxies (no VPS).

Serves build-web-wsl/ (preferred) or build/web/ and reverse-proxies the
same-origin paths the browser already uses:

  /play/api/masterlist/...  →  tools/master_proxy/server.py  :20711
  /play/api/pings/...       →  same
  /play/api/gamews          →  tools/game_proxy/relay.py     :20712
  /play/api/servers, POST /play/api/matchmake  →  master proxy (project
                            server directory, when --servers is given)

Run from the repo root (Windows or WSL):

  pip install websockets          # once, for the WS↔UDP relay
  python tools/serve_web_local.py
  # open http://127.0.0.1:8080/cod2.html

Optional:
  python tools/serve_web_local.py --port 8080 --coop
  python tools/serve_web_local.py --root path/to/web --no-spawn
  python tools/serve_web_local.py --servers tools/master_proxy/servers.local.json
  python tools/serve_web_local.py --servers tools/master_proxy/servers.local.json --sessions

COOP/COEP: off by default (MEMFS). Pass --coop for pthread / WASMFS / WORKERFS
(SharedArrayBuffer). Cross-origin isolation is not required for the default build.
"""

from __future__ import annotations

import argparse
import http.client
import json
import mimetypes
import os
import select
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MASTER_PY = os.path.join(REPO, "tools", "master_proxy", "server.py")
RELAY_PY = os.path.join(REPO, "tools", "game_proxy", "relay.py")
SRC_REMOTE_CONFIG = os.path.join(REPO, "src", "web", "remote-config.js")

MASTER_HOST = "127.0.0.1"
RELAY_HOST = "127.0.0.1"
DEFAULT_HTTP_PORT = 8080
DEFAULT_MASTER_PORT = 20711
DEFAULT_RELAY_PORT = 20712

MIME_EXTRA = {
    ".wasm": "application/wasm",
    ".js": "application/javascript; charset=utf-8",
    ".mjs": "application/javascript; charset=utf-8",
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".map": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".ico": "image/x-icon",
    ".webp": "image/webp",
    ".woff": "font/woff",
    ".woff2": "font/woff2",
    ".iwd": "application/octet-stream",
    ".pk3": "application/octet-stream",
    ".data": "application/octet-stream",
}

# Shared with the Handler via serve() assignment.
CFG: dict = {}

# Private asset access (stage 2 test setups). Original game archives must not
# be served to anyone who merely knows the URL: with --access-key, the asset
# manifest, game archives and anything under main/ or zone/ require a cookie
# that a tester gets by opening the page once with ?access=<key>.
ACCESS_COOKIE = "cod2_access"
PROTECTED_EXT = (".iwd", ".pk3", ".bik", ".roq", ".str", ".ff")


def _access_token(key: str) -> str:
    import hashlib
    import hmac
    return hmac.new(key.encode("utf-8"), b"cod2web-asset-access", hashlib.sha256).hexdigest()


def is_protected_asset(path: str) -> bool:
    p = urllib.parse.unquote(path).replace("\\", "/").lstrip("/").lower()
    return (p == "assets.json" or p.endswith("/assets.json") or p.startswith("main/")
            or p.startswith("zone/") or p.endswith(PROTECTED_EXT))
_CHILDREN: list[subprocess.Popen] = []


def _port_open(host: str, port: int, timeout: float = 0.35) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def _http_ok(host: str, port: int, path: str) -> bool:
    try:
        conn = http.client.HTTPConnection(host, port, timeout=2.5)
        conn.request("GET", path, headers={"Accept": "application/json"})
        resp = conn.getresponse()
        ok = 200 <= resp.status < 300
        resp.read()
        conn.close()
        return ok
    except OSError:
        return False


def find_webroot(explicit: str | None) -> str:
    if explicit:
        root = os.path.abspath(explicit)
        html = os.path.join(root, "cod2.html")
        if not os.path.isfile(html):
            raise SystemExit("no cod2.html under --root %s" % root)
        return root
    for rel in ("build-web-wsl", os.path.join("build", "web")):
        root = os.path.join(REPO, rel)
        if os.path.isfile(os.path.join(root, "cod2.html")):
            return os.path.realpath(root)
    raise SystemExit(
        "no web build found. Expected %s or %s (or pass --root)."
        % (os.path.join(REPO, "build-web-wsl"), os.path.join(REPO, "build", "web"))
    )


def _wait_port(host: str, port: int, seconds: float, label: str) -> bool:
    deadline = time.time() + seconds
    while time.time() < deadline:
        if _port_open(host, port):
            return True
        time.sleep(0.15)
    print("warning: %s did not open %s:%d" % (label, host, port), flush=True)
    return False


def spawn_backends(master_port: int, relay_port: int, servers_file: str | None = None,
                   sessions: bool = False) -> None:
    if sessions and not os.environ.get("COD2_SESSION_SECRET"):
        # Master proxy (issues tickets) and relay (checks them) share it.
        import secrets
        os.environ["COD2_SESSION_SECRET"] = secrets.token_urlsafe(32)
        print("player sessions: on (random secret for this run)", flush=True)
    if not _port_open(MASTER_HOST, master_port):
        env = os.environ.copy()
        env["PORT"] = str(master_port)
        if servers_file:
            env["COD2_SERVERS_FILE"] = os.path.abspath(servers_file)
        print("starting master proxy  %s" % MASTER_PY, flush=True)
        _CHILDREN.append(subprocess.Popen([sys.executable, MASTER_PY], env=env, cwd=REPO))
        _wait_port(MASTER_HOST, master_port, 6.0, "master proxy")
    else:
        print("reusing master proxy on %s:%d" % (MASTER_HOST, master_port), flush=True)

    if not _port_open(RELAY_HOST, relay_port):
        try:
            import websockets  # noqa: F401
        except ImportError:
            print("warning: pip install websockets  (needed for tools/game_proxy/relay.py)",
                  flush=True)
            return
        env = os.environ.copy()
        env["RELAY_PORT"] = str(relay_port)
        env["RELAY_HOST"] = RELAY_HOST
        # Allowlist from THIS master proxy; otherwise a test relay on 20722
        # would read the preview master on 20711.
        env.setdefault("COD2_RELAY_MASTERLIST",
                       "http://%s:%d/masterlist/cod2/1.3" % (MASTER_HOST, master_port))
        print("starting game relay    %s" % RELAY_PY, flush=True)
        _CHILDREN.append(subprocess.Popen([sys.executable, RELAY_PY], env=env, cwd=REPO))
        _wait_port(RELAY_HOST, relay_port, 6.0, "game relay")
    else:
        print("reusing game relay on %s:%d" % (RELAY_HOST, relay_port), flush=True)


def stop_children() -> None:
    for proc in _CHILDREN:
        if proc.poll() is None:
            proc.terminate()
    deadline = time.time() + 3.0
    for proc in _CHILDREN:
        while proc.poll() is None and time.time() < deadline:
            time.sleep(0.05)
        if proc.poll() is None:
            proc.kill()
    _CHILDREN.clear()


def _coop_headers() -> list[tuple[str, str]]:
    if not CFG.get("coop"):
        return []
    return [
        ("Cross-Origin-Opener-Policy", "same-origin"),
        ("Cross-Origin-Embedder-Policy", "require-corp"),
        ("Cross-Origin-Resource-Policy", "same-origin"),
    ]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    # HEAD reuses the GET routes but must not write a body.
    _head_only = False

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send_bytes(self, code: int, body: bytes, ctype: str, extra: list[tuple[str, str]] | None = None) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        for k, v in _coop_headers():
            self.send_header(k, v)
        if extra:
            for k, v in extra:
                self.send_header(k, v)
        self.end_headers()
        if not self._head_only:
            self.wfile.write(body)

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, HEAD, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", "0")
        for k, v in _coop_headers():
            self.send_header(k, v)
        self.end_headers()

    def do_HEAD(self) -> None:
        self._head_only = True
        try:
            self.do_GET()
        finally:
            # BaseHTTPRequestHandler reuses this instance for HTTP/1.1
            # keep-alive requests. A later GET must send its body.
            self._head_only = False

    def _has_access(self) -> bool:
        key = CFG.get("access_key")
        if not key:
            return True
        import hmac
        want = _access_token(key)
        for part in (self.headers.get("Cookie") or "").split(";"):
            name, _, value = part.strip().partition("=")
            if name == ACCESS_COOKIE and hmac.compare_digest(value, want):
                return True
        return False

    def _grant_access(self) -> bool:
        """?access=<key> on any page sets the cookie and redirects without it."""
        key = CFG.get("access_key")
        if not key:
            return False
        parts = urllib.parse.urlsplit(self.path)
        params = urllib.parse.parse_qsl(parts.query, keep_blank_values=True)
        given = [v for k, v in params if k == "access"]
        if not given:
            return False
        import hmac
        if not hmac.compare_digest(given[0].encode("utf-8"), key.encode("utf-8")):
            self._send_bytes(403, b'{"error":"bad_access_key"}\n', "application/json")
            return True
        rest = urllib.parse.urlencode([(k, v) for k, v in params if k != "access"])
        location = parts.path + ("?" + rest if rest else "")
        self.send_response(302)
        self.send_header("Location", location or "/")
        self.send_header("Set-Cookie", "%s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000"
                         % (ACCESS_COOKIE, _access_token(key)))
        self.send_header("Content-Length", "0")
        self.end_headers()
        return True

    def do_GET(self) -> None:
        path = urllib.parse.urlsplit(self.path).path
        if self._grant_access():
            return
        if is_protected_asset(path) and not self._has_access():
            self.close_connection = True
            self._send_bytes(403, b'{"error":"private_assets","hint":"open the page with ?access=<key>"}\n',
                             "application/json")
            return
        if path == "/play/api/dev-status":
            self._dev_status()
            return
        if path == "/play/api/gamews" or path.startswith("/play/api/gamews/"):
            if (self.headers.get("Upgrade") or "").lower() == "websocket":
                self._ws_proxy()
            else:
                up = _port_open(RELAY_HOST, int(CFG["relay_port"]))
                body = json.dumps({
                    "ok": 1 if up else 0,
                    "service": "cod2-game-relay",
                    "hint": "WebSocket upgrade at /play/api/gamews?host=<ipv4>&port=<28960-29970>",
                }).encode("utf-8")
                self._send_bytes(200 if up else 503, body, "application/json")
            return
        if path == "/play/api" or path.startswith("/play/api/"):
            suffix = path[len("/play/api"):] or "/"
            if suffix == "/":
                suffix = "/health"
            qs = urllib.parse.urlsplit(self.path).query
            if qs:
                suffix = suffix + "?" + qs
            self._proxy_master(suffix)
            return
        self._serve_static()

    def do_POST(self) -> None:
        path = urllib.parse.urlsplit(self.path).path
        if not path.startswith("/play/api/") or path.startswith("/play/api/gamews"):
            self.close_connection = True
            self._send_bytes(404, b"not found\n", "text/plain; charset=utf-8")
            return
        try:
            length = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            length = -1
        if length < 0 or length > 4096:
            self.close_connection = True
            self._send_bytes(413, b'{"error":1,"description":"body too large"}\n', "application/json")
            return
        body = self.rfile.read(length) if length else b""
        self._proxy_master(path[len("/play/api"):], method="POST", body=body)

    def _dev_status(self) -> None:
        master_port = int(CFG["master_port"])
        relay_port = int(CFG["relay_port"])
        master_ok = _http_ok(MASTER_HOST, master_port, "/health")
        relay_ok = _port_open(RELAY_HOST, relay_port)
        payload = {
            "ok": 1 if (master_ok and relay_ok) else 0,
            "service": "cod2-local-dev",
            "webroot": CFG.get("webroot"),
            "coop": bool(CFG.get("coop")),
            "master": {
                "ok": 1 if master_ok else 0,
                "url": "http://%s:%d/health" % (MASTER_HOST, master_port),
            },
            "relay": {
                "ok": 1 if relay_ok else 0,
                "url": "ws://%s:%d/" % (RELAY_HOST, relay_port),
            },
        }
        body = json.dumps(payload).encode("utf-8")
        self._send_bytes(200, body, "application/json")

    def _proxy_master(self, suffix: str, method: str = "GET", body: bytes | None = None) -> None:
        port = int(CFG["master_port"])
        try:
            conn = http.client.HTTPConnection(MASTER_HOST, port, timeout=30)
            headers = {"Accept": self.headers.get("Accept") or "application/json",
                       "X-Forwarded-For": self.client_address[0]}
            if method == "POST":
                headers["Content-Type"] = "application/json"
            conn.request(method, suffix, body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            ctype = resp.getheader("Content-Type") or "application/json"
            extra = [("Access-Control-Allow-Origin", "*")]
            cache = resp.getheader("Cache-Control")
            if cache:
                extra.append(("Cache-Control", cache))
            self._send_bytes(resp.status, data, ctype, extra)
            conn.close()
        except OSError as exc:
            err = json.dumps({
                "error": 1,
                "description": "master proxy unreachable (%s). Is tools/master_proxy/server.py running?" % exc,
            }).encode("utf-8")
            self._send_bytes(502, err, "application/json")

    def _ws_proxy(self) -> None:
        """TCP-splice the browser WS upgrade to relay.py (no extra WS library)."""
        qs = urllib.parse.urlsplit(self.path).query
        dest_path = "/" + (("?" + qs) if qs else "")
        port = int(CFG["relay_port"])
        try:
            upstream = socket.create_connection((RELAY_HOST, port), timeout=5)
        except OSError as exc:
            body = json.dumps({
                "error": 1,
                "description": "game relay unreachable (%s). pip install websockets && python tools/game_proxy/relay.py" % exc,
            }).encode("utf-8")
            self._send_bytes(502, body, "application/json")
            return

        hop_drop = {"host", "content-length", "x-forwarded-for"}
        req = ["GET %s HTTP/1.1" % dest_path, "Host: %s:%d" % (RELAY_HOST, port),
               # The relay limits sessions per browser address.
               "X-Forwarded-For: %s" % self.client_address[0]]
        for key, val in self.headers.items():
            if key.lower() in hop_drop:
                continue
            req.append("%s: %s" % (key, val))
        req.append("")
        req.append("")
        try:
            upstream.sendall("\r\n".join(req).encode("iso-8859-1"))
            buf = b""
            while b"\r\n\r\n" not in buf:
                chunk = upstream.recv(4096)
                if not chunk:
                    break
                buf += chunk
            if b"\r\n\r\n" not in buf:
                upstream.close()
                self._send_bytes(502, b'{"error":1,"description":"relay handshake failed"}\n',
                                 "application/json")
                return
            self.close_connection = True
            self.connection.sendall(buf)
            _splice_sockets(self.connection, upstream)
        except OSError:
            pass
        finally:
            try:
                upstream.close()
            except OSError:
                pass

    def _resolve_static(self) -> str | None:
        path = urllib.parse.urlsplit(self.path).path
        rel = urllib.parse.unquote(path).lstrip("/")
        if not rel:
            return ""
        rel_norm = rel.replace("\\", "/")
        if rel_norm == "remote-config.js" and os.path.isfile(SRC_REMOTE_CONFIG):
            return SRC_REMOTE_CONFIG
        # Launcher-only files are served from src/web so lobby/branding edits
        # do not need a WASM relink (the shell HTML itself still does).
        if rel_norm in ("lobby.js", "brand-mark.png"):
            src = os.path.join(REPO, "src", "web", rel_norm)
            if os.path.isfile(src):
                return src
        root = os.path.realpath(CFG["webroot"])
        joined = os.path.normpath(os.path.join(root, *rel_norm.split("/")))
        # Reject ../ traversal lexically, before symlinks are resolved. Content
        # symlinked/junctioned *inside* the webroot stays reachable (the usual
        # way to serve a CoD2 install without copying the iwds).
        if joined != root and not joined.startswith(root + os.sep):
            return None
        full = os.path.realpath(joined)
        if os.path.isdir(full):
            return None
        if not os.path.isfile(full):
            return None
        return full

    def _send_file(self, code: int, path: str, ctype: str, extra: list[tuple[str, str]] | None = None) -> None:
        """Send a file without loading it into memory (the iwds are ~700 MB)."""
        size = os.path.getsize(path)
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(size))
        self.send_header("Cache-Control", "no-cache")
        for k, v in _coop_headers():
            self.send_header(k, v)
        if extra:
            for k, v in extra:
                self.send_header(k, v)
        self.end_headers()
        if self._head_only:
            return
        try:
            with open(path, "rb") as fh:
                while True:
                    chunk = fh.read(1 << 20)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
        except OSError:
            self.close_connection = True

    def _serve_static(self) -> None:
        resolved = self._resolve_static()
        if resolved == "":
            self.send_response(302)
            self.send_header("Location", "/cod2.html")
            self.send_header("Content-Length", "0")
            for k, v in _coop_headers():
                self.send_header(k, v)
            self.end_headers()
            return
        if not resolved:
            self._send_bytes(404, b"not found\n", "text/plain; charset=utf-8")
            return
        ext = os.path.splitext(resolved)[1].lower()
        ctype = MIME_EXTRA.get(ext) or mimetypes.guess_type(resolved)[0] or "application/octet-stream"
        extra = []
        if ext in (".html", ".js", ".css"):
            extra.append(("Cache-Control", "no-cache"))
        self._send_file(200, resolved, ctype, extra)


def _splice_sockets(a: socket.socket, b: socket.socket) -> None:
    pair = [a, b]
    try:
        while True:
            readable, _, failed = select.select(pair, [], pair, 120.0)
            if failed:
                break
            if not readable:
                continue
            for sock in readable:
                other = b if sock is a else a
                data = sock.recv(65536)
                if not data:
                    return
                other.sendall(data)
    except OSError:
        return


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve CoD2 web build + local Join Game proxies")
    parser.add_argument("--port", type=int,
                        default=int(os.environ.get("COD2_LOCAL_PORT", str(DEFAULT_HTTP_PORT))))
    parser.add_argument("--bind", default=os.environ.get("COD2_LOCAL_BIND", "127.0.0.1"))
    parser.add_argument("--root", default=None, help="web artifact dir (default: build-web-wsl or build/web)")
    parser.add_argument("--master-port", type=int, default=DEFAULT_MASTER_PORT)
    parser.add_argument("--relay-port", type=int, default=DEFAULT_RELAY_PORT)
    parser.add_argument("--servers", default=os.environ.get("COD2_SERVERS_FILE"),
                        help="project server directory JSON for the spawned master proxy")
    parser.add_argument("--access-key", default=os.environ.get("COD2_ACCESS_KEY"),
                        help="private test setup: game assets need ?access=<key> once (cookie)")
    parser.add_argument("--sessions", action="store_true",
                        help="require player sessions/join tickets (generates COD2_SESSION_SECRET if unset)")
    parser.add_argument("--no-spawn", action="store_true",
                        help="do not start server.py / relay.py (they must already be listening)")
    parser.add_argument("--coop", action="store_true",
                        help="send COOP/COEP headers (pthread / WASMFS / WORKERFS only)")
    args = parser.parse_args()

    webroot = find_webroot(args.root)
    CFG.update({
        "webroot": webroot,
        "master_port": args.master_port,
        "relay_port": args.relay_port,
        "coop": bool(args.coop or os.environ.get("COD2_LOCAL_COOP")),
        "access_key": args.access_key or None,
    })
    if args.access_key and len(args.access_key) < 12:
        raise SystemExit("--access-key must be at least 12 characters")
    if args.bind not in ("127.0.0.1", "localhost", "::1") and not args.access_key:
        print("WARNING: serving on %s without --access-key; anyone who can reach it can download "
              "the game archives under build/web/main. Use --access-key for shared test hosts."
              % args.bind, flush=True)

    if not args.no_spawn:
        spawn_backends(args.master_port, args.relay_port, args.servers, args.sessions)

    httpd = ThreadingHTTPServer((args.bind, args.port), Handler)
    url = "http://%s:%d/cod2.html" % (args.bind, args.port)
    print("", flush=True)
    print("cod2 local dev", flush=True)
    print("  static   %s" % webroot, flush=True)
    print("  page     %s" % url, flush=True)
    print("  master   /play/api/masterlist/cod2/1.3 -> %s:%d" % (MASTER_HOST, args.master_port), flush=True)
    print("  relay    /play/api/gamews -> %s:%d" % (RELAY_HOST, args.relay_port), flush=True)
    print("  status   http://%s:%d/play/api/dev-status" % (args.bind, args.port), flush=True)
    print("  servers  %s" % (args.servers or "public masterlist (no --servers)"), flush=True)
    print("  COOP/COEP %s" % ("on (--coop)" if CFG["coop"] else "off (default MEMFS)"), flush=True)
    print("Ctrl+C to stop.", flush=True)

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping...", flush=True)
    finally:
        httpd.server_close()
        stop_children()


if __name__ == "__main__":
    # Handler threads hold WS tunnels; do not let process hang on leftover threads.
    threading.current_thread().name = "main"
    main()
