# CoD2 1.3 dedicated-server configurations — verified reuse plan (2026-10-26)

> **User decision:** keep the original existing CoD2 C/Emscripten engine. Use working native-server configurations, commands and proven server behavior as REFERENCE; do not start a new FPS. This is an implementation/reference document, not an assertion that a real Windows test has passed.

## Sources checked

1. Community standalone CoD2 server setup and cvars: https://free.war24.net/guides/cod2-dedicated-server/ and https://free.war24.net/cvars/
2. GameServerManagers config: https://github.com/GameServerManagers/Game-Server-Configs/blob/main/cod2/server.cfg
3. Community example with map rotation and gameplay options: https://www.moddb.com/games/call-of-duty-2/tutorials/server-configuration-cfg-file-example
4. CoD2x 1.3 extension and protocol behavior: https://github.com/callofduty2x/CoD2x
5. CoD2rev dedicated source with AGPL-3.0: https://github.com/voron00/CoD2rev_Server
6. Extended CoD2rev fork (AGPL-3.0), WebSocket-related additions and zPAM: https://github.com/callofduty2x/CoD2rev_Server
7. Large community config example (contains unrelated mod/unsafe options; **reference only**): https://github.com/firebull/ghmanager/blob/master/scripts/servers_configs/cod/cod2/main/server.cfg
8. Server hosting guide with DM/CTF/TDM dvars: https://pingperfect.com/knowledgebase/617/Call-of-Duty-2--Server-Configuration.html

**License/provenance:** Use ideas, published settings and behavior tests; do not copy reverse-engineered engine implementation, downloadable binaries, unauthorized maps or paid mod assets into the public project without legal review. Config templates may contain mod-specific/obsolete options. Not every Cvar advertised by CoD2x or a plugin exists in vanilla/our port.

## What the existing fork ALREADY supports (verified in repository source on `codex/browser-mp-checkpoint`)

| Category | Dvars / commands | Implementation evidence | Project use |
|---|---|---|---|
| Server ticks and network | `sv_fps`, `sv_maxRate`, `sv_floodProtect`, `sv_timeout`, `sv_reconnectlimit` | `src/PC/server_mp/sv_init_mp.c` registers them; `sv_main_mp.c` uses them | Keep conservative 20 Hz server baseline, 25k B/s client cap, connection safeguards |
| Players / max slots | `sv_maxclients`, `sv_privateClients` | `sv_init_mp.c`, `sv_main_mp.c` | Start 4-slot private test; validate full game/entity allocation before moving to 16 (1+15 bots goal) |
| Assets / browser | `sv_pure`, `sv_allowDownload`, `fs_game` | `sv_init_mp.c`, `sv_main_mp.c` | Keep original IWD resources *on user device only*. No public download of proprietary files |
| Dedicated process | `dedicated`, `net_ip`, `net_port`, `+exec cfg` | `HANDOFF.md`, `tools/run_server_fleet.py`, launcher scripts | Dedicated 1 + bind 127.0.0.1 behind project relay; *not* dedicated 2 open UDP on the Internet for local QA |
| Game rules | `g_gametype`, `g_antilag`, `g_log`, `g_logSync` | `src/PC/game_mp/g_main_mp.c` registers them | Minimal DM to reproduce hits, stable logs; `g_logSync` is **boolean** in our source |
| Map rotation | `sv_mapRotation`, `sv_mapRotationCurrent` | `src/PC/server_mp/sv_init_mp.c` | One map `mp_toujane` for reproducible QA; additional maps later |
| Project server directory | UDP `getstatus`, `/play/api/servers`, `/play/api/matchmake`, relay session tickets | `tools/master_proxy/directory.py`, `HANDOFF.md` | Real native server supplies population/map/ping, browser lobby uses our own directory |

**Caution: `sv_fps` is NOT the number of browser rendered frames.** Typical original server tick is 20; CoD2x documents higher tick ceilings on its *patched* server. Do NOT copy `sv_fps 40` or `snaps 40` into our client/server and claim gains unless version/protocol/timing behavior was verified. The current engine source accepts an integer range but that does not prove network snapshots at a given rate are sound.

**Caution: inconsistent community templates.** Some public config examples use `rconpassword` (without underscore) — for *this* CoD2 source `sv_init_mp.c` registers **`rcon_password`**. Keep RCON disabled locally (`set rcon_password ""`), avoid storing an actual secret in repository configs, and don't enable anonymous Internet admin endpoints.

## Ready-to-review file added

[`docs/server_configs/cod2_13_browser_baseline.cfg`](server_configs/cod2_13_browser_baseline.cfg) — conservative original CoD2 **LAN-only** configuration, scoped to isolated QA. Contains no IP addresses, passwords, original data files or unverified mods. It is **NOT currently activated** and has not been executed on a Windows game server.

What it sets:

