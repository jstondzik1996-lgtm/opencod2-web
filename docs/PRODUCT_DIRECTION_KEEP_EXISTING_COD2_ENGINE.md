# DECISION 2026-10-26 — KEEP EXISTING COD2 ENGINE; RESTORE PLAYABLE BROWSER GAME

**Owner instruction overrides the previous PlayCanvas/Colyseus/Rapier rewrite experiment.**

## Authoritative direction
- **DO NOT replace or rewrite the existing browser CoD2 engine.** No new engine implementation, no move to PlayCanvas/Colyseus/Rapier, no generic placeholder FPS.
- **USE existing `codex/browser-mp-checkpoint` implementation**: current C reconstruction, Emscripten WASM client, reconstructed Win32 dedicated server, existing local original CoD2 asset loading, existing launcher/relay, tests and worklogs.
- The `new_engine` directory on Jules's PR #2 is an abandoned test prototype, **not the product**. Its draft PR is not to be merged. Preserve its history as a reference; do not introduce or integrate it into the existing engine without a new explicit user instruction.
- User wants to **play CoD2 in their own browser NOW**. Deliver the best available true CoD2 browser build first; then fix its runtime failures.
- The actual game was previously visible at `http://127.0.0.1:8081/cod2.html` on the **original Windows computer** with local original game assets. Other computers' `127.0.0.1` point to themselves and do not host that build. Do not claim that 8081 is available unless verified on the actual machine.
- Do not delete or overwrite installed CoD2, original IWDs, credentials, user browsers, running previews or other agents' pending changes.

## P0 — actual live Windows gameplay, measured not asserted
1. At the original Windows workstation with `work/` and the user's licensed original CoD2 files, see `HANDOFF.md`, `docs/WORK_LOG.md` and `tools/RUN_UPDATED_LOCAL_TEST.bat`.
2. Check whether the existing 8081 preview is still up; **open it to verify original CoD2 map/gameplay**. If not, use existing safe separate QA launcher, do not launch the PlayCanvas `8090` mockup. The runner needs its original `work` sibling dirs (`emsdk`, `w64devkit`, `native-server/main`, `mp-assets/main`); lacking these on a different laptop is not a bug in the engine.
3. Rebuild the ORIGINAL WASM client and original C Win32 dedicated server on a separate port to keep last working previews. Verify exactly what is served (map, assets, login, FPS, local player movement/shooting) with real screenshot and console/server log. Collect performance profile/frame time on actual GPU.
4. Fix issues in impact order, retaining existing game: P0 crash/stalls, renderer purple assets, missing player DObj, visible players, bullet damage/HP/death/respawn and reliable server/reconnect; then ladders, weapon handling, bots.
5. Previous fixes are in code; don't pretend all are verified in game: `CM_TempBoxModel` and CM world indexing, `takedamage` in `ClientEndFrame`, `BG_UpdatePlayerDObj` diagnostics, experimental ladder baseline, hot path WebGL allocation work, safer non-SAFE_HEAP QA build. Validate actual effects, identify regressions, and patch specifically.
6. Working gameplay beats planning, unrelated infrastructure and new placeholder prototypes. No success claim based solely on CI builds, stubbed lobby tests or headless blank screenshot.

## Bot goal (after live game is stable)
User prefers **1 human browser player + 15 server-controlled bots at difficulty NORMAL**, not requiring two humans. This is a product objective **not an already implemented feature**. Investigate integrating CoD2 Bot Warfare as behavioral reference and implementing server-controlled player-equivalent actors consistent with actual engine and licenses; acceptance stages 1+1 -> 1+4 -> 1+8 -> 1+15 with real graphics/combat/FPS tests. Do NOT rewrite engine to make bots.

## Agent division and coordination
- **Only one primary local agent (Antigravity/Work on original Windows PC)** can perform authoritative full end-to-end runtime tests with the user's legally acquired CoD2 assets. It must not claim access from a cloud sandbox.
- Jules: scoped C-engine code debugging/regression tests only; no greenfield rebuild.
- Claude/Work: independent code review, original-port optimizations, test harness when capacity permits; isolate concurrent changes in feature branches/worktrees, share verified commits via PRs targeting `codex/browser-mp-checkpoint`.
- All agents: read this document before coding, report in Polish, do not restart the project, no destructive git reset/force-push, no sharing proprietary CoD2 assets or keys.
- Preserve the legal boundary: local private testing with user-provided legitimate game files is distinct from permission to publicly distribute Activision's game engine and content. Do not promise public release rights merely by changing the appearance/branding.

## Acceptance for the NEXT milestone
A real screenshot/video of the original CoD2 map running in browser from this code, movement and shooting confirmed, the exact URL and original engine commit recorded, honest observed FPS and current bug list. Follow with concrete verified repairs; never substitute a blue sky mockup.
