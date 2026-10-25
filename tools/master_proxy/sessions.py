"""Player identity for the project's own servers.

No CD keys are involved. Project servers run in the original game's LAN mode
(net_lanauthorize 0) and are reachable only through the project's WebSocket
relay, so neither the client nor the server asks an external authorization
service. Identity and admission are handled here and enforced at the relay:

* Guest identity: the browser keeps a random ``playerKey`` (never sent to game
  servers). ``pid`` is derived from it, so the same browser keeps the same id
  without accounts; the backend stores nothing.
* Session token (hours): ``{typ: session, pid, nick}``.
* Join ticket (seconds): ``{typ: ticket, pid, nick, srv: "ip:port"}``. The
  relay only opens a session to ``srv`` with a valid, unexpired ticket and a
  pid that is not banned.

Tokens are ``base64url(json).base64url(HMAC-SHA256(secret, json))``. Master
proxy and relay share ``COD2_SESSION_SECRET``.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import re
import secrets
import threading
import time

SESSION_TTL = 12 * 3600
TICKET_TTL = 90

_NICK_ALLOWED = re.compile(r"[^A-Za-z0-9_\-.\[\]]")
_COLOR = re.compile(r"\^.")
_PID_RE = re.compile(r"^[0-9a-f]{16}$")
_KEY_RE = re.compile(r"^[A-Za-z0-9_-]{16,128}$")


class TokenError(ValueError):
    pass


def _b64e(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def _b64d(text: str) -> bytes:
    pad = "=" * (-len(text) % 4)
    return base64.urlsafe_b64decode(text + pad)


def clean_nick(raw) -> str | None:
    """Returns a nick safe for the CoD2 userinfo string, or None.

    Colour codes are removed and only [A-Za-z0-9_-.[]] is kept, so a nick can
    never inject userinfo separators (backslash), quotes, semicolons or '+'
    into the engine command line.
    """
    if not isinstance(raw, str):
        return None
    nick = _NICK_ALLOWED.sub("", _COLOR.sub("", raw.replace(" ", "_")))[:16]
    if len(nick) < 2:
        return None
    return nick


def player_id(player_key: str) -> str:
    return hashlib.sha256(("cod2web-pid:" + player_key).encode("ascii")).hexdigest()[:16]


def valid_player_key(key) -> bool:
    return isinstance(key, str) and bool(_KEY_RE.match(key))


class Signer:
    def __init__(self, secret: str | bytes, clock=time.time) -> None:
        if isinstance(secret, str):
            secret = secret.encode("utf-8")
        if len(secret) < 16:
            raise ValueError("session secret must be at least 16 bytes")
        self._secret = secret
        self._clock = clock

    def sign(self, payload: dict) -> str:
        body = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
        sig = hmac.new(self._secret, body, hashlib.sha256).digest()
        return _b64e(body) + "." + _b64e(sig)

    def verify(self, token, typ: str) -> dict:
        if not isinstance(token, str) or token.count(".") != 1 or len(token) > 2048:
            raise TokenError("malformed token")
        body_b64, sig_b64 = token.split(".")
        try:
            body = _b64d(body_b64)
            sig = _b64d(sig_b64)
        except (ValueError, TypeError):
            raise TokenError("malformed token") from None
        expected = hmac.new(self._secret, body, hashlib.sha256).digest()
        if not hmac.compare_digest(sig, expected):
            raise TokenError("bad signature")
        try:
            payload = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise TokenError("malformed payload") from None
        if not isinstance(payload, dict) or payload.get("typ") != typ or payload.get("v") != 1:
            raise TokenError("wrong token type")
        if not isinstance(payload.get("exp"), int) or payload["exp"] <= int(self._clock()):
            raise TokenError("expired")
        if not _PID_RE.match(str(payload.get("pid", ""))):
            raise TokenError("bad player id")
        return payload

    def session(self, player_key: str, nick: str) -> dict:
        now = int(self._clock())
        pid = player_id(player_key)
        token = self.sign({"v": 1, "typ": "session", "pid": pid, "nick": nick,
                           "iat": now, "exp": now + SESSION_TTL})
        return {"token": token, "pid": pid, "nick": nick, "expiresIn": SESSION_TTL}

    def ticket(self, session: dict, server: str) -> str:
        now = int(self._clock())
        return self.sign({"v": 1, "typ": "ticket", "pid": session["pid"], "nick": session["nick"],
                          "srv": server, "iat": now, "exp": now + TICKET_TTL,
                          "jti": secrets.token_hex(6)})


class BanList:
    """Player ids that may not join, from a text file (one pid per line, '#' comments).

    Reloaded when the file changes; a missing file means no bans.
    """

    def __init__(self, path: str | None) -> None:
        self.path = path
        self._lock = threading.Lock()
        self._mtime = None
        self._pids: set[str] = set()

    def _reload(self) -> None:
        if not self.path:
            return
        try:
            mtime = os.path.getmtime(self.path)
        except OSError:
            with self._lock:
                self._pids, self._mtime = set(), None
            return
        if mtime == self._mtime:
            return
        pids = set()
        with open(self.path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.split("#", 1)[0].strip().lower()
                if _PID_RE.match(line):
                    pids.add(line)
        with self._lock:
            self._pids, self._mtime = pids, mtime

    def banned(self, pid: str) -> bool:
        self._reload()
        with self._lock:
            return pid in self._pids


def signer_from_env() -> Signer | None:
    secret = os.environ.get("COD2_SESSION_SECRET", "")
    return Signer(secret) if secret else None
