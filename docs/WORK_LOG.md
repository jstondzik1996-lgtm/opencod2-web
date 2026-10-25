# Work log

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

See `HANDOFF.md` for exact endpoints, build steps, limitations and next work. This checkpoint does not establish working combat.
