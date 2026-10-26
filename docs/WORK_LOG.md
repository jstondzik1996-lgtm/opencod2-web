# Work log

## 2026-10-26 — Codex: automatyczne testowanie prawdziwej gry (osobna gałąź)

- Zakres: wyłącznie infrastruktura testów istniejącego portu C/Emscripten. Gałąź `codex/automated-game-tests`, PR do `codex/browser-mp-checkpoint`, zgodnie z bieżącym poleceniem właściciela. Nie zmieniano plików renderera ani obrażeń.
- Sprawdzono aktualne `tools/RUN_UPDATED_LOCAL_TEST.bat`, `tools/browser_multiplayer_check.py`, HANDOFF i WORK_LOG z bazy `7a6b5f72c6d77c7da5aa10ae6ae04673fd718cc4`. Lokalny wcześniejszy checkout ma inną historię; zachowano go bez zmian. Kod i dokumenty do PR pobrano z dokładnej rewizji GitHub.
- Wcześniejszy BAT jedynie otwierał stronę. Teraz po osobnej kompilacji uruchamia `tools/run_browser_game_test.py`: własny serwer UDP, katalog, relay, HTTP i dwa klienty, nowe wolne porty, nowe profile bez CD-Key, sesje projektu, watchdog i sprzątanie wyłącznie utworzonych procesów. BAT można uruchomić z `tools/` albo wcześniejszego `work/`; obsługuje wybór gałęzi i tryb nieinteraktywny.
- Raport PL `raport.md/json`: PASS/FAIL/NIE_WYKONANO; logi usług, odpowiedzi UDP, kody wyjścia, screenshoty etapów i przy wyjątku. Brak uruchomienia etapu nie daje PASS. Błędy JS/WASM/WebGL, logowane braki modeli/materiałów/DObj i błędy sieci trafiają do osobnych kontroli.
- Pomiar płynności: wyłącznie takty rAF z rysowaniem WebGL, średnia, p95 i maksymalna przerwa; nie jest to pomiar czasu GPU ani dowód wizualnej poprawności całej mapy. Synchronizacja: postęp i cofanie migawek, zastoje, skoki, odchylenie zegara i zgodność client/psClient. Zachowano prawdziwy scenariusz strzałów, śmierci, respawnu i reconnect; dodano potwierdzenie faktycznego disconnect.
- Regresje lokalne: **33 testy PASS** dla infrastruktury i anonimizacji; obejmują także wykonanie skryptu próbnika w Node.js na pozorowanym kontekście (nie prawdziwa gra). Nowy workflow powtarza je na Windows/Linux i przechowuje raport kontrolowanego błędu brakujących plików.
- Dostępne lokalne pliki starszego buildu `f0dc2fea5c` przeszły kontrolę nagłówków WASM/MZ, manifestu i indeksów ZIP: 5 plików, 2 633 150 094 bajtów, 1533 wpisy modeli i 4369 materiałów. Nie wykonywano pełnego CRC ani nowej kompilacji silnika w tej sesji.
- Pierwszy start ujawnił rzeczywistą wadę fixture: starszy serwer nie odnalazł luźnego `qa-test.cfg` w osobnym home. Nadzorca przekazuje teraz ustawienia i `+devmap mp_toujane` jako argumenty; log kolejnego startu doszedł do `Scr_StartupGameType: done`. Dodano regresję tego sposobu uruchomienia.
- **Pełny test gry NIEZALICZONY / NIEPOTWIERDZONY**: kontrola gotowości usług kończy się FAIL w ograniczonym środowisku wykonawczym; dwóch klientów i pomiaru FPS nie wykonano. Wcześniej próba połączenia z istniejącym localhost zwróciła Windows 10013, a bezpośredni GitHub/DNS był niedostępny. To nie stanowi dowodu awarii silnika; przyczyny każdego zatrzymania zapisuje raport i logi. Nie tworzono zastępczych screenshotów ani nie oznaczano walki jako PASS.
- Instrukcja i ograniczenia: `docs/AUTOMATYCZNE_TESTY_GRY.md`. Następny krok: lokalny agent uruchamia ten sam runner poza blokującym środowiskiem, na świeżo zbudowanej rewizji i rzeczywistych zasobach; analizuje raport zamiast uznawać samą kompilację za sukces.