- `dedicated=1`, `sv_maxclients=4` (not 16 until bot support is proven).
- `sv_fps=20`, `sv_maxRate=25000`, `sv_floodProtect=1`, `sv_timeout=120`, `sv_reconnectlimit=3`, `g_antilag=1`.
- `sv_pure=0`, `sv_allowDownload=0` **ONLY for local test** with user-owned assets; prepare stricter production policy later.
- `g_gametype=dm`, no automatic rotation, `mp_toujane` via command line.
- `g_logSync=0` + logs; avoid filesystem stalls from synchronous log writes.
- `scr_dm_timelimit` and `scr_dm_scorelimit` as **game script variables**, cross-check their effective values in actual 1.3 DM script IWD on the host before relying on them.

## How Antigravity should run a safe A/B validation on the REAL Windows PC

**A. Do not disturb the active browser build**. Preserve `work/native-server`, current `browser-test.cfg`, running 8081/8082 services, any uncommitted local code and existing login/session flow. Prefer a new *clean QA server root*, with read-only links to the user's legitimate local IWDs, isolated endpoints and a distinct data/log folder.

**B. Capture baseline before changing anything:** actual server executable SHA, complete launch argv, loaded game config, **effective cvar values queried via server console** (`sv_fps`, `sv_maxRate`, `sv_maxclients`, `g_antilag`, `sv_pure`, `sv_timeout`, `g_gametype`, `g_logSync`), UDP `getstatus` with map name and slot counts, browser relay handshake, snapshots and damage test results. Save sanitized logs and revision.

**C. In the isolated clean server root only**: place the new `cod2_13_browser_baseline.cfg` under local `main/`. Example **conceptual** launch, path and ports must be adjusted to a truly free isolated stack and should reuse the project's existing scripts instead of spawning conflicting services:

```text
cod2_win32_ded.exe +set dedicated 1 +set net_ip 127.0.0.1 +set net_port 28963 +set fs_basepath . +set fs_homepath . +exec cod2_13_browser_baseline.cfg +devmap mp_toujane
```

The server itself is native; the browser connects **through the existing project-owned relay and server-directory tickets**, not directly over arbitrary web WebSocket to CoD2 UDP and not via third-party master servers. Do not open 28963 on a public firewall. If the `+devmap` fixture changes expected cheat flags for tests, record it and compare like-for-like.

**D. Checks, each PASS/FAIL (never infer PASS from Cvar merely being present):**

1. Startup: correct game mode/map and 4 slots; `getstatus` parsed correctly; same web asset names and game protocol; no missing config, errors or port collision.
2. Compatibility: real browser can connect through relay, get screenshots of Toujane and stable snapshots, move, fire and reconnect.
3. Combat: same two-client native server test, confirm actual hit / HP decrement / death / respawn. Config cannot fix missing DObj/CM trace, so keep those failures separate.
4. Latency/bandwidth: measure update interval, server CPU and memory, packet loss, snapshot gaps, `sv_fps=20` vs carefully controlled alternative in *separate* runs if needed; do not change tickrate blindly.
5. Integrity/security: limit UDP to loopback, no RCON password, no outbound original masters, no automatic proprietary IWD downloads, no exposed debug logs containing keys.

**E. Integrate only if evidence supports improvement.** Store the resulting sanitized config and test report in `docs/evidence/` and comment on PR #5. Submit a small PR from this branch, do not merge to checkpoint until Antigravity verifies it with the new renderer + Work's runner.

## What config files cannot fix

- Missing visible remote player models due to DObj, snapshot corruption, XAnim errors.
- WebGL2 GPU renderer perf, shader/texture color bugs (Claude PR #4).
- Broken `CM_TraceBox`, `DObjTraceline`, collision or `G_Damage` (Antigravity/Jules).
- Real bot AI: external GSC mods like Bot Warfare depend on script callbacks and their supported native game host; not plug-and-play with a partially reconstructed WASM/C server. 1 human + 15 NORMAL bots requires functional engine/player/AI paths and actual load testing.

## Next advanced research after the baseline passes

1. Review CoD2x and CoD2rev server *behavior* and compare command responses, protocol 1.3 field encoding, flood timing and snapshot scheduling with our C sources. CoD2rev fork is AGPL-3.0: legal obligations and mixed dependencies need assessment before copying anything.
2. Investigate `sv_pure`, mod folders (`fs_game`), weapon allowances and gametype-specific Cvars using actual locally owned `.iwd` GSC scripts; don't enable unsupported mods without confirming their dependencies.
3. Reuse **our own server directory** (already exists) and familiar standard `getstatus` replies to monitor multiple servers instead of relying on abandoned historical master servers.
4. Later add true server-side bots and test 1+1 -> 1+4 -> 1+8 -> 1+15 with high-quality model/weapon rendering. Don't let tweaking configuration replace actual gameplay bug fixes.
