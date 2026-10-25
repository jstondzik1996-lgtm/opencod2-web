# CoD2 browser multiplayer handoff

Updated: 2026-10-25 (Claude: collision fix + project server directory, lobby and Quick Play; combat integration still pending). This is an unfinished port, not a finished multiplayer release.

## Goal and boundaries

Run the original Call of Duty 2 1.3 multiplayer client inside a browser, without a running Steam client or native game client. A native dedicated server is acceptable. Singleplayer is not a prerequisite for multiplayer and is outside the current work. Never commit or publish IWD archives, game binaries, CD keys, browser profiles, or proprietary game assets. Players supply their own legally obtained assets; public asset distribution remains out of scope.

## Verified progress

- Emscripten 3.1.74 diagnostic build succeeds; Windows x86 dedicated server builds with w64devkit 2.10.1/GCC 16.2 and static zlib.
- Local Steam installation was found and verified as 1.3. All 28 official English/base IWD archives pass ZIP CRC checks. Custom mods are excluded.
- The browser renders the original menu and Toujane world, weapon and HUD, initializes WebGL2/Web Audio, and loads original assets locally.
- Two independent browser pages connect to the reconstructed local dedicated server, have distinct client numbers, join opposing teams, spawn with health 100, receive advancing snapshots, move, and consume ammunition when firing.
- The controlled damage test FAILS: target health remains 100. Do not claim playable combat or a completed port. See `docs/evidence/two-client-partial.json` for the earlier successful movement/firing run and failed damage assertion.
- A previous disconnect VM crash, wrong snapshot client indexing, missing WASM data pointers, and persistent HTTP HEAD/GET bug have been repaired. Python regression tests pass.
- Project server directory, `/play/api/servers`, Quick Play matchmaking with slot reservations, relay allowlist from the directory and the launcher lobby are implemented and tested in the cloud against fake UDP servers and a stubbed engine (see "Own servers and lobby"). Not yet tested with the real dedicated server or real WASM client.
- Not verified: all maps, stock-server interoperability, Internet play, performance on other hardware, campaign, full round lifecycle. Headless rendering ran around 10–12 FPS in a diagnostic build; actual GPU renderer was not measured.

## P0 status (Claude, 2026-10-25, latest)

Two independent blockers were on the bullet path; both are fixed in code, neither is proven by a real two-client run yet:
1. Player hulls were never linked for collision (temp box leaf contents 0) and the world-sector lists mixed 0/1-based indices. Fixed in `cm_load_obj.c` (leaf contents -1 like the original) and `cm_world.c`.
2. `ClientEndFrame` never set `takedamage = 1` for living players, so every hit was discarded before damage. Fixed in `g_active_mp.c` (also viewmodelIndex and PM_DEAD selection).
Asset-free tests: `tests/cm_world_link/run.sh`, `tests/dobj_trace/run.sh`, Python suite; all run in CI. Next proof needed: `tools/browser_multiplayer_check.py` on the local Windows stack.

## Player sessions on project servers (P2, Claude 2026-10-25)

Model: project dedicated servers run in the original game's LAN mode (`net_lanauthorize 0`, bound to `127.0.0.1` or a private address, UDP port NOT reachable from the Internet). Browsers reach them only through the project relay, so the server sees LAN clients and the client sees a LAN server: neither side contacts an external authorization service and no CD key is involved. Identity and admission are the project's own and are enforced at the relay:
- `POST /play/api/session {nick, playerKey}`: the browser keeps a random `playerKey` in localStorage; `pid` = hash of it (stable guest id, nothing stored server-side). Nick is cleaned to `[A-Za-z0-9_-.[]]`, 2–16 chars (no userinfo/command injection).
- `/matchmake` and `/join` require the session when `COD2_SESSION_SECRET` is set and return a 90 s join ticket bound to `ip:port`.
- Relay admits a WebSocket only with a ticket for exactly that server, or a valid session token (in-game reconnect / in-game server list), and refuses banned pids (`COD2_BANS_FILE`, one pid per line, reloaded on change).
- Lobby: nick field + "Graj"; passes `+set name <nick> +connect ip:port`; publishes `COD2_RELAY_TICKETS` / `COD2_RELAY_SESSION` for `src/web/web_net.js` (tokens are redacted in console logs).
- Local: `python tools/serve_web_local.py --servers FILE --sessions` (random secret shared by master proxy and relay for that run). Production: set the same `COD2_SESSION_SECRET` (>=16 bytes) for both services.
- Tests: `tools/test_sessions.py` (tokens, forgery/expiry, nick cleaning, bans, routes, relay admission), `tools/test_lobby_browser.py` (Chromium: nick required, session + ticket flow, full-server retry with a new ticket).
- Pending on the real stack: verify that a browser with NO key in preferences joins a LAN-mode project server through the relay (expected from the code paths in `cl_main_mp.c`/`sv_client_mp.c`), and that the in-game name equals the lobby nick.