## 2026-10-25 — ChatGPT: recorded gameplay performance review + first WASM/WebGL hotfixes

- Analysed the user's 88-second Chromium/Brave screen recording (1920x1002, recorded at 30 FPS). Real Toujane gameplay appears around 50–88s. The game's own counter visibly varies from about 20 FPS to 41 FPS; a frame-difference estimate from the recording shows approximately 8–11 visibly distinct frames/sec during moving gameplay (recording-rate/content artefacts possible; **not** a reliable GPU benchmark). There are visibly purple-tinted surfaces on the halftrack/boxes/foliage and stutter; user also reports invisible second player, ladder/weapon bugs.
- Confirmed performance regression by source inspection: `tools/RUN_UPDATED_LOCAL_TEST.bat` forced `-DCOD2_WEB_SAFE_HEAP=ON` for every gameplay QA build, which enables Emscripten SAFE_HEAP=2 + STACK_OVERFLOW_CHECK=2 in CMake. Changed BAT default to performance mode `SAFE_HEAP=OFF` (commit `70e2f61`), preserving a separate opt-in diagnostic build via `COD2_QA_MEMCHECK=1`; QA web directories are suffixed -play/-memcheck to avoid CMake cache and build collisions. This is a suspected major FPS contributor, not yet measured on user's Windows GPU.
- `src/web/webgl2_compat.c`: real hot path used to malloc/free and convert a full vertex array for EACH legacy glDrawElements/glDrawArrays. Added one growable WASM-side scratch vertex buffer reused between draws, preserving existing buffer-copy/upload and draw semantics (commit `6fde958`). This removes CPU allocation churn, but actual FPS gain remains **unmeasured**.
- Browser asset colours and absent player models are **not fixed** in this change; rendering pipeline/lightmap and client DObj must be traced using genuine runtime logs, not guessed. Previous critical "GUARD CORRUPTED" log from the browser means retain memory-check mode for focused diagnosis if crashes recur. Disabling SAFE_HEAP in play builds is a performance test, not proof of memory safety.
- GitHub Actions run 40244453951 completed successfully (Python, real 32-bit world/DObj harness, Chromium stubbed lobby, MinGW dedicated-server build, Emscripten WASM compile); this DOES NOT test GPU FPS or visual correctness on original assets.
- Run the revised BAT from the same `work` directory; it downloads latest shared branch and launches on a free QA port. Record the exact URL and FPS comparing the same map/location with the older SAFE_HEAP build; if an out-of-bounds/guard warning appears use opt-in memcheck. Do not disturb 8081 older preview.


## 2026-10-25 — ChatGPT: own-server guest admission fixes after invalid CD-Key screenshot

