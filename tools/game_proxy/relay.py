#!/usr/bin/env python3
"""WebSocket ↔ UDP relay for CoD2 browser clients.

Each WebSocket connection is bound to one remote CoD2 server endpoint,
specified as query parameters ?host=<ip>&port=<port>.  Binary WS frames
are forwarded as UDP datagrams; UDP replies are forwarded back as WS
binary frames.

The relay runs as a standalone asyncio server (default :20712) behind
nginx, which upgrades /play/api/gamews to this backend.

Security:
  - Targets are checked against the master list the master proxy serves, so
    the relay only forwards to servers a player could already see there.
    Set COD2_RELAY_ALLOW_ANY=1 to skip that and fall back to the port range.
  - COD2_PORT_MIN/COD2_PORT_MAX bound the fallback range.
  - Each WS session is pinned to exactly one (host, port) pair.
  - Maximum datagram size matches CoD2 MAX_MSGLEN (16384).

Usage:
  python3 tools/game_proxy/relay.py
  # or: RELAY_PORT=20712 python3 relay.py
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

try:
    import websockets
    import websockets.server
except ImportError:
    print("ERROR: pip install websockets  (>= 12.0)", file=sys.stderr)
    sys.exit(1)

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "master_proxy"))
import sessions  # noqa: E402  (tools/master_proxy/sessions.py)

RELAY_PORT = int(os.environ.get("RELAY_PORT", "20712"))
RELAY_HOST = os.environ.get("RELAY_HOST", "127.0.0.1")

PORT_MIN = int(os.environ.get("COD2_PORT_MIN", "1024"))
PORT_MAX = int(os.environ.get("COD2_PORT_MAX", "65535"))

# CoD2 servers sit on 28960-28970 mostly, but the live master list also carries
# 28885, 23000, 4009, ... so a narrow port range would silently refuse a fifth
# of the servers a player can see. The real control is the master list below.
MASTERLIST_URL = os.environ.get(
    "COD2_RELAY_MASTERLIST", "http://127.0.0.1:20711/masterlist/cod2/1.3")
ALLOW_ANY = os.environ.get("COD2_RELAY_ALLOW_ANY", "") not in ("", "0", "no")
ALLOWLIST_REFRESH = float(os.environ.get("COD2_RELAY_ALLOWLIST_REFRESH", "120"))
# A target that is not in the cached list triggers one fresh fetch, at most
# this often, so a server that came online after the last refresh is usable.
ALLOWLIST_MISS_REFRESH = float(os.environ.get("COD2_RELAY_ALLOWLIST_MISS_REFRESH", "5"))

MAX_DGRAM = 16384
UDP_RECV_TIMEOUT = 0.05

# Server->browser datagrams wait in one bounded queue per session; when a slow
# browser lets it fill up, the oldest datagrams are dropped (UDP semantics)
# instead of growing memory without limit.
SEND_QUEUE = int(os.environ.get("COD2_RELAY_SEND_QUEUE", "256"))
# Browser->server packets per second per session (CoD2 cl_maxpackets <= 100;
# connection setup bursts are covered by the bucket size).
UP_PPS = float(os.environ.get("COD2_RELAY_UP_PPS", "250"))
# Concurrent sessions per client address (0 = unlimited).
MAX_PER_IP = int(os.environ.get("COD2_RELAY_MAX_PER_IP", "4"))

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("cod2-relay")


def _ws_path_and_headers(ws) -> tuple[str, object]:
    """websockets 10-12: ws.path / ws.request_headers; 13+: ws.request.path."""
    req = getattr(ws, "request", None)
    if req is not None:
        return req.path, req.headers
    path = getattr(ws, "path", None)
    headers = getattr(ws, "request_headers", None)
    if not path:
        raise ValueError("missing request path")
    return path, headers


class AllowList:
    """The (ip, port) pairs the master proxy is currently advertising.

    Without this the relay is an open UDP proxy - anyone could point it at any
    address. Refreshing the set from the master proxy keeps forwarding to
    servers a player could already see in the browser, which is a tighter
    control than the narrow port range that used to refuse legitimate servers.

    An empty set rejects everything: a relay that silently forwards anywhere
    because the master proxy is down is worse than one that refuses.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._targets: set[tuple[str, int]] = set()
        self._last_refresh = 0.0

    def _refresh(self) -> None:
        self._last_refresh = time.time()
        try:
            with urllib.request.urlopen(MASTERLIST_URL, timeout=4) as res:
                data = json.load(res)
        except (OSError, ValueError, urllib.error.URLError) as exc:
            log.warning("allowlist refresh failed (%s); keeping %d entries",
                        exc, len(self._targets))
            return
        targets: set[tuple[str, int]] = set()
        for srv in data.get("servers") or []:
            ip = srv.get("ip")
            try:
                port = int(srv.get("port"))
            except (TypeError, ValueError):
                continue
            if ip and 0 < port < 65536:
                targets.add((str(ip), port))
        if targets or data.get("source") == "project":
            # The project directory may legitimately list zero online servers;
            # an empty public list means the upstream fetch went wrong.
            with self._lock:
                self._targets = targets
            log.info("allowlist: %d servers", len(targets))

    def start(self) -> None:
        """Refresh once up front, then keep it fresh on a background thread."""
        self._refresh()

        def loop() -> None:
            while True:
                time.sleep(ALLOWLIST_REFRESH)
                self._refresh()

        threading.Thread(target=loop, name="allowlist", daemon=True).start()

    def allows(self, host: str, port: int) -> bool:
        with self._lock:
            if (host, port) in self._targets:
                return True
        if time.time() - self._last_refresh >= ALLOWLIST_MISS_REFRESH:
            self._refresh()
            with self._lock:
                return (host, port) in self._targets
        return False