## Current blocker and next experiment (history)

Player bullets hit world entity 1022 and did not damage the second player. Claude (2026-10-25, commit after 774cd3a) analysed and applied the paired fix:

1. `cm_trace.c` `CM_TempBoxModel` now also sets `model->leaf.brushContents = contents`. Without it `CM_LinkEntity` saw contents 0 and unlinked every player hull, AND the leaf test in `cm_trace.c` (`if (!leaf || !(leaf->brushContents & tw->contents))`) skipped the player box even when it was a candidate.
2. `cm_world.c` `CM_WorldEntityIndex` now returns `ent - svEntities + 1` and `CM_WorldEntityForIndex` returns `svEntities[index - 1]` (via `imp_sv`, like every other reader). Every reader in the file (`CM_UnlinkEntity`, `CM_AreaEntities_r`, all trace/sight traversals) already used `index - 1` with 0 as the end-of-list sentinel; only the two writers (`CM_InsertEntityIntoWorldSector`, `CM_SortNode`) were zero-based. That explains the earlier hang: `CM_UnlinkEntity` loops `while (1)` until it finds the entity, and with the off-by-one encoding it never does. Entity 0 (first player) was also stored as the sentinel and silently lost.

Verified so far (cloud, no game assets): `tests/cm_world_link/run.sh` builds the REAL `cm_world.c` as 32-bit x86 with stubs; old code fails (entity 0 missing, then hangs), fixed code passes 4000 random link/move/unlink steps compared against a brute-force scan. Python tests pass. Both changed files pass a 32-bit clang syntax check.

NOT yet verified: Windows/MinGW build, Emscripten build, and the real two-client damage test. This is a hull/list unit test, not combat.

### Instructions for the local Windows agent ("work")

1. `git pull` this branch in `work/opencod2-web`. Rebuild `build-win32` (server) and `build/web` (client) exactly as in "Build and test" below.
2. Copy the new `cod2_win32_ded.exe` into a SEPARATE test server root (not the 8080 preview server). Start it on 28961 with `g_debugBullets 6`, web service on 8081/20721/20722 (env vars below). Leave 8080/20711/20712/28960 running untouched.
3. Run `tools/browser_multiplayer_check.py` with `COD2_TEST_URL=http://127.0.0.1:8081/cod2.html`, `COD2_TEST_SERVER=127.0.0.1:28961`, separate `COD2_TEST_OUTPUT`. The harness now also checks `target_died`, `target_respawned` (fires on the victim page to respawn in DM), `server_alive_after_disconnect` (shooter snapshots advance for 6 s after the victim disconnects), `reconnected` and `server_alive_after_reconnect`. Raw samples are in `lifecycle` in the result JSON.
4. Expected log: `bullet candidate: entity=<other client>` followed by `bullet trace: ... hit=<client number>` (not 1022). Target health must drop below 100.
5. If the respawn check fails only because the game type needs a different respawn input, record the observed `lifecycle.respawn` samples and adjust the harness, not the server. Server must not hang (watch CPU and `serverTime`).
6. If the server hangs or crashes: capture the server log tail and the last `bullet` lines into `docs/evidence/`, revert only on the test server, and report in WORK_LOG. Do not update the 8080 preview until steps 3–5 pass.
7. Save the sanitized result JSON as `docs/evidence/two-client-damage.json` (no CD key, no profile paths), update this file and WORK_LOG, commit and push.

`sv_world_mp.c` allocates four rows for the affine transform used by MatrixTransposeTransformVector43 and supplies entity origin; this fixed a real out-of-bounds read but did not alone restore hits. Optional `g_debugBullets 6` logging records ray/candidate evidence. Remove the noisy instrumentation once damage is proven.

## Product direction (user, 2026-10-25) — see docs/ROADMAP.md

Working name: **WWII Browser Multiplayer**. Two tracks share one code base: (A) the port compatible with original CoD2 1.3, running on game files each player supplies locally; (B) a standalone public version with original or properly licensed maps, weapons, models, sounds, UI and branding. Never publish original IWD/models/sounds/textures, keys or Steam files. Identity on our servers must be our own (P2); no keygens, shared keys or bypassing other servers' authorization. The launcher shows the unofficial-project notice; it is attribution, not permission.