- User's freshly built QA page at 127.0.0.1:8082 loaded the 0/4 local server directory but initialization failed with `EXE_ERR_INVALID_CD_KEY`. The error is generated by `CL_RequestAuthorization` in `cl_main_mp.c`: it validates the original CD key when the original authentication route is reached. This browser should instead be using the project's LAN-mode own server, with signed guest/session admission at the project relay.
- Fixed `src/web/lobby.js` (commit `2b8cc0f`) so a target selected through the project directory starts with `+set net_lanauthorize 0` BEFORE `+connect`. Native network source `win_net.c:Sys_IsLANAddress` recognizes 127.0.0.0/8 as LAN; `CL_CheckForResend` and `CL_RunDeferredPostGamestate` therefore avoid the obsolete original CD-key authorization route for our local project sessions. The change is scoped to the launcher for our directory servers, not external servers.
- Fixed `src/web/web_lobby.c` (commit `83bcb18`) to execute `set net_lanauthorize 0` before `connect` on **subsequent** joins/full-server fallback once the original boot args are no longer available. This is not a random CD-key or a bypass of third-party server authentication; our relay requires our own session/join ticket.
- Added regression assertion to Chromium launcher test `tools/test_lobby_browser.py` (commit `b086dca`) for exact initial +set/+connect order and absence of injected CD key.
- Added native-C real bridge command-validation test `tools/test_web_lobby_bridge_c.py` (commits `961acca`, `87bb21f`): proves local LAN setting is issued before connect and malicious addresses are rejected. Verified actual compilation/execution in GitHub Actions; Python job ran **48 tests** and reported no failures.
- Updated `tools/RUN_UPDATED_LOCAL_TEST.bat` (commit `1374eee`): QA builds are revision-specific and isolated from older running HTTP servers; dynamically selects free QA HTTP/master/relay/UDP ports (starts at 8082, moves higher if occupied). No local previews are intentionally stopped.
- GitHub Actions run `40243423603`: **PASS 5/5** (Python including native C lobby test, Chromium stubbed lobby, i386 collision/DObj harness, MinGW dedicated server, Emscripten browser WASM compilation). **Still unverified on the user's Windows machine**: that a fresh browser session joins the owned server without a CoD2 key, real two-player damage/death, engine crash guard messages. Do not confuse green builds with live multiplayer success.
- For Windows user: download the updated BAT from the above commit into existing `work`, rerun and open the exact URL it prints (may be 8083+ when 8082 is occupied). If `EXE_ERR_INVALID_CD_KEY` persists, capture the new browser log and whether lobby `+set net_lanauthorize 0` appears, then trace why the flag is reset; do not fabricate/assign random CD-keys.

## 2026-10-25 — ChatGPT: fix Windows QA script after second real user screenshot

- User re-ran the QA BAT and MinGit download successfully completed (git version 2.56.0.windows.3), so first problem was resolved. New screenshot explicitly shows ERROR: CMake is missing, followed by merge: origin/codex/browser-mp-checkpoint - not something we can merge; that merge error does not prove branch divergence, it means the named ref wasn't available.
- Fixed the EXISTING shared-repo BAT in commit 093d82a: it bootstraps portable CMake v4.4.4 (54.4 MB) and Ninja v1.13.2, with SHA-256 verification against GitHub release metadata, and reuses the MinGit already downloaded. Does not install system-wide or require admin.
- Git workflow now fetches the exact shared-branch head directly from jstondzik1996-lgtm/opencod2-web.git, uses FETCH_HEAD's validated SHA instead of relying on origin/<branch> tracking refs, fast-forwards the original checkout ONLY if clean and ancestor, and otherwise creates/updates a separate QA worktree of the SAME existing Git repository (not a clone or new product). Preserves both uncommitted and divergent local Work/Claude work. Never resets/force pushes.
- Scripts build under work/qa-build-web and work/qa-build-win32; QA ports remain HTTP 8082 / master 20723 / relay 20724 / UDP 28963. Previous 8080/8081 preview stays untouched.
- The BAT was verified by static inspection but has not been run on the user's Windows system after this fix; if it fails, request the FIRST error line/screenshot instead of assuming that builds and gameplay work.

## 2026-10-25 — ChatGPT: Windows QA runner Git/PATH fix from user screenshot

