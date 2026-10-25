# Stage 2 — playing from the website without a local game install

Goal: a player opens the page, picks a nick, presses **Graj** and plays: no
Steam, no installed CoD2, no folder picker, no key entry. This document
describes how code and assets are served. **Public launch waits for rights to
every distributed asset**; until then this runs only as a private test
environment (see "Private test environment").

## Components (all in this repository)

| Piece | What it does | Where |
|---|---|---|
| Static client | `cod2.html/js/wasm`, launcher, lobby | `build/web`, `src/web` |
| Asset manifest | list of archives with size + version | `assets.json` from `tools/gen_assets_manifest.py` |
| Asset loader | downloads archives once into browser storage (OPFS), re-downloads only changed versions, then boots | `src/web/cod2_fs.js` (`ingestRemote`) |
| Asset pack builder | MP-only trimmed archives | `tools/build_web_mp_pack.py` |
| Replacement packs | `zz_overlay_<name>.iwd` overriding individual files | `tools/build_overlay_pack.py` |
| Server directory + Quick Play | our servers, slot reservations | `tools/master_proxy` |
| Player sessions | nick + guest id, join tickets, bans | `tools/master_proxy/sessions.py` |
| Relay | WebSocket <-> UDP gateway, admission, limits | `tools/game_proxy/relay.py` |
| Dedicated servers | LAN mode, loopback/private only, supervised | `tools/run_server_fleet.py` |

## Flow

1. Page load: launcher finds a remote manifest (`COD2_REMOTE_ASSETS`, `?assets=` or `assets.json`).
2. First visit downloads the archives into OPFS (~2.5 GB for the current private pack; later visits reuse the cache; a changed archive is fetched again because the manifest version differs).
3. Lobby: nick -> `/play/api/session`; **Graj** -> `/play/api/matchmake` returns a server and a join ticket.
4. Engine boots with `+set name <nick> +connect <ip:port>`; the WebSocket to the relay carries the ticket; the relay forwards UDP to the LAN-mode dedicated server.
5. Full server -> lobby retries another one; in-game reconnect uses the session token.

## Replacing individual assets

If rights cannot be obtained for specific files, put legally usable
replacements under the same game paths and build an overlay:

```
python tools/build_overlay_pack.py replacements/ --name licensed -o <web>/main/zz_overlay_licensed.iwd --base <web>/main
python tools/gen_assets_manifest.py <web> -o <web>/assets.json
```

The report lists which base archive each replaced path came from and flags
paths that do not exist in the game. Engine, netcode and lobby are unchanged.
With `sv_pure 1` the dedicated servers need the same overlay. A fully
standalone public build means every distributed file is original or licensed;
the overlay mechanism lets that happen file by file.

## Private test environment (until rights are settled)

- Original archives are never committed, published, or put on an open endpoint.
- Local only: `serve_web_local.py` binds to 127.0.0.1 by default.
- Shared test host: `serve_web_local.py --access-key <long key>` (or the equivalent check in nginx). The launcher page is public; `assets.json`, `main/`, `zone/` and archive files return 403 until a tester opens the page once with `?access=<key>` (sets an HttpOnly cookie with a derived token, then removes the key from the URL). Starting on a non-loopback address without a key prints a warning.
- Testers must be people entitled to the files (e.g. owners of the game) in the private phase.

## Pending / not verified

- End-to-end on the real stack: remote asset download -> Quick Play -> two players -> combat.
- Download size: consider per-map packs and HTTP range/resume for slow links.
- Production nginx config for the access cookie, TLS, caching headers.
