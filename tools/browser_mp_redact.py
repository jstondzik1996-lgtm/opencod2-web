"""Redact game keys and short-lived project admission tokens from test logs.

The browser's WebSocket URL contains a server-bound ticket or guest session.
Playwright's websocket event exposes that URL verbatim; no admission token
should ever be written to evidence shared in docs/evidence or CI logs.
"""
from __future__ import annotations

import re

# The tokens appear as ordinary URL query args in JSON-serialized events.
_TOKEN_QUERY = re.compile(r'([?&](?:ticket|session|access|token)=)[^&" ]+', re.I)


_TOKEN_JSON = re.compile(r'("(?:ticket|session|token|playerKey|codkey|access_key)"\s*:\s*")[^"]*', re.I)
_BEARER = re.compile(r"(Authorization:\s*Bearer\s+)\S+", re.I)

def sanitize_test_log(value: str, installed_key: str | None = None) -> str:
    value = _TOKEN_QUERY.sub(lambda match: match.group(1) + '[redacted]', value)
    value = _TOKEN_JSON.sub(lambda match: match.group(1) + '[redacted]', value)
    value = _BEARER.sub(lambda match: match.group(1) + '[redacted]', value)
    if installed_key:
        value = value.replace(installed_key, '[redacted]')
        if len(installed_key) >= 16:
            value = value.replace(installed_key[:16], '[redacted]')
    return value