- User executed `work/RUN_UPDATED_LOCAL_TEST.bat` in PowerShell. Actual output showed **`ERROR: git.exe missing`**, then a misleading empty current branch check. No build/gameplay update took place.
- Confirmed root cause: the original runner expected Git in PATH and the compound `where git ... || (echo ... & goto :fail)` did not stop before later branch code. This is a **runner setup failure**, not a result about missing player models/ladder gameplay.
- Updated **the same repo/file** `tools/RUN_UPDATED_LOCAL_TEST.bat` in commit `447a3cc`. It now searches common local Git paths; otherwise uses the official Git for Windows **MinGit v2.56.0(3)** zip (39.85 MB) downloaded to a portable `work/portable-git` directory, checks fixed SHA-256 `2e5619966e588fde5f8122c5557cad50c0fbafcaed736689f40ec1dfb1d78ea7`, and never installs Git system-wide. Missing tooling returns a hard error. Detects CMake/Ninja in common paths, uses fast-forward-only update of the same existing checkout, and still targets isolated QA ports 8082 / 28963.
- **Not yet executed on the user's Windows host after fix**. Download the new BAT from its commit, replacing the old one in `work/`, and retry; capture the first ERROR if it still fails.
- No original CoD2 files or user's live 8081 preview were modified from this cloud session.


## 2026-10-25 — ChatGPT: local QA launcher, ladder baseline and remote-player diagnostics

- User reports locally working `http://127.0.0.1:8081/cod2.html` but major gameplay defects: stalls, missing assets, invisible second player, weapon glitches, broken ladders. The browser machine is **NOT directly accessible** to this ChatGPT session, so these visual/runtime issues are reported by the user; not locally reproduced here.
- Created `tools/RUN_UPDATED_LOCAL_TEST.bat` (commit `7c4d524`) for the user's already existing `work` folder. It verifies expected local repo/SDK/compiler/assets, refuses dirty tracked files or wrong branch, fast-forward pulls the shared branch, builds WASM and Win32 in **separate** `build/qa-web` / `build-win32-qa`, junctions to the already installed asset pack and runs a **separate QA stack** at 8082 / 20723 / 20724 / 28963 without intentionally killing previews on 8080/8081. The batch file is **UNTESTED on Windows** and must not be presented as validated one-click deployment; its initial execution may require local adjustments.
- Verified source-level bug: `PM_LadderMove` previously called `PM_AirMove` outright, which discards requested vertical ladder motion and applies air gravity. Replaced with a **conservative experimental** ladder-only climbing baseline with gravity off, W/S vertical and A/D tangent movement (commit `a7b2d33`). This is **not proven** to fix ladders in CoD2 maps; ladder-state detection/pm_flags and exact original feel still need browser/native gameplay tests.
- `CG_Player` silently returned when `Com_GetClientDObj` was null, leaving players invisible with no diagnostic. Added rate-capped `[cg-player] missing DObj` logging of entity/client numbers (commit `965d9d9`). **Diagnostic only**; the actual reason the DObj is missing, or any shader/visibility issue, must be established from local logs before fixing.
- CI after experimental ladder baseline and missing-DObj instrumentation: GitHub Actions run 40234580261 **PASS 5/5** (Python regressions, Chromium stub lobby, native i386 harness, WASM compile, MinGW dedicated-server compile); this is still **NOT** a live gameplay verification. The QA BAT was not executed on Windows by this assistant.
- Next real work: in fresh QA build confirm player sprites/model, capture `[cg-player]`/animation registration errors, frame timings/WebGL memory and packet loss; isolate movement bugs vs relay timing; inspect actual `PMF_LADDER` detection and original ladder gameplay; validate true two-player damage/death. CI compilation/tests are not equivalent to runtime.
- No proprietary assets, CD keys or Windows paths were committed; no local processes were touched from this chat.


## 2026-10-25 — ChatGPT: keyless / guest-session P0 test mode after local preview started

