/*
 * Host regression test for CM_TraceBox / CM_CalcTraceEntents
 * (src/PC/qcommon/cm_tracebox.c), compiled as 32-bit x86 like the server.
 *
 * CM_TraceBox is the early-out before per-bone hit detection: it returns
 * nonzero ("skip this entity") when the bullet segment does not enter the
 * entity box before the current trace fraction. Compared against an exact
 * slab intersection for random segments/boxes, plus the real case from the
 * first Windows two-client run: player 85 units ahead, wall behind him.
 */
#include "common_types.h"

void CM_CalcTraceEntents(TraceExtents *extents);
qboolean CM_TraceBox(const TraceExtents *extents, const vec_t *mins, const vec_t *maxs, float fraction);

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static void out(const char *s) { int n = 0; while (s[n]) ++n; sys3(4, 1, (int)s, n); }
static void out_int(int v)
{
    char buf[16]; int i = 15; unsigned u = v < 0 ? -v : v;
    buf[i] = 0; do { buf[--i] = '0' + u % 10; u /= 10; } while (u);
    if (v < 0) buf[--i] = '-';
    out(&buf[i]);
}

static unsigned rng = 7;
static float frand(float lo, float hi)
{
    rng = rng * 1103515245u + 12345u;
    return lo + (hi - lo) * (float)((rng >> 8) & 0xffff) / 65535.0f;
}

/* Exact: does start + t*(end-start) for t in [0, fraction) touch the box?
 * Returns 1 = hit possible, 0 = no, -1 = too close to an edge to judge. */
static int reference(const float *s, const float *e, const float *mn, const float *mx, float fraction)
{
    float t0 = 0.0f, t1 = fraction;
    int i;
    for (i = 0; i < 3; i++) {
        float d = e[i] - s[i];
        if (d == 0.0f) {
            if (s[i] < mn[i] || s[i] > mx[i]) return 0;
            continue;
        }
        float a = (mn[i] - s[i]) / d, b = (mx[i] - s[i]) / d;
        if (a > b) { float tmp = a; a = b; b = tmp; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
    }
    if (t1 - t0 > 1e-3f) return 1;
    if (t0 - t1 > 1e-3f) return 0;
    return -1;
}

static int fails;

static void tb_case(const float *s, const float *e, const float *mn, const float *mx, float fr, int want_hit, const char *what)
{
    TraceExtents ex;
    int i;
    for (i = 0; i < 3; i++) { ex.start[i] = s[i]; ex.end[i] = e[i]; }
    CM_CalcTraceEntents(&ex);
    int skip = CM_TraceBox(&ex, mn, mx, fr) != 0;
    if (skip == want_hit) {
        out("FAIL: "); out(what); out(" skip="); out_int(skip); out("\n");
        fails++;
    }
}

void _start(void)
{
    /* Real case: shooter eye (894.3,805.1,43.2) aiming at player 1 at
     * (944.0,867.5,-21.9); the world was hit at fraction 0.02014 behind him.
     * Player box: origin + actorLocational bounds (-64,-64,-32)..(64,64,72). */
    float s[3] = {894.3f, 805.1f, 43.2f}, e[3] = {5670.0f, 6801.5f, -2845.6f};
    float o[3] = {944.0f, 867.5f, -21.9f};
    float mn[3] = {o[0] - 64, o[1] - 64, o[2] - 32}, mx[3] = {o[0] + 64, o[1] + 64, o[2] + 72};
    tb_case(s, e, mn, mx, 0.02014f, 1, "player in front of a wall (real run)");
    tb_case(s, e, mn, mx, 1.0f, 1, "player, nothing behind");
    float far_mn[3] = {mn[0] + 3000, mn[1] + 3000, mn[2] - 2000}, far_mx[3] = {mx[0] + 3000, mx[1] + 3000, mx[2] - 2000};
    tb_case(s, e, far_mn, far_mx, 0.02014f, 0, "player behind the wall");
    float side_mn[3] = {mn[0] + 500, mn[1], mn[2]}, side_mx[3] = {mx[0] + 500, mx[1], mx[2]};
    tb_case(s, e, side_mn, side_mx, 1.0f, 0, "player beside the ray");

    /*
     * Jules 2026-10-27: an independent early-out check with a world hit
     * fraction after the victim box (existing fix should already pass).
     * The same bounding box MUST be skipped if world geometry stops the
     * bullet before entering it; never infer damage from these tests alone.
     */
    {
        float s2[3] = {0, 0, 0};
        float e2[3] = {1000, 0, 0};
        float mn2[3] = {80, -15, -15};
        float mx2[3] = {90, 15, 15};
        tb_case(s2, e2, mn2, mx2, 165.0f / 1000.0f, 1,
                "Jules: entity precedes world hit");
        tb_case(s2, e2, mn2, mx2, 70.0f / 1000.0f, 0,
                "Jules: world hit precedes entity");
    }

    int n, judged = 0;
    for (n = 0; n < 200000; n++) {
        float a[3], b[3], lo[3], hi[3], fr = frand(0.05f, 1.0f);
        int i;
        for (i = 0; i < 3; i++) {
            a[i] = frand(-300, 300); b[i] = frand(-300, 300);
            float c = frand(-150, 150), h = frand(2, 80);
            lo[i] = c - h; hi[i] = c + h;
        }
        if (n % 7 == 0) b[n % 3] = a[n % 3];        /* axis-parallel segments */
        int want = reference(a, b, lo, hi, fr);
        if (want < 0) continue;
        judged++;
        TraceExtents ex;
        for (i = 0; i < 3; i++) { ex.start[i] = a[i]; ex.end[i] = b[i]; }
        CM_CalcTraceEntents(&ex);
        int skip = CM_TraceBox(&ex, lo, hi, fr) != 0;
        if (skip == want) {
            if (fails < 10) { out("FAIL random case "); out_int(n); out(" want_hit="); out_int(want); out("\n"); }
            fails++;
        }
    }
    if (judged < 150000) { out("FAIL: too few judged cases\n"); fails++; }
    if (fails) { out("trace_box: FAILED "); out_int(fails); out("\n"); sys3(1, 1, 0, 0); }
    out("trace_box: OK\n");
    sys3(1, 0, 0, 0);
}