_allowlist = AllowList()

# With COD2_SESSION_SECRET the relay admits a WebSocket only with a valid join
# ticket for exactly this server (issued by the master proxy) whose player id
# is not banned. This is the project's own admission control; project servers
# run in LAN mode behind the relay and never see a CD key.
try:
    SIGNER = sessions.signer_from_env()
except ValueError as exc:
    print("ERROR: COD2_SESSION_SECRET: %s" % exc, file=sys.stderr)
    sys.exit(1)
BANS = sessions.BanList(os.environ.get("COD2_BANS_FILE") or None)


def check_ticket(params: dict, host: str, port: int):
    """Returns the admitted player payload, or None when sessions are off.

    A join ticket (from Quick Play / Join, bound to this server) is preferred.
    A player session token is also accepted so that in-game reconnects and the
    in-game server list keep working after the short ticket expired; the
    target is still limited to directory servers by the allowlist and the
    dedicated server still enforces its own slot limit."""
    if SIGNER is None:
        return None
    tickets = params.get("ticket")
    sessions_ = params.get("session")
    try:
        if tickets:
            payload = SIGNER.verify(tickets[0], "ticket")
            if payload.get("srv") != "%s:%d" % (host, port):
                raise ValueError("ticket is for another server")
        elif sessions_:
            payload = SIGNER.verify(sessions_[0], "session")
        else:
            raise ValueError("join ticket or player session required")
    except sessions.TokenError as exc:
        raise ValueError("invalid ticket/session: %s" % exc) from None
    if BANS.banned(payload["pid"]):
        raise ValueError("player is banned")
    return payload


class DropOldestQueue:
    """Bounded FIFO for asyncio: put() never blocks; when full the oldest
    item is discarded. Counts drops for logging."""

    def __init__(self, maxsize: int) -> None:
        self._q: asyncio.Queue = asyncio.Queue(maxsize=max(1, maxsize))
        self.dropped = 0

    def put(self, item) -> None:
        if self._q.full():
            try:
                self._q.get_nowait()
                self.dropped += 1
            except asyncio.QueueEmpty:
                pass
        self._q.put_nowait(item)

    async def get(self):
        return await self._q.get()

    def qsize(self) -> int:
        return self._q.qsize()


class TokenBucket:
    def __init__(self, rate: float, burst: float, clock=time.monotonic) -> None:
        self.rate = rate
        self.burst = burst
        self.tokens = burst
        self._clock = clock
        self._last = clock()

    def allow(self) -> bool:
        if self.rate <= 0:
            return True
        now = self._clock()
        self.tokens = min(self.burst, self.tokens + (now - self._last) * self.rate)
        self._last = now
        if self.tokens >= 1.0:
            self.tokens -= 1.0
            return True
        return False


class ConnLimiter:
    def __init__(self, per_ip: int) -> None:
        self.per_ip = per_ip
        self._lock = threading.Lock()
        self._active: dict[str, int] = {}

    def acquire(self, ip: str) -> bool:
        with self._lock:
            n = self._active.get(ip, 0)
            if self.per_ip > 0 and n >= self.per_ip:
                return False
            self._active[ip] = n + 1
            return True

    def release(self, ip: str) -> None:
        with self._lock:
            n = self._active.get(ip, 0) - 1
            if n <= 0:
                self._active.pop(ip, None)
            else:
                self._active[ip] = n


_conns = ConnLimiter(MAX_PER_IP)


def client_ip(ws, headers) -> str:
    """The browser's address. X-Forwarded-For is trusted only from a loopback
    peer (nginx / serve_web_local in front of the relay)."""
    peer = ""
    try:
        peer = ws.remote_address[0]
    except Exception:  # noqa: BLE001
        pass
    if peer in ("127.0.0.1", "::1", "") and headers is not None:
        try:
            fwd = headers.get("X-Forwarded-For") or ""
        except Exception:  # noqa: BLE001
            fwd = ""
        if fwd:
            return fwd.split(",")[0].strip()
    return peer or "?"