- User confirmed their local browser preview works at `http://127.0.0.1:8081/cod2.html`. This **cannot be remotely inspected** from ChatGPT: 127.0.0.1 points at the user's own machine. The last documented Windows two-client test on e49bf6d **failed damage/death/respawn**, and `CM_TraceBox` has since been fixed in 19253db; **a new real run is still required**.
- Existing Windows-only `tools/browser_multiplayer_check.py` always read the installed CD-Key from the registry. Added optional `COD2_TEST_NO_KEY=1`: skip reading registry and avoid inserting any CD-Key into browser preferences. It uses a fresh temporary browser profile so no previous credential can mask a broken keyless path (commit `33d451c`).
- Optional `COD2_TEST_SESSION=1` **requires** `COD2_TEST_NO_KEY=1` and a configured project directory + `--sessions` backend. Each browser client now receives its own randomly generated guest identity, project session and server-bound `/join` ticket. Ticket and session are forwarded only in browser memory to the WS relay, not written to logs or JSON. Reconnect uses the session. This is a *test feature* for the project's own private LAN-mode instances; it does not grant access to third-party servers (commit `24b539f`).
- CI now syntax-checks the Windows integration harness independently of the user's machine (`0fb7722`), in addition to Python, Chromium lobby, 32-bit collision and WASM/MinGW compile jobs. Added `tools/browser_mp_redact.py` and `tools/test_browser_mp_redact.py` to strip URL admission tokens (`ticket`, `session`, `access`, `token`) from browser WebSocket/log evidence (commits `441720f`, `1e70299`). `tools/test_browser_mp_redact.py` passed in GitHub Actions Python job of run 40233556380; other CI jobs were still in progress at last check. **Automated CI does not run two real game clients**. Wait for real local Windows P0 run before claiming damage/keyless admission PASS.
- No modifications to the user's running 8081 server, game archives, CD-Key or local browser profile; all code changes were made directly on the shared `codex/browser-mp-checkpoint` GitHub branch.


## 2026-10-25 — ChatGPT: stale-server guard and asset-free CI on shared branch

- Continued the existing Work/Claude project directly through GitHub; **no new repository/fork** and no modifications to local Windows preview.
- `tools/master_proxy/directory.py`: reads of server list, in-game masterlist and Quick Play now reject servers whose last successful UDP status heartbeat has exceeded `stale_after`, even if the poller thread has stalled. Former implementation only recomputed `online` during `poll_once`, potentially advertising a dead instance indefinitely.
- `tools/test_server_directory.py`: regression for stopped poller, stale server rejection and recovery after fresh poll.
- `.github/workflows/browser-mp-regressions.yml`: asset-free Python suite and 32-bit native `cm_world.c` sector-list test automatically run for relevant commits. This is distinct from the full multiplayer two-client combat test.
- GitHub commits: `dfd13c9` (directory), `3e2593c` (test), `38fec08` (CI).
- First CI run found a legitimate test assumption conflict: reservation expiry was checked after 31s with a server heartbeat older than 10s. Corrected the test to refresh the heartbeat before asserting reservation re-use (`11f2e3f`). Asset-free Python and the real i386 `cm_world.c` harness **PASS** in GitHub Actions run 40225032629. Added real headless Chromium + Playwright smoke coverage (`f1ca222`); all three jobs **PASS** in run 40225113259 (Chromium tests run with a stub game engine, not a real multiplayer match). Added an Emscripten 3.1.74 WASM compile job (`5638351`). GitHub Actions run 40225295436 **PASSED ALL FOUR JOBS**: asset-free Python regressions, real headless Chromium lobby/Quick Play tests with a stub game engine, 32-bit x86 `cm_world.c` harness and complete Emscripten test-export browser-client compilation plus required artifact checks (`cod2.html`, `cod2.js`, `cod2.wasm`, `lobby.js`). The WASM compile is not proof that two-client combat works. Local two-client combat/damage integration remains **NOT VERIFIED** because no access to the local CoD2 installation / native test server is available in this chat.
- Next step after CI: inspect job results, repair any regression if the jobs fail. When local access returns, perform P0 on isolated ports as in HANDOFF, then P1 real two-server lobby test.

## 2026-10-25 — Codex: shared collaboration instruction

The user explicitly wants Work, Codex and Claude to implement together on the same project and shared `codex/browser-mp-checkpoint` branch. The earlier review-only instruction for ChatGPT is superseded. No assistant-specific branches are requested. Sync incoming changes, preserve each other's edits, and record stages/results here. Current technical next step remains the paired collision contents/entity-index investigation described in HANDOFF.md; this documentation change does not claim a combat fix.

