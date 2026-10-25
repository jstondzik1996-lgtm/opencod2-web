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

See `HANDOFF.md` for exact endpoints, build steps, limitations and next work. This checkpoint does not establish working combat.
