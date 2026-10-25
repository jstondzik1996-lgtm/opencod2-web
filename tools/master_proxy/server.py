#!/usr/bin/env python3
"""Same-origin HTTP masterlist proxy for CoD2 web Join Game.

Browsers cannot UDP-query cod2master / getinfo, and api.cod.pm has no CORS
(and COEP require-corp blocks cross-origin). This process runs on the VPS and
is reverse-proxied at /play/api/ → http://127.0.0.1:20711/.

The upstream masterlist carries no usable latency: the web client used to
average the connected players' pings, which is their latency to the server, and
999 whenever a server was empty. So this proxy also measures the real
VPS→server round trip with connectionless UDP `getinfo` sweeps and injects it as
`ping_ms`. Because a browser has no UDP, a joining client's traffic has to pass
through this host anyway, which makes VPS→server the meaningful half of the
latency to display.

Project server directory (preferred): set COD2_SERVERS_FILE to a JSON list
of the project's own dedicated servers (see servers.example.json). Then
  GET  /masterlist/cod2/1.3   only our online servers (engine Join Game list,
                              and the relay allowlist)
  GET  /servers               our servers with status, players, free slots
  POST /matchmake             Quick Play: {"mode","region","exclude":[ids]}
  POST /matchmake/release     {"reservation": token}
The public api.cod.pm list is then only served at /external/masterlist/...
and only when COD2_EXTERNAL_MASTER=1; it is labelled external.

Without COD2_SERVERS_FILE the proxy keeps the old behaviour (public list).

Usage:
  python3 tools/master_proxy/server.py
  # or: PORT=20711 COD2_SERVERS_FILE=servers.json python3 server.py
"""

from __future__ import annotations

import json
import os
import re
import select
import socket
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from directory import ConfigError, ServerDirectory, load_config, udp_query_socket
    import sessions
except ImportError:  # imported as tools.master_proxy.server
    from .directory import ConfigError, ServerDirectory, load_config, udp_query_socket
    from . import sessions

UPSTREAM = os.environ.get("COD2_MASTER_UPSTREAM", "https://api.cod.pm").rstrip("/")
PORT = int(os.environ.get("PORT", "20711"))
CACHE_TTL = float(os.environ.get("COD2_MASTER_CACHE_TTL", "30"))

# One sweep = one getinfo to every known server, then collect for RESPONSE_WINDOW.
PING_INTERVAL = float(os.environ.get("COD2_PING_INTERVAL", "2.0"))
PING_RESPONSE_WINDOW = float(os.environ.get("COD2_PING_WINDOW", "1.2"))
# Servers that never answer keep this value rather than a stale measurement.
PING_UNREACHABLE = 999
# Drop a measurement that is older than this so the UI cannot show a dead value.
PING_MAX_AGE = float(os.environ.get("COD2_PING_MAX_AGE", "20"))

_PATH_RE = re.compile(r"^/(masterlist|pings)/(cod2|cod|coduo|cod4)(?:/(1\.\d))?/?$")
_EXTERNAL_RE = re.compile(r"^/external/masterlist/(cod2)(?:/(1\.\d))?/?$")

SERVERS_FILE = os.environ.get("COD2_SERVERS_FILE", "").strip()
EXTERNAL_MASTER = os.environ.get("COD2_EXTERNAL_MASTER", "") not in ("", "0", "no")
MATCHMAKE_PER_MINUTE = int(os.environ.get("COD2_MATCHMAKE_PER_MINUTE", "30"))
MAX_BODY = 4096

# Set in main() (or by tests) when COD2_SERVERS_FILE is configured.
DIRECTORY: ServerDirectory | None = None


