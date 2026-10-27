# Jules CM_TraceBox handoff ZIP — review and safe selective integration (2026-10-27)

## Context / provenance

User supplied `jules_session_6659151480864656588.zip` from Jules after publishing failed. Archive contains 14 ZIP entries including 2 C files (`src/PC/qcommon/cm_tracebox.c`, `tests/trace_box/test_trace_box.c`), 4 docs and an earlier `tools/RUN_UPDATED_LOCAL_TEST.bat`.

**Do not import entire ZIP onto the active tree.** Its documentation and BAT come from an older checkpoint, and the currently integrated test runner, renderer and worklog have since changed. The ZIP includes no `new_engine` directory, but the historic Jules branch on GitHub does contain that abandoned PlayCanvas proof-of-concept; it must never be merged into the original port.

### What the ZIP changed

- Rewrote `CM_CalcTraceEntents` and `CM_TraceBox` as an alternative slab intersection algorithm.
- Replaced signed inverse deltas `1/(start-end)` with `1/(end-start)` to match this alternative implementation.
- Added one explicit test that a player bbox earlier than the current world collision fraction remains a possible hit.

## Comparison to the *existing* C/Emscripten engine

The checkout on `codex/browser-mp-checkpoint` **already includes an earlier repaired `CM_TraceBox`**, with the same documented real game case (player in front of a wall) and a 200,000-case randomized slab regression. Therefore Jules's proposed source replacement is NOT evidence of another previously absent fix.

Independent asset-free test (uploaded source functions compiled as native x86_64 `.so` with minimal stand-in type headers, checked through Python `ctypes`, NOT a real CoD2 full build):

- 199,946 non-boundary randomized segment/axis-aligned-box cases: Jules implementation matched the mathematical slab reference in **all** assessed cases (0 mismatches).
- 250,000 further randomized segment/box/fraction cases: existing source and Jules source returned the **same skip/possible-hit** decision in all sampled cases (0 differences).
- Four manually chosen fractions for a segment from x=0 to 1000 crossing bbox x=80–90 also produced identical expected results (0.165 -> could hit, 0.07 -> skip, 0.091 -> could hit, 0.075 -> skip).

These findings establish no improvement over existing behavior. They do NOT establish equivalence over all floating-point edge cases, other callers of `CM_CalcTraceEntents`, or live bullet damage.

## Integration decision

**Preserve the additional useful regression ONLY**: two explicit test cases in `tests/trace_box/test_trace_box.c` — victim before world hit, and world hit before victim.

Do **not** replace the original C implementation or revert sign conventions just to adopt Jules's style: unneeded gameplay risk, while the user's Windows agent is investigating hitbox/DObj/aim issues in live combat. Preserve the existing shader+automated-test integration PR #5 and local uncommitted Antigravity work.

Expected CI: the existing `tests/trace_box/run.sh` in the Browser MP regression workflow should execute the C test against the real existing source and show PASS; follow-up local Windows gameplay must demonstrate target HP decrease, death, respawn, reconnect with actual game files.

## Where actual progress is needed

Antigravity's Windows tests establish connection, spawning and shooting, but **hit registration remains unproven**. Investigate `SV_PointTraceToEntity`, server DObj model/bones and per-bone traces, player viewangle aim direction, `Bullet_Fire`, and `G_Damage` on the original code. Do not mark the bug fixed from an asset-free box hit test.

The goal remains the actual original CoD2 browser port, not a new PlayCanvas game. User-owned original CoD2 assets remain local-only.
