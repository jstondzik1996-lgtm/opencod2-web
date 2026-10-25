# Work log

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

See `HANDOFF.md` for exact endpoints, build steps, limitations and next work. This checkpoint does not establish working combat.