class RateLimiter:
    """Fixed one-minute window per client address."""

    def __init__(self, per_minute: int) -> None:
        self.per_minute = per_minute
        self._lock = threading.Lock()
        self._hits: dict[str, tuple[int, int]] = {}

    def allow(self, key: str) -> bool:
        window = int(time.time() // 60)
        with self._lock:
            if len(self._hits) > 10000:
                self._hits.clear()
            win, count = self._hits.get(key, (window, 0))
            if win != window:
                win, count = window, 0
            if count >= self.per_minute:
                self._hits[key] = (win, count)
                return False
            self._hits[key] = (win, count + 1)
            return True


_matchmake_limit = RateLimiter(MATCHMAKE_PER_MINUTE)
_session_limit = RateLimiter(int(os.environ.get("COD2_SESSION_PER_MINUTE", "20")))

# Player sessions (sessions.py). With a secret configured, Quick Play and
# joins require a session and return a relay ticket for the chosen server.
SIGNER = None
BANS = sessions.BanList(os.environ.get("COD2_BANS_FILE") or None)

_cache_lock = threading.Lock()
_cache: dict[str, tuple[float, bytes, str]] = {}


class Pinger:
    """Measures VPS→server RTT for every address seen in a served masterlist."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._targets: dict[tuple[str, int], float] = {}
        self._results: dict[tuple[str, int], tuple[int, float]] = {}
        self._sweeps = 0

    def note_targets(self, addrs: list[tuple[str, int]]) -> None:
        now = time.time()
        with self._lock:
            for addr in addrs:
                self._targets[addr] = now

    def snapshot(self) -> dict[str, int]:
        cutoff = time.time() - PING_MAX_AGE
        with self._lock:
            return {
                "%s:%d" % addr: ms
                for addr, (ms, when) in self._results.items()
                if when >= cutoff
            }

    def stats(self) -> dict[str, int]:
        with self._lock:
            return {
                "targets": len(self._targets),
                "measured": len(self._results),
                "sweeps": self._sweeps,
            }

    def run_forever(self) -> None:
        while True:
            started = time.time()
            try:
                self._sweep()
            except Exception as exc:  # noqa: BLE001 — a sweep must never kill the thread
                print("ping sweep failed: %s" % exc, flush=True)
            sleep_for = PING_INTERVAL - (time.time() - started)
            if sleep_for > 0:
                time.sleep(sleep_for)

    def _sweep(self) -> None:
        with self._lock:
            targets = list(self._targets)
        if not targets:
            time.sleep(PING_INTERVAL)
            return

        sock = udp_query_socket()
        try:
            # CoD2 connectionless packet: 0xFFFFFFFF + command. The challenge is
            # echoed back in infoResponse, but matching on the source address is
            # enough here and keeps one packet per server.
            payload = b"\xff\xff\xff\xffgetinfo cod2web"
            sent_at: dict[tuple[str, int], float] = {}
            for addr in targets:
                try:
                    sock.sendto(payload, addr)
                except OSError:
                    continue
                sent_at[addr] = time.perf_counter()

            answered: set[tuple[str, int]] = set()
            measured: dict[tuple[str, int], int] = {}
            deadline = time.perf_counter() + PING_RESPONSE_WINDOW
            while True:
                remaining = deadline - time.perf_counter()
                if remaining <= 0:
                    break
                if not select.select([sock], [], [], remaining)[0]:
                    break
                try:
                    _, src = sock.recvfrom(2048)
                except ConnectionResetError:
                    continue   # Windows: ICMP unreachable from an offline server
                except OSError:
                    break
                start = sent_at.get(src)
                if start is None or src in answered:
                    continue
                answered.add(src)
                rtt = int(round((time.perf_counter() - start) * 1000.0))
                measured[src] = min(max(rtt, 1), 998)
        finally:
            sock.close()

        now = time.time()
        with self._lock:
            for addr in sent_at:
                if addr in measured:
                    self._results[addr] = (measured[addr], now)
                else:
                    self._results[addr] = (PING_UNREACHABLE, now)
            # Forget servers that dropped out of the masterlist entirely.
            for addr in list(self._results):
                if addr not in self._targets:
                    del self._results[addr]
            self._sweeps += 1


_pinger = Pinger()


def _fetch_upstream(game: str, ver: str) -> tuple[bytes, str]:
    key = f"{game}/{ver}"
    now = time.time()
    with _cache_lock:
        hit = _cache.get(key)
        if hit and now - hit[0] < CACHE_TTL:
            return hit[1], hit[2]

    url = f"{UPSTREAM}/masterlist/{game}/{ver}"
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": "cod2-web-master-proxy/1.0",
            "Accept": "application/json",
        },
        method="GET",
    )
    with urllib.request.urlopen(req, timeout=25) as resp:
        body = resp.read()
        ctype = resp.headers.get("Content-Type", "application/json;charset=utf-8")

    try:
        data = json.loads(body.decode("utf-8", errors="replace"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"upstream non-JSON: {exc}") from exc
    if not isinstance(data, dict) or "servers" not in data:
        raise RuntimeError("upstream missing servers[]")

    body = _inject_pings(data)

    with _cache_lock:
        _cache[key] = (now, body, ctype)
    return body, ctype


def _inject_pings(data: dict) -> bytes:
    """Registers every address for probing and stamps known RTTs onto the list."""
    addrs: list[tuple[str, int]] = []
    servers = data.get("servers") or []
    for srv in servers:
        if not isinstance(srv, dict):
            continue
        ip = srv.get("ip")
        try:
            port = int(srv.get("port"))
        except (TypeError, ValueError):
            continue
        if not ip or not 0 < port < 65536:
            continue
        addrs.append((str(ip), port))

    _pinger.note_targets(addrs)
    known = _pinger.snapshot()

    for srv in servers:
        if not isinstance(srv, dict):
            continue
        ms = known.get("%s:%s" % (srv.get("ip"), srv.get("port")))
        if ms is not None:
            srv["ping_ms"] = ms

    return json.dumps(data, separators=(",", ":")).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args) -> None:
        sys_stderr = __import__("sys").stderr
        sys_stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        if path in ("/", "/health"):
            info = {"ok": 1, "service": "cod2-master-proxy", "ping": _pinger.stats(),
                    "directory": None}
            if DIRECTORY is not None:
                listed = DIRECTORY.list_servers(include_private=True)
                info["directory"] = {
                    "servers": len(listed),
                    "online": sum(1 for x in listed if x["status"] == "online"),
                    "sweeps": DIRECTORY.sweeps,
                }
            info["sessions"] = SIGNER is not None
            body = json.dumps(info).encode("utf-8")
            self._send(200, body, "application/json")
            return

        if DIRECTORY is not None:
            if path in ("/servers", "/servers/"):
                body = json.dumps({"servers": DIRECTORY.list_servers(),
                                   "source": "project"},
                                  separators=(",", ":")).encode("utf-8")
                self._send(200, body, "application/json")
                return
            if _PATH_RE.match(path):
                if _PATH_RE.match(path).group(1) == "pings":
                    pings = {s["connect"]: s["ping_ms"] for s in DIRECTORY.list_servers()
                             if s["status"] == "online"}
                    body = json.dumps({"pings": pings}, separators=(",", ":"))
                else:
                    body = json.dumps(DIRECTORY.masterlist(), separators=(",", ":"))
                self._send(200, body.encode("utf-8"), "application/json")
                return
            m = _EXTERNAL_RE.match(path)
            if m:
                if not EXTERNAL_MASTER:
                    self._send(404, b'{"error":1,"description":"external master disabled"}\n',
                               "application/json")
                    return
                try:
                    body, ctype = _fetch_upstream(m.group(1), m.group(2) or "1.3")
                    data = json.loads(body)
                    data["source"] = "external"
                    data["note"] = "public servers; their own authorization rules apply"
                    self._send(200, json.dumps(data).encode("utf-8"), ctype)
                except Exception as exc:  # noqa: BLE001
                    err = json.dumps({"error": 1, "description": str(exc)}).encode("utf-8")
                    self._send(502, err, "application/json")
                return

        m = _PATH_RE.match(path)
        if not m:
            self._send(404, b'{"error":1,"description":"invalid route"}\n', "application/json")
            return

        kind = m.group(1)
        game = m.group(2)
        ver = m.group(3) or "1.3"

        if kind == "pings":
            # Cheap poll route so the client can refresh latency without
            # rebuilding its whole server list.
            body = json.dumps({"pings": _pinger.snapshot()},
                              separators=(",", ":")).encode("utf-8")
            self._send(200, body, "application/json")
            return

        try:
            body, ctype = _fetch_upstream(game, ver)
            self._send(200, body, ctype)
        except Exception as exc:  # noqa: BLE001 — surface to client JSON
            err = json.dumps({"error": 1, "description": str(exc)}).encode("utf-8")
            self._send(502, err, "application/json")


    def _read_json(self):
        try:
            length = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            length = -1
        if length < 0 or length > MAX_BODY:
            return None
        raw = self.rfile.read(length) if length else b"{}"
        try:
            data = json.loads(raw.decode("utf-8") or "{}")
        except (UnicodeDecodeError, json.JSONDecodeError):
            return None
        return data if isinstance(data, dict) else None

    def do_POST(self) -> None:
        path = self.path.split("?", 1)[0].rstrip("/")
        if DIRECTORY is None:
            self._send(404, b'{"error":1,"description":"no project server directory configured"}\n',
                       "application/json")
            return
        data = self._read_json()
        if data is None:
            self.close_connection = True
            self._send(400, b'{"error":1,"description":"invalid JSON body"}\n', "application/json")
            return
        client = (self.headers.get("X-Forwarded-For") or self.client_address[0]).split(",")[0].strip()
        if path == "/session":
            self._post_session(data, client)
            return
        if path in ("/matchmake", "/join"):
            if not _matchmake_limit.allow(client):
                self._send(429, b'{"error":"rate_limited","retryAfter":60}\n', "application/json")
                return
            session = None
            if SIGNER is not None:
                try:
                    session = SIGNER.verify(data.get("session"), "session")
                except sessions.TokenError as exc:
                    self._json(401, {"error": "session_required", "detail": str(exc)})
                    return
                if BANS.banned(session["pid"]):
                    self._json(403, {"error": "banned"})
                    return
        if path == "/join":
            pick, reason = DIRECTORY.reserve(str(data.get("serverId") or "")[:32])
            if pick is None:
                code = 404 if reason == "unknown" else 409
                self._json(code, {"error": reason})
                return
            if session is not None:
                pick["ticket"] = SIGNER.ticket(session, pick["connect"])
            self._json(200, pick)
            return
        if path == "/matchmake":
            exclude = data.get("exclude") or []
            if not isinstance(exclude, list):
                exclude = []
            pick = DIRECTORY.matchmake(mode=str(data.get("mode") or "")[:16],
                                       region=str(data.get("region") or "")[:16],
                                       exclude=[str(x)[:32] for x in exclude[:16]])
            if pick is None:
                listed = DIRECTORY.list_servers()
                online = sum(1 for x in listed if x["status"] == "online")
                body = {"error": "no_free_server" if online else "no_server_online",
                        "online": online, "retryAfter": 10}
                self._send(503, json.dumps(body).encode("utf-8"), "application/json")
                return
            if session is not None:
                pick["ticket"] = SIGNER.ticket(session, pick["connect"])
            self._send(200, json.dumps(pick).encode("utf-8"), "application/json")
            return
        if path == "/matchmake/release":
            ok = DIRECTORY.release(str(data.get("reservation") or ""))
            self._send(200, json.dumps({"released": ok}).encode("utf-8"), "application/json")
            return
        self._send(404, b'{"error":1,"description":"invalid route"}\n', "application/json")


    def _json(self, code: int, obj: dict) -> None:
        self._send(code, json.dumps(obj).encode("utf-8"), "application/json")

    def _post_session(self, data: dict, client: str) -> None:
        if SIGNER is None:
            self._json(404, {"error": "sessions_disabled"})
            return
        if not _session_limit.allow(client):
            self._json(429, {"error": "rate_limited", "retryAfter": 60})
            return
        nick = sessions.clean_nick(data.get("nick"))
        key = data.get("playerKey")
        if nick is None:
            self._json(400, {"error": "bad_nick", "detail": "2-16 chars: letters, digits, _ - . [ ]"})
            return
        if not sessions.valid_player_key(key):
            self._json(400, {"error": "bad_player_key"})
            return
        if BANS.banned(sessions.player_id(key)):
            self._json(403, {"error": "banned"})
            return
        self._json(200, SIGNER.session(key, nick))


def setup_directory(path: str) -> ServerDirectory:
    entries = load_config(path)
    return ServerDirectory(
        entries,
        poll_interval=float(os.environ.get("COD2_DIRECTORY_POLL", "2.0")),
        stale_after=float(os.environ.get("COD2_DIRECTORY_STALE", "10")),
        reservation_ttl=float(os.environ.get("COD2_RESERVATION_TTL", "30")),
    )


def main() -> None:
    global DIRECTORY, SIGNER
    try:
        SIGNER = sessions.signer_from_env()
    except ValueError as exc:
        raise SystemExit("COD2_SESSION_SECRET: %s" % exc)
    if SIGNER is not None:
        print("player sessions: on (relay tickets required)", flush=True)
    if SERVERS_FILE:
        try:
            DIRECTORY = setup_directory(SERVERS_FILE)
        except (OSError, ValueError, ConfigError) as exc:
            raise SystemExit("cannot load COD2_SERVERS_FILE %s: %s" % (SERVERS_FILE, exc))
        DIRECTORY.start()
        print("project directory: %d servers from %s" % (len(DIRECTORY.list_servers(True)), SERVERS_FILE),
              flush=True)
    threading.Thread(target=_pinger.run_forever, name="pinger", daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"cod2 master proxy on http://127.0.0.1:{PORT}/  "
          f"list={'project directory' if DIRECTORY else 'public ' + UPSTREAM}"
          f"  ping every {PING_INTERVAL}s", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