## 2026-10-25 — Initial Windows/browser checkpoint

Upstream baseline: `c301dd6e8550c59a3fdd3081c73dc4ffd2c8602c`.

- Verified official local assets and configured portable Emscripten, x86 MinGW, CMake/Ninja and Playwright.
- Fixed Windows long linker command using a response file; restored missing bg_pmoveDebug definition and dedicated-server GL stubs.
- Fixed browser HTTP keepalive following HEAD, archive patch precedence and shared asset filtering.
- Restored browser preference CD-key loading and several missing WASM data relocations. Added safe optional client-state/aim exports for integration tests.
- Fixed wrong runtime cookie inspection in WASM and error reporting before renderer cleanup.
- Fixed two active snapshot paths using an incorrect client structure stride; fixed script paused-stack lookup/removal during disconnect.
- Two browser clients rendered Toujane, spawned, moved and fired on the local reconstructed dedicated server. Damage test failed.
- Fixed player affine transform storage/translation; damage still failed.
- Temporary hull contents experiment exposed a sector entity-list infinite loop and was reverted. Identified suspected zero-based/sentinel mismatch as the next paired fix. Preview server restored to the earlier stable build.
- Created user's fork at https://github.com/jstondzik1996-lgtm/opencod2-web for durable checkpoints and Claude handoff.

## 2026-10-25 — Claude: paired collision-list fix (integration pending)

- `cm_world.c`: world-sector entity list writers now store entity number + 1 like all readers (fixes lost entity 0 and the infinite loop in `CM_UnlinkEntity` that hung the earlier experiment).
- `cm_trace.c`: `CM_TempBoxModel` sets `leaf.brushContents`, so player hulls are linked and not skipped by the leaf contents test.
- Added `tests/cm_world_link/` (real `cm_world.c`, 32-bit, stubs). Old code: FAIL; new code: OK. Python tests OK.
- Not run: Windows/Emscripten builds and two-client damage. Steps for the local agent are in `HANDOFF.md`.

## 2026-10-25 — Claude: own server directory, Quick Play, lobby (P1 code; real-stack test pending)

- Stage: P0 waits for Work's real two-client run; P1 implemented in parallel.
- Files: `tools/master_proxy/{directory.py,server.py,servers.example.json}`, `tools/serve_web_local.py`, `tools/game_proxy/relay.py`, `src/web/{lobby.js,web_lobby.c,shell_nzp.html,brand-mark.png,favicon*.png,apple-touch-icon.png}`, `CMakeLists.txt`, `tools/browser_multiplayer_check.py`, new tests `tools/test_server_directory.py`, `tools/test_lobby_browser.py`.
- Harness: added death, respawn, disconnect (server keeps sending snapshots) and reconnect checks; no health is set by the test.
- Results (cloud): 18 Python tests OK, including headless Chromium lobby test with stubbed engine; `Web_LobbyConnect` address parser tested natively. Not built with Emscripten/MinGW here (no SDK/network). Not run against a real server.
- Next: Work runs P0 with the extended harness, then the P1 steps in HANDOFF. Claude continues with P2 design (own player identity for project servers) and track B asset separation plan.

## 2026-10-25 — Claude: P0 hit-path audit, takedamage fix (combat integration still pending)