## Own servers and lobby (P1, Claude 2026-10-25)

Implemented:
- `tools/master_proxy/directory.py`: project server list from `COD2_SERVERS_FILE` (example: `tools/master_proxy/servers.example.json`), UDP `getstatus` polling every 2 s (map, game type, players, `sv_maxclients`, ping), offline after 10 s without answer, Quick Play choosing the busiest server with a free slot (then lowest ping), 30 s slot reservations so concurrent joins do not overbook, release endpoint.
- `tools/master_proxy/server.py` with a directory: `GET /servers`, `POST /matchmake {mode, region, exclude}`, `POST /matchmake/release`, and `/masterlist/cod2/1.3` returns ONLY our online servers (so the in-game Join Game list and the relay allowlist follow the directory). Public api.cod.pm list only at `/external/masterlist/...` with `COD2_EXTERNAL_MASTER=1`, labelled external. Matchmake rate limit 30/min per client. Without `COD2_SERVERS_FILE` the old public-list behaviour is unchanged (keeps the 8080 preview working).
- `tools/serve_web_local.py`: forwards POST, `--servers FILE`, serves `lobby.js`/`brand-mark.png` from `src/web` (no relink needed for lobby edits), and points the spawned relay at its own master port (test relay on 20722 no longer reads the 20711 preview list).
- `tools/game_proxy/relay.py`: allowlist refetch on a miss (at most every 5 s) so a server that came online after the last refresh works; an empty project list is accepted (no fallback to forwarding anywhere).
- `src/web/lobby.js` + shell: "Nasze serwery" panel with Quick Play and per-server Join. The choice is passed as `+connect ip:port` at engine boot. On `EXE_SERVERISFULL` Quick Play excludes that server and reconnects through the new export `Web_LobbyConnect` (`src/web/web_lobby.c`, accepts only `a.b.c.d:port`). The lobby hides itself when no project directory is configured.
- Branding: title "WWII Browser Multiplayer", new neutral helmet icon (old star-"2" icons removed), unofficial-project notice in the launcher.

Tests (cloud): `tools/test_server_directory.py` (fake UDP servers: parsing, offline/stale, fill-then-overflow, reservations, 12 concurrent matchmakes never exceed capacity, filters, HTTP routes, rate limit, POST through serve_web_local, relay allowlist), `tools/test_lobby_browser.py` (headless Chromium with the real shell/lobby/cod2_fs.js and a stubbed engine: list, Quick Play -> `+connect` to the fuller server, full-server retry -> other server, reservation released). The engine stub means this proves the launcher path only.

### Instructions for Work: P1 on the real stack (after P0)

1. Start two test dedicated servers from separate clean roots: ports 28961 and 28962, `sv_maxclients 2`, same config otherwise.
2. Create `tools/master_proxy/servers.local.json` (not committed) from the example with ports 28961/28962 and `maxPlayers` 2.
3. Rebuild the web client (shell + `web_lobby.c` changed). Start `python tools/serve_web_local.py --port 8081 --master-port 20721 --relay-port 20722 --servers tools/master_proxy/servers.local.json` WITHOUT `COD2_RELAY_ALLOW_ANY` (the relay must now accept only directory servers).
4. Check `http://127.0.0.1:8081/play/api/servers` shows both online. Open three browser profiles, press "Szybka gra" in each: expected A, A, then B. Kill server B: within ~10 s it shows offline and Quick Play no longer offers it. Confirm the relay refuses a non-directory port.
5. Also confirm the in-game Join Game menu lists only the two project servers.
6. Save the sanitized results to `docs/evidence/` and record them in WORK_LOG.

## Local Windows state (machine-specific, not portable paths)

Workspace parent: `C:\Users\PC\Documents\Codex\2026-10-25\referenced-chatgpt-conversation-this-is-an`.
Repository: `work/opencod2-web`. Portable SDK: `work/emsdk`; compiler: `work/w64devkit`; Python packages: `work/python-tools`.
Python: `C:\Users\PC\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe`.

Game: `C:\Program Files (x86)\Steam\steamapps\common\Call of Duty 2`.
Native clean server root: `work/native-server`; config: `main/browser-test.cfg` (devmap mp_toujane, DM, sv_pure 0, maxclients 4, downloads off).
Packed browser assets: `work/mp-assets/main`; `build/web/main` is a local junction to that directory. Five IWDs total approximately 2511 MiB. The extra iw_02 contains shared dependencies removed by the earlier filter. Regenerate a clean pack with the corrected packer for a portable setup.

