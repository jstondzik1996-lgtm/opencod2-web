"""Project-owned server directory and Quick Play matchmaking.

The directory lists only dedicated servers the project runs (configured in a
JSON file), not arbitrary public CoD2 servers. A background poller sends the
standard connectionless ``getstatus`` query to each one and records map, game
type, player count, max clients and round-trip time. Servers that stop
answering are reported offline after ``stale_after`` seconds.

Quick Play picks the server with a free slot that best fills existing games
(most players first, then lowest ping). Each pick creates a short-lived
reservation so that concurrent joins do not all target the last slot of the
same server. The server itself stays authoritative: if it still refuses the
client ("Server is full"), the launcher asks again with that server excluded.

Configuration file (``COD2_SERVERS_FILE``), see servers.example.json:

    {"servers": [{"id": "eu-1", "name": "EU #1", "region": "eu",
                  "host": "127.0.0.1", "port": 28961,
                  "mode": "dm", "maxPlayers": 32}]}

``host`` must be an IPv4 literal: the browser relay only accepts those and it
is the address the relay host uses to reach the server.
"""

from __future__ import annotations

import json
import re
import secrets
import select
import socket
import threading
import time
from dataclasses import dataclass, field

STATUS_QUERY = b"\xff\xff\xff\xffgetstatus"
_RESPONSE_HEAD = b"\xff\xff\xff\xffstatusResponse"
_PLAYER_RE = re.compile(r'^\s*(-?\d+)\s+(-?\d+)\s+"(.*)"\s*$')
_ID_RE = re.compile(r"^[A-Za-z0-9_.-]{1,32}$")
_COLOR_RE = re.compile(r"\^\d")


class ConfigError(ValueError):
    pass


