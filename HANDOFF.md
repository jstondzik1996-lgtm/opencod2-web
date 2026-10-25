# CoD2 browser multiplayer handoff

Updated: 2026-10-25 (Claude: entity-list + temp box fix, integration pending). This is an unfinished port, not a finished multiplayer release.

## Goal and boundaries

Run the original Call of Duty 2 1.3 multiplayer client inside a browser, without a running Steam client or native game client. A native dedicated server is acceptable. Singleplayer is not a prerequisite for multiplayer and is outside the current work. Never commit or publish IWD archives, game binaries, CD keys, browser profiles, or proprietary game assets. Players supply their own legally obtained assets; public asset distribution remains out of scope.

## Verified progress

- Emscripten 3.1.74 diagnostic build succeeds; Windows x86 dedicated server builds with w64devkit 2.10.1/GCC 16.2 and static zlib.
- Local Steam installation was found and verified as 1.3. All 28 official English/base IWD archives pass ZIP CRC checks. Custom mods are excluded.
- The browser renders the original menu and Toujane world, weapon and HUD, initializes WebGL2/Web Audio, and loads original assets locally.
- Two independent browser pages connect to the reconstructed local dedicated server, have distinct client numbers, join opposing teams, spawn with health 100, receive advancing snapshots, move, and consume ammunition when firing.
- The controlled damage test FAILS: target health remains 100. Do not claim playable combat or a completed port. See `docs/evidence/two-client-partial.json` for the earlier successful movement/firing run and failed damage assertion.
- A previous disconnect VM crash, wrong snapshot client indexing, missing WASM data pointers, and persistent HTTP HEAD/GET bug have been repaired. Python regression tests pass.
- Not verified: all maps, stock-server interoperability, Internet play, performance on other hardware, campaign, full round lifecycle. Headless rendering ran around 10–12 FPS in a diagnostic build; actual GPU renderer was not measured.

## Current blocker and next experiment

Player bullets hit world entity 1022 and did not damage the second player. Claude (2026-10-25, commit after 774cd3a) analysed and applied the paired fix:

1. `cm_trace.c` `CM_TempBoxModel` now also sets `model->leaf.brushContents = contents`. Without it `CM_LinkEntity` saw contents 0 and unlinked every player hull, AND the leaf test in `cm_trace.c` (`if (!leaf || !(leaf->brushContents & tw->contents))`) skipped the player box even when it was a candidate.
2. `cm_world.c` `CM_WorldEntityIndex` now returns `ent - svEntities + 1` and `CM_WorldEntityForIndex` returns `svEntities[index - 1]` (via `imp_sv`, like every other reader). Every reader in the file (`CM_UnlinkEntity`, `CM_AreaEntities_r`, all trace/sight traversals) already used `index - 1` with 0 as the end-of-list sentinel; only the two writers (`CM_InsertEntityIntoWorldSector`, `CM_SortNode`) were zero-based. That explains the earlier hang: `CM_UnlinkEntity` loops `while (1)` until it finds the entity, and with the off-by-one encoding it never does. Entity 0 (first player) was also stored as the sentinel and silently lost.

Verified so far (cloud, no game assets): `tests/cm_world_link/run.sh` builds the REAL `cm_world.c` as 32-bit x86 with stubs; old code fails (entity 0 missing, then hangs), fixed code passes 4000 random link/move/unlink steps compared against a brute-force scan. Python tests pass. Both changed files pass a 32-bit clang syntax check.

NOT yet verified: Windows/MinGW build, Emscripten build, and the real two-client damage test. This is a hull/list unit test, not combat.

### Instructions for the local Windows agent ("work")

1. `git pull` this branch in `work/opencod2-web`. Rebuild `build-win32` (server) and `build/web` (client) exactly as in "Build and test" below.
2. Copy the new `cod2_win32_ded.exe` into a SEPARATE test server root (not the 8080 preview server). Start it on 28961 with `g_debugBullets 6`, web service on 8081/20721/20722 (env vars below). Leave 8080/20711/20712/28960 running untouched.
3. Run `tools/browser_multiplayer_check.py` with `COD2_TEST_URL=http://127.0.0.1:8081/cod2.html`, `COD2_TEST_SERVER=127.0.0.1:28961`, separate `COD2_TEST_OUTPUT`.
4. Expected log: `bullet candidate: entity=<other client>` followed by `bullet trace: ... hit=<client number>` (not 1022). Target health must drop below 100.
5. Then verify manually / by script: kill -> death, respawn, disconnect of one client while the other keeps receiving advancing snapshots, reconnect. Server must not hang (watch CPU and `serverTime`).
6. If the server hangs or crashes: capture the server log tail and the last `bullet` lines into `docs/evidence/`, revert only on the test server, and report in WORK_LOG. Do not update the 8080 preview until steps 3–5 pass.
7. Save the sanitized result JSON as `docs/evidence/two-client-damage.json` (no CD key, no profile paths), update this file and WORK_LOG, commit and push.

`sv_world_mp.c` allocates four rows for the affine transform used by MatrixTransposeTransformVector43 and supplies entity origin; this fixed a real out-of-bounds read but did not alone restore hits. Optional `g_debugBullets 6` logging records ray/candidate evidence. Remove the noisy instrumentation once damage is proven.

## Product direction requested by the user (2026-10-25)

The user wants the browser build to join a game immediately or pick from a list of OUR servers (the project's own dedicated servers via `tools/master_proxy` / relay), not the old stock servers. Scope that is fine to build: own master list + server picker in the browser UI, auto-connect, our dedicated servers configured for this project's clients. Boundaries that stay: no publishing of game assets, keys or profiles; players still load their own legally obtained game files. Do not implement cracking/key generation or a hosted copy of the game for people who do not own it. Discuss key-auth handling on our own servers with the user before implementing it.

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

Local web service: `python tools/serve_web_local.py --port 8081 --master-port 20721 --relay-port 20722`. Set `COD2_RELAY_ALLOW_ANY=1`, `COD2_PORT_MIN=28961`, `COD2_PORT_MAX=28961` for the isolated local relay.

Integration harness: `tools/browser_multiplayer_check.py` requires Playwright Chromium, the test-export build, local assets, and the devmap server. Environment variables `COD2_TEST_URL`, `COD2_TEST_SERVER`, and `COD2_TEST_OUTPUT` select isolated endpoints/profile/results. It creates two real clients and sends normal game commands; aim and teleport are fixtures, while collision and damage run in the real server. A failing assertion must remain reported as failing.

## Continue documenting

Update this file and `docs/WORK_LOG.md` after each meaningful verified change. Commit code, reproduction steps and sanitized test results together. Push to the user's fork. Keep incomplete work explicit so another assistant can continue without repeating failed experiments. Do not commit build products, asset archives or local credentials. Do not overwrite the known-good preview with an unverified experimental server.