Visible preview uses HTTP 8080, master proxy 20711, WS relay 20712, UDP server 28960. Keep these running while working; use separate test ports 8081 / 20721 / 20722 / 28961. Current PIDs and logs live in the workspace `work/` folder; verify executable paths before stopping processes. Browser preview profile is `work/user-preview`; automated test profile is separate. The in-app preview is http://127.0.0.1:8080/cod2.html . A Chrome window was also opened; actual current visual state has not been rechecked after restoring the server.

The test harness reads the user's installed CD key from the Windows registry into memory and browser-local preferences. It redacts console logs. Never print/export that key or sync profiles. No Steam process is required for this client/server setup.

## Build and test

Set PATH to the SDK/tool directories above, and PYTHONPATH to `work/python-tools` for Playwright/websockets. Emscripten's Python must point at the bundled Python above. Set TEMP/TMP to `work/tmp` if needed.

```powershell
emcmake cmake -S . -B build/web -G Ninja -DCMAKE_OBJECT_PATH_MAX=200 -DCOD2_WEB_SAFE_HEAP=ON -DCOD2_WEB_TESTING=ON
cmake --build build/web -j 8
cmake -S . -B build-win32 -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=gcc -DCMAKE_ASM_COMPILER=gcc -DCMAKE_RC_COMPILER=windres
cmake --build build-win32 -j 8
python -m unittest discover -s tools -p 'test_*.py'
```

Native server (working directory is its clean game root):
```text
cod2_win32_ded.exe +set dedicated 1 +set net_ip 127.0.0.1 +set net_port 28961 +set fs_basepath . +set fs_homepath . +exec browser-test.cfg
```

Local web service: `python tools/serve_web_local.py --port 8081 --master-port 20721 --relay-port 20722`. Set `COD2_RELAY_ALLOW_ANY=1`, `COD2_PORT_MIN=28961`, `COD2_PORT_MAX=28961` for the isolated local relay. With `--servers FILE` use the project directory instead and do NOT set `COD2_RELAY_ALLOW_ANY` (never set it on a public host). Do not set `COD2_REMOTE_ASSETS` to original IWDs on any public host.

Cloud-runnable tests (no assets): `python -m unittest discover -s tools -p 'test_*.py'` (the lobby browser test skips without Playwright Chromium) and `tests/cm_world_link/run.sh` (clang + GNU ld, i386).

Integration harness: `tools/browser_multiplayer_check.py` requires Playwright Chromium, the test-export build, local assets, and the devmap server. Environment variables `COD2_TEST_URL`, `COD2_TEST_SERVER`, and `COD2_TEST_OUTPUT` select isolated endpoints/profile/results. It creates two real clients and sends normal game commands; aim and teleport are fixtures, while collision and damage run in the real server. A failing assertion must remain reported as failing.

## Continue documenting

### Shared collaboration (user instruction, 2026-10-25)

Codex, ChatGPT Work and Claude are all authorized to implement improvements in this same project and on the shared `codex/browser-mp-checkpoint` branch of `jstondzik1996-lgtm/opencod2-web`. Work is not restricted to review. Continue from the latest shared state; do not create separate branches or forks for each assistant.

Before editing, read the current work log and inspect local uncommitted changes. With separate checkouts, fetch the shared branch and integrate incoming commits before pushing; use ordinary non-force pushes. If a push is rejected because another assistant pushed first, integrate those changes while preserving both contributions, then retry. With a shared checkout, preserve the other assistant's in-progress edits and coordinate overlapping files through the work log/messages instead of resetting or overwriting them.

Record the assistant, current stage, files being changed, test results, remaining problems and next step in `docs/WORK_LOG.md`. These notes provide coordination, not an approval gate or exclusive ownership. Keep working on useful tasks, exchange findings, and update `HANDOFF.md` when the verified project status changes. All assistants may improve code and run tests; report actual results and push small coherent checkpoints.

Update this file and `docs/WORK_LOG.md` after each meaningful verified change. Commit code, reproduction steps and sanitized test results together. Push to the user's fork. Keep incomplete work explicit so another assistant can continue without repeating failed experiments. Do not commit build products, asset archives or local credentials. Do not overwrite the known-good preview with an unverified experimental server.