- Audited the whole bullet path against an independent CoD2 server reconstruction (CoD2rev_Server, AGPL; used only as a behavioural reference, no code copied): `Bullet_Fire_Extended`, `SV_Trace`, `CM_PointTraceToEntities`, `SV_PointTraceToEntity`, `DObjTraceline`, `G_Damage`, `Scr_PlayerDamage`, `PlayerCmd_finishPlayerDamage`, `G_SetClientContents`, `GScr_LoadGameTypeScript`.
- **Real bug (blocks all player damage):** `ClientEndFrame` never set `ent->takedamage = 1` for a playing client (only corpses/triggers ever got 1), so `Bullet_Fire_Extended` and `G_Damage` rejected every hit even after the collision fix. Restored, plus `takedamage = 0` while dead.
- Same function: wrote `sess.viewmodelIndex` into `ps.damageCount` instead of `ps.viewmodelIndex` (view model + damage feedback), and chose PM_DEAD/PM_DEAD_LINKED from `ent->count` instead of `ent->tagInfo`. Both fixed.
- Collision: the original keeps the temp box hull's `leaf.brushContents = -1` from load time and `CM_TempBoxModel` only sets the brush contents. Moved my earlier fix there (`cm_load_obj.c`), removed the per-call leaf write. `tests/cm_world_link` updated to the -1 hull; still OK, old cm_world.c still FAILs.
- Checked and left as in the original: `finishPlayerDamage` reads the inflictor from argument 1 (same quirk in the reference).
- New `tests/dobj_trace/` runs the REAL `dobj.c` DObjTraceline on a synthetic skeleton (12 checks: fraction, normal, misses, rotation, world limit, priorities, inheritance, ignoreCollision, start-solid). Mutation-checked: three deliberate bugs are each caught.
- CI: hitbox test added; new MinGW i686 dedicated-server compile job; trigger paths now cover src/ and CMakeLists.
- Pending (needs local Windows + game files): real two-client run of `tools/browser_multiplayer_check.py`. Expected now: `damage_to_other_player`, `target_died`, `target_respawned` true.

## 2026-10-25 — Claude: P2 player sessions and join tickets (code + tests; real-stack check pending)

- Finding: on project servers reached through the relay both client (`CL_CheckForResend`) and server (`SV_GetChallenge`) take the original LAN path with `net_lanauthorize 0`, so no CD key or external authorization is used. Design documented in HANDOFF ("Player sessions").
- Files: `tools/master_proxy/{sessions.py,server.py,directory.py}`, `tools/game_proxy/relay.py`, `tools/serve_web_local.py`, `src/web/{lobby.js,web_net.js}`, tests `tools/test_sessions.py`, `tools/test_lobby_browser.py`.
- Bug caught by the new browser test and fixed: nick input maxlength cut the raw text before colour codes were removed.
- Results: 29 Python tests OK locally. CI run for 07f4215: WASM build, Chromium lobby, Python and i386 harness jobs passed (MinGW job pending at the time of writing).
- Next: P1 multi-instance helper and concurrency tests; P3 performance review.

## 2026-10-25 — Claude: P1 server fleet, P3 relay hardening

- `tools/run_server_fleet.py`: starts N dedicated-server instances from one game root on consecutive ports (own fs_homepath each, LAN mode, loopback/private bind enforced), writes the directory JSON for the master proxy, restarts crashed instances with backoff.
- Relay (P3): one bounded drop-oldest queue and one writer task per session instead of a task per datagram (no unbounded memory with a slow browser), upstream packet-rate limit (`COD2_RELAY_UP_PPS`, default 250/s), per-address session limit (`COD2_RELAY_MAX_PER_IP`, default 4; X-Forwarded-For trusted only from a loopback peer; serve_web_local and the nginx snippet now send it).
- Tests: `tools/test_server_fleet.py` (fake dedicated server process: directory file, both instances online, crash -> restart -> online, argument guards; 20 parallel HTTP Quick Play requests never exceed 7 free slots), `tools/test_relay_session.py` (real relay_handler with fake WebSocket + UDP echo: order, slow-browser drops, rate limit, per-IP limit, header trust). 41 Python tests OK.
- CI: MinGW i686 server job FAILED on run 40227512003 (compile step). Full log is not reachable from the cloud session (blob storage blocked), so the step now emits the first error lines as annotations. Investigating next; may be a toolchain difference (Ubuntu MinGW GCC 13 vs w64devkit GCC 16 used locally).

## 2026-10-25 — Claude: stage 2 delivery pieces, CI MinGW fix

