#include "common_types.h"
#include "imports.h"

void CM_CalcTraceEntents(TraceExtents *extents)
{
    int i;
    float delta;

    for (i = 0; i < 3; i++) {
        delta = extents->start[i] - extents->end[i];
        if (delta == 0.0f)
            extents->invDelta[i] = 0.0f;
        else
            extents->invDelta[i] = 1.0f / delta;
    }
}

/*
 * Returns nonzero when the segment start->end does NOT enter the box before
 * `fraction` (so callers skip the entity), zero when it may hit it.
 *
 * Slab test over both faces of each axis: pass 0 uses mins with sign -1,
 * pass 1 uses maxs with sign +1 (t = signed distance outside the face).
 * The earlier reconstruction used the same sign for both passes, mixed up
 * the entry comparison and returned "skip" whenever fraction != 1, i.e.
 * whenever the world was already hit behind the entity - which is almost
 * every bullet, so no player was ever tested against its hitboxes.
 */
qboolean CM_TraceBox(const TraceExtents *extents, const vec_t *mins, const vec_t *maxs, float fraction)
{
    const vec_t *bounds = mins;
    float sign = -1.0f;
    float enter = 0.0f;
    float t1, t2, f;
    int i, pass;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < 3; i++) {
            t1 = (extents->start[i] - bounds[i]) * sign;
            t2 = (extents->end[i] - bounds[i]) * sign;

            if (t1 > 0.0f) {
                /* start outside this face */
                if (t2 > 0.0f)
                    return 1;   /* both outside: misses */
                f = t1 * extents->invDelta[i] * sign;
                if (f >= fraction)
                    return 1;   /* enters only after the current hit */
                if (f > enter)
                    enter = f;
            } else if (t2 > 0.0f) {
                /* start inside, leaves through this face */
                f = t1 * extents->invDelta[i] * sign;
                if (enter >= f)
                    return 1;   /* leaves before it enters */
                if (f < fraction)
                    fraction = f;
            }
        }
        bounds = maxs;
        sign = 1.0f;
    }

    return 0;
}