def udp_query_socket() -> socket.socket:
    """Non-blocking UDP socket for status/ping sweeps.

    On Windows a datagram to a closed port makes the NEXT recvfrom() fail with
    WSAECONNRESET (10054). Without handling, one offline server ended the whole
    sweep and every other server was reported offline. Disable that behaviour
    where possible; the receive loops also skip ConnectionResetError."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    if hasattr(socket, "SIO_UDP_CONNRESET"):
        try:
            sock.ioctl(socket.SIO_UDP_CONNRESET, False)
        except OSError:
            pass
    return sock


def _ipv4(text: str) -> bool:
    parts = str(text).split(".")
    return len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts)


@dataclass
class ServerEntry:
    id: str
    name: str
    host: str
    port: int
    region: str = ""
    mode: str = ""
    max_players: int = 32
    private: bool = False
    # Live state from getstatus.
    online: bool = False
    last_seen: float = 0.0
    ping_ms: int = 999
    map: str = ""
    gametype: str = ""
    hostname: str = ""
    server_max: int = 0
    players: list = field(default_factory=list)
    password: bool = False

    @property
    def capacity(self) -> int:
        # The server's own sv_maxclients wins when it is lower than the config.
        if self.server_max > 0:
            return min(self.max_players, self.server_max)
        return self.max_players


def parse_status(data: bytes) -> dict | None:
    """Parses a statusResponse datagram into rules and a player list."""
    if not data.startswith(_RESPONSE_HEAD):
        return None
    text = data[len(_RESPONSE_HEAD):].decode("latin-1", errors="replace")
    lines = text.split("\n")
    # lines[0] is what followed "statusResponse" on the first line (usually "").
    if len(lines) < 2:
        return None
    info = lines[1]
    tokens = info.split("\\")
    if tokens and tokens[0] == "":
        tokens = tokens[1:]
    rules = {}
    for i in range(0, len(tokens) - 1, 2):
        rules[tokens[i].lower()] = tokens[i + 1]
    players = []
    for line in lines[2:]:
        m = _PLAYER_RE.match(line)
        if m:
            players.append({"score": int(m.group(1)), "ping": int(m.group(2)),
                            "name": m.group(3)})
    return {"rules": rules, "players": players}


def load_config(path: str) -> list[ServerEntry]:
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    return parse_config(data)


def parse_config(data) -> list[ServerEntry]:
    items = data.get("servers") if isinstance(data, dict) else data
    if not isinstance(items, list):
        raise ConfigError("servers file must contain a servers[] list")
    seen_ids: set[str] = set()
    seen_addrs: set[tuple[str, int]] = set()
    out: list[ServerEntry] = []
    for raw in items:
        if not isinstance(raw, dict):
            raise ConfigError("server entry must be an object")
        sid = str(raw.get("id", "")).strip()
        if not _ID_RE.match(sid):
            raise ConfigError("invalid server id %r" % sid)
        host = str(raw.get("host", "")).strip()
        if not _ipv4(host):
            raise ConfigError("server %s: host must be an IPv4 literal" % sid)
        try:
            port = int(raw.get("port"))
        except (TypeError, ValueError):
            raise ConfigError("server %s: invalid port" % sid) from None
        if not 0 < port < 65536:
            raise ConfigError("server %s: port out of range" % sid)
        try:
            max_players = int(raw.get("maxPlayers", 32))
        except (TypeError, ValueError):
            raise ConfigError("server %s: invalid maxPlayers" % sid) from None
        if not 1 <= max_players <= 64:
            raise ConfigError("server %s: maxPlayers must be 1-64" % sid)
        if sid in seen_ids:
            raise ConfigError("duplicate server id %s" % sid)
        if (host, port) in seen_addrs:
            raise ConfigError("duplicate address %s:%d" % (host, port))
        seen_ids.add(sid)
        seen_addrs.add((host, port))
        out.append(ServerEntry(
            id=sid,
            name=str(raw.get("name") or sid)[:64],
            host=host,
            port=port,
            region=str(raw.get("region", ""))[:16].lower(),
            mode=str(raw.get("mode", ""))[:16].lower(),
            max_players=max_players,
            private=bool(raw.get("private", False)),
        ))
    return out


class ServerDirectory:
    def __init__(self, entries: list[ServerEntry], *, poll_interval: float = 2.0,
                 response_window: float = 1.0, stale_after: float = 10.0,
                 reservation_ttl: float = 30.0, clock=time.time) -> None:
        self._lock = threading.Lock()
        self._entries = {e.id: e for e in entries}
        self._order = [e.id for e in entries]
        self._by_addr = {(e.host, e.port): e for e in entries}
        self.poll_interval = poll_interval
        self.response_window = response_window
        self.stale_after = stale_after
        self.reservation_ttl = reservation_ttl
        self._clock = clock
        # token -> (server_id, expires_at)
        self._reservations: dict[str, tuple[str, float]] = {}
        self.sweeps = 0

    # ---- polling ------------------------------------------------------------
    def poll_once(self) -> None:
        with self._lock:
            targets = list(self._by_addr)
        if not targets:
            return
        sock = udp_query_socket()
        sent: dict[tuple[str, int], float] = {}
        got: dict[tuple[str, int], tuple[dict, int]] = {}
        try:
            for addr in targets:
                try:
                    sock.sendto(STATUS_QUERY, addr)
                    sent[addr] = time.perf_counter()
                except OSError:
                    continue
            deadline = time.perf_counter() + self.response_window
            while len(got) < len(sent):
                remaining = deadline - time.perf_counter()
                if remaining <= 0 or not select.select([sock], [], [], remaining)[0]:
                    break
                try:
                    data, src = sock.recvfrom(16384)
                except ConnectionResetError:
                    continue   # ICMP port unreachable from an offline server
                except OSError:
                    break
                if src not in sent or src in got:
                    continue
                parsed = parse_status(data)
                if parsed is None:
                    continue
                rtt = int(round((time.perf_counter() - sent[src]) * 1000.0))
                got[src] = (parsed, min(max(rtt, 1), 998))
        finally:
            sock.close()

        now = self._clock()
        with self._lock:
            for addr, (parsed, rtt) in got.items():
                e = self._by_addr.get(addr)
                if e is None:
                    continue
                rules = parsed["rules"]
                e.last_seen = now
                e.ping_ms = rtt
                e.map = rules.get("mapname", "")
                e.gametype = rules.get("g_gametype", "")
                e.hostname = _COLOR_RE.sub("", rules.get("sv_hostname", ""))
                try:
                    e.server_max = int(rules.get("sv_maxclients", "0"))
                except ValueError:
                    e.server_max = 0
                e.password = rules.get("pswrd", "0") not in ("", "0")
                e.players = parsed["players"]
            for e in self._entries.values():
                e.online = e.last_seen > 0 and now - e.last_seen <= self.stale_after
                if not e.online:
                    e.ping_ms = 999
            self.sweeps += 1

    def run_forever(self) -> None:
        while True:
            started = time.time()
            try:
                self.poll_once()
            except Exception as exc:  # noqa: BLE001 - polling must not die
                print("directory poll failed: %s" % exc, flush=True)
            delay = self.poll_interval - (time.time() - started)
            if delay > 0:
                time.sleep(delay)

    def start(self) -> None:
        threading.Thread(target=self.run_forever, name="directory", daemon=True).start()

    # ---- views --------------------------------------------------------------
    def _reserved_locked(self, now: float) -> dict[str, int]:
        for token, (_, exp) in list(self._reservations.items()):
            if exp <= now:
                del self._reservations[token]
        counts: dict[str, int] = {}
        for sid, _ in self._reservations.values():
            counts[sid] = counts.get(sid, 0) + 1
        return counts

    def _is_online(self, e: ServerEntry, now: float) -> bool:
        """Never advertise stale server status if the polling thread stalls.

        e.online is the last poller's verdict, but must not outlive
        stale_after when a poll fails to run at all (CPU stalls/shutdown).
        """
        return e.online and e.last_seen > 0 and now - e.last_seen <= self.stale_after

    def _public(self, e: ServerEntry, reserved: int, now: float) -> dict:
        players = len(e.players)
        online = self._is_online(e, now)
        return {
            "id": e.id,
            "name": e.name,
            "region": e.region,
            "mode": e.mode or e.gametype,
            "host": e.host,
            "port": e.port,
            "connect": "%s:%d" % (e.host, e.port),
            "map": e.map,
            "gametype": e.gametype,
            "players": players,
            "maxPlayers": e.capacity,
            "reserved": reserved,
            "free": max(0, e.capacity - players - reserved) if online else 0,
            "status": "online" if online else "offline",
            "lastSeen": int(e.last_seen) if e.last_seen else None,
            "ping_ms": e.ping_ms,
            "password": e.password,
        }

    def list_servers(self, include_private: bool = False) -> list[dict]:
        now = self._clock()
        with self._lock:
            reserved = self._reserved_locked(now)
            return [self._public(self._entries[sid], reserved.get(sid, 0), now)
                    for sid in self._order
                    if include_private or not self._entries[sid].private]

    def masterlist(self) -> dict:
        """Online public servers in the api.cod.pm shape the engine already reads."""
        now = self._clock()
        out = []
        with self._lock:
            for sid in self._order:
                e = self._entries[sid]
                if e.private or not self._is_online(e, now):
                    continue
                out.append({
                    "ip": e.host,
                    "port": e.port,
                    "sv_hostname": e.name,
                    "mapname": e.map,
                    "g_gametype": e.gametype,
                    "clients": len(e.players),
                    "sv_maxclients": e.capacity,
                    "pswrd": 1 if e.password else 0,
                    "sv_pure": 1,
                    "ping_ms": e.ping_ms,
                    "playerinfo": [dict(p) for p in e.players],
                    "directory_id": e.id,
                })
        return {"servers": out, "source": "project", "generated": int(now)}

    def allows(self, host: str, port: int) -> bool:
        with self._lock:
            return (host, port) in self._by_addr

    # ---- matchmaking --------------------------------------------------------
    def matchmake(self, mode: str = "", region: str = "", exclude=()) -> dict | None:
        mode = (mode or "").lower()
        region = (region or "").lower()
        excluded = set(exclude or ())
        now = self._clock()
        with self._lock:
            reserved = self._reserved_locked(now)
            best = None
            best_key = None
            for sid in self._order:
                e = self._entries[sid]
                if e.private or not self._is_online(e, now) or e.password or sid in excluded:
                    continue
                if mode and mode not in (e.mode, e.gametype.lower()):
                    continue
                if region and e.region != region:
                    continue
                occupied = len(e.players) + reserved.get(sid, 0)
                if occupied >= e.capacity:
                    continue
                # Fill games that already have people, then prefer low ping.
                key = (-occupied, e.ping_ms, self._order.index(sid))
                if best_key is None or key < best_key:
                    best, best_key = e, key
            if best is None:
                return None
            return self._reserve_locked(best, reserved, now)

    def _reserve_locked(self, e: ServerEntry, reserved: dict, now: float) -> dict:
        token = secrets.token_urlsafe(12)
        self._reservations[token] = (e.id, now + self.reservation_ttl)
        reserved[e.id] = reserved.get(e.id, 0) + 1
        result = self._public(e, reserved[e.id], now)
        result["reservation"] = token
        result["expiresIn"] = int(self.reservation_ttl)
        return result

    def reserve(self, server_id: str, include_private: bool = False):
        """Reserves a slot on one named server (join from the list or a
        private room). Returns (result, None) or (None, reason) with reason in
        unknown / offline / full / password."""
        now = self._clock()
        with self._lock:
            e = self._entries.get(server_id)
            if e is None or (e.private and not include_private):
                return None, "unknown"
            if not self._is_online(e, now):
                return None, "offline"
            if e.password:
                return None, "password"
            reserved = self._reserved_locked(now)
            if len(e.players) + reserved.get(server_id, 0) >= e.capacity:
                return None, "full"
            return self._reserve_locked(e, reserved, now), None

    def release(self, token: str) -> bool:
        with self._lock:
            return self._reservations.pop(token, None) is not None