def parse_target(path: str, headers) -> tuple[str, int]:
    """Extract host= and port= from the WS request URI query string.

    Also enforces the allowlist and, with sessions on, the join ticket."""
    qs = urllib.parse.urlparse(path).query
    params = urllib.parse.parse_qs(qs)
    host_list = params.get("host")
    port_list = params.get("port")
    if not host_list or not port_list:
        raise ValueError("missing ?host=&port= query parameters")
    host = host_list[0].strip()
    port = int(port_list[0])
    if not host:
        raise ValueError("empty host")
    parts = host.split(".")
    if len(parts) != 4 or not all(p.isdigit() and 0 <= int(p) <= 255 for p in parts):
        raise ValueError(f"invalid IPv4: {host}")
    if port < PORT_MIN or port > PORT_MAX:
        raise ValueError(f"port {port} outside allowed range {PORT_MIN}-{PORT_MAX}")
    if not ALLOW_ANY and not _allowlist.allows(host, port):
        raise ValueError(
            f"{host}:{port} is not in the master list "
            f"(set COD2_RELAY_ALLOW_ANY=1 to skip the check)")
    check_ticket(params, host, port)
    return host, port


async def relay_handler(ws):
    """Handle one browser client → one CoD2 server session."""
    try:
        path, headers = _ws_path_and_headers(ws)
        host, port = parse_target(path, headers)
    except Exception as exc:
        log.warning("bad handshake: %s", exc)
        await ws.close(4400, str(exc))
        return

    target = (host, port)
    ip = client_ip(ws, headers)
    if not _conns.acquire(ip):
        log.warning("refused %s: more than %d sessions from this address", ip, MAX_PER_IP)
        await ws.close(4429, "too many sessions from this address")
        return
    log.info("session open → %s:%d from %s", host, port, ip)

    loop = asyncio.get_running_loop()
    udp_transport = None
    udp_protocol = None
    outbox = DropOldestQueue(SEND_QUEUE)
    upstream = TokenBucket(UP_PPS, max(UP_PPS, 1.0))
    up_dropped = 0

    class UdpRelay(asyncio.DatagramProtocol):
        def connection_made(self, transport):
            log.debug("udp socket bound to %s", transport.get_extra_info("sockname"))

        def datagram_received(self, data: bytes, addr: tuple):
            if addr[0] != target[0] or addr[1] != target[1]:
                log.debug("udp rx from %s dropped: does not match %s", addr, target)
                return
            outbox.put(data)

        def error_received(self, exc):
            log.warning("udp error: %s", exc)

    async def pump() -> None:
        """Single writer per session: preserves order, no task per packet."""
        try:
            while True:
                await ws.send(await outbox.get())
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001 - closed socket ends the pump
            log.debug("udp->ws pump ended: %s", exc)

    sender = None
    try:
        # AF_INET and an explicit local_addr: with family=0 asyncio cannot pick
        # a family and every session died with "unexpected address family"; with
        # only a family and no local_addr Windows handed back an unbound socket
        # (sockname None) whose sendto failed with WSAEINVAL (10022).
        # parse_target only accepts IPv4, so the family is known.
        udp_transport, udp_protocol = await loop.create_datagram_endpoint(
            UdpRelay, local_addr=("0.0.0.0", 0), family=socket.AF_INET)
        sender = asyncio.ensure_future(pump())

        async for message in ws:
            if not isinstance(message, (bytes, bytearray)):
                log.debug("ws rx non-binary frame (%s)", type(message).__name__)
                continue
            if len(message) > MAX_DGRAM:
                log.warning("oversized ws frame %d from %s", len(message),
                            ws.remote_address if hasattr(ws, 'remote_address') else "?")
                continue
            if not upstream.allow():
                up_dropped += 1
                continue
            udp_transport.sendto(message, target)

    except websockets.exceptions.ConnectionClosed:
        pass
    except Exception as exc:
        log.error("session error: %s", exc)
    finally:
        if sender:
            sender.cancel()
        if udp_transport:
            udp_transport.close()
        _conns.release(ip)
        log.info("session closed → %s:%d (dropped down=%d up=%d)", host, port,
                 outbox.dropped, up_dropped)


async def main():
    stop = asyncio.get_running_loop().create_future()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            asyncio.get_running_loop().add_signal_handler(sig, stop.set_result, None)
        except NotImplementedError:
            pass

    if not ALLOW_ANY:
        _allowlist.start()

    async with websockets.server.serve(
        relay_handler,
        RELAY_HOST,
        RELAY_PORT,
        max_size=MAX_DGRAM + 256,
        compression=None,
        ping_interval=20,
        ping_timeout=30,
    ):
        log.info("cod2 game relay on ws://%s:%d/  (ports %d-%d, allowlist=%s, tickets=%s)",
                 RELAY_HOST, RELAY_PORT, PORT_MIN, PORT_MAX,
                 "off" if ALLOW_ANY else MASTERLIST_URL,
                 "required" if SIGNER else "off")
        try:
            await stop
        except asyncio.CancelledError:
            pass

    log.info("relay stopped")


if __name__ == "__main__":
    asyncio.run(main())