- CI MinGW failure root cause (from annotations): Ubuntu's MinGW GCC 13 rejects `-Wno-error=return-mismatch` (GCC 14+ flag). Not a code error. CMake now adds each `-Wno-error=` flag only if the compiler supports it (checked locally with GCC 13 and clang).
- Stage 2 (play without a local install), see `docs/STAGE2_DELIVERY.md`:
  - `serve_web_local.py --access-key`: assets.json / main/ / zone/ / archives need a cookie obtained once via `?access=<key>`; launcher stays public; warning when bound publicly without a key.
  - Manifest per-file `version`; `cod2_fs.js` skips a cached archive only if size AND version match (a replaced same-size archive is fetched again).
  - `tools/build_overlay_pack.py`: zz_ overlay IWD to replace individual files with licensed equivalents, with an override report (bug found by the test and fixed: the overlay indexed itself when written into the base folder).
- Tests: `tools/test_stage2_delivery.py` (access key, overlay report, real cod2_fs.js in Chromium/OPFS: unchanged archives cached, replaced one re-downloaded). 45 Python tests OK.

## 2026-10-25 — Claude: CI 5/5 green incl. Windows server; deploy fixes; field-level audit

- CI run 40228343453 (a854e80): all five jobs passed — Python (45 tests), Chromium lobby, i386 cm_world + dobj harnesses, Emscripten WASM client, and now the MinGW i686 dedicated server build (with today's takedamage, hull contents and entity-list fixes). Compiled only; not run.
- Field-level comparison of every same-named function in game_mp/server_mp against the behavioural reference: no further missing assignments on the combat path (remaining differences are style: macros, element-wise copies, aliases). Notes: our `player_die` also returns for noclip/ufo (`pm_type > 1`) where the reference returns only when dead; death also requires a server DObj for the player (`Com_GetServerDObj`), to watch in the real run.
- Deploy: relay now finds `sessions.py` next to itself (VPS layout) or in tools/master_proxy (repo); `deploy_web_to_vps.sh` copies `sessions.py`, `lobby.js`, new icons; remote game assets are now OFF unless `PUBLISH_REMOTE_ASSETS=1` (the old script published `/play/assets.json` to everyone). Master proxy README lists the three files it needs.
- Still pending, needs the local Windows stack: `tools/browser_multiplayer_check.py` (damage, death, respawn, disconnect, reconnect), then fleet + lobby + sessions on real servers, then load tests.

## 2026-10-25 — Claude: first real Windows two-client run on the new code; CM_TraceBox fix

Run on the user's PC (isolated: server 127.0.0.1:28961, web 8081/20721/20722; preview 8080 untouched) with commit e49bf6d, via `work/CLAUDE_RUN_TEST.bat` (downloads the branch to `work/opencod2-claude`, builds, runs `tools/browser_multiplayer_check.py` with installed Chrome):
- PASS: two distinct clients, spawn, movement, firing, server alive after disconnect (snapshots keep advancing), reconnect, server alive after reconnect. The old unlink hang is gone.
- FAIL: damage_to_other_player, target_died, target_respawned. Server log: the target is now a bullet candidate (`entity=1 contents=2000000`), aim is exact (target ~85 units, world hit at ~165 units behind him), but `bullet model` never printed: the hitbox test was never reached.
- Cause: `CM_TraceBox` (cm_tracebox.c) was mis-reconstructed: same sign for the mins and maxs passes, inverted entry comparison, and it returned "skip" whenever fraction != 1, i.e. whenever any world surface had been hit behind the entity. Rewritten to the original slab test. New `tests/trace_box` (real file, i386): old code fails the recorded game case and 5105/200000 random cases, new code passes all. Also affects static-model collision and sight traces (all callers use the same early-out).
- Windows also exposed the UDP connection-reset bug in the directory poller (fixed in e49bf6d).
- Next: rerun the same script on the PC.

See `HANDOFF.md` for exact endpoints, build steps, limitations and next work. This checkpoint does not establish working combat.
