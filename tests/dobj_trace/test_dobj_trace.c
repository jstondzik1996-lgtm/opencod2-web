/*
 * Host regression test for the server's per-bone hit detection,
 * DObjTraceline() in src/PC/xanim/dobj.c (bullet vs player hitboxes).
 *
 * The REAL dobj.c is compiled as 32-bit x86 and run on a synthetic skeleton
 * (no game assets): bone matrices, bone hitboxes (XBoneInfo), part
 * classifications (hit locations) and a priority map are built here.
 *
 * Checks: entry fraction and face normal, misses, rotated/translated bones
 * (quaternion convention), the caller's current fraction limit (a wall in
 * front), priority rules (a higher-priority part behind wins, priority 0 is
 * ignored, priority 1 inherits the parent's classification), ignored models
 * and the start-inside case.
 *
 * Not tested here: animation/pose (G_DObjCalcPose), real xmodel data, the
 * server entity transform and damage scripts. See tools/browser_multiplayer_check.py.
 */
#include "common_types.h"

void DObjTraceline(DObj *obj, vec_t *start, vec_t *end, unsigned char *priorityMap, DObjTrace *trace);

/* ---- freestanding runtime ---------------------------------------------- */
static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static void out(const char *s)
{
    int n = 0;
    while (s[n])
        ++n;
    sys3(4, 1, (int)s, n);
}
static void out_int(int v)
{
    char buf[16];
    int i = 15;
    unsigned int u = v < 0 ? (unsigned int)-v : (unsigned int)v;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        buf[--i] = '-';
    out(&buf[i]);
}
float sqrtf(float x)
{
    float r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}
void *memset(void *d, int c, unsigned int n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned int n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}
const char *SL_ConvertToString(unsigned int v) { (void)v; return ""; }

/* ---- synthetic skeleton ------------------------------------------------- */
#define NB 3
static struct {
    DSkel skel;
    DObjAnimMat more[NB - 1];
} g_skel;
static XBoneInfo g_info[NB];
static unsigned char g_class[NB];
static struct { unsigned short *names; unsigned char parentList[NB]; } g_hier;
static unsigned short g_names[NB] = {101, 102, 103};
static XModelParts g_parts;
static XModel g_model;
static DObj g_obj;
static unsigned char g_prio[256];
static int g_fail;

static DObjAnimMat *mat(int i) { return &g_skel.skel.mat[i]; }

static void set_identity(int i, float x, float y, float z)
{
    DObjAnimMat *m = mat(i);
    m->quat[0] = m->quat[1] = m->quat[2] = 0.0f;
    m->quat[3] = 1.0f;
    m->trans[0] = x;
    m->trans[1] = y;
    m->trans[2] = z;
    m->transWeight = 2.0f; /* 2 / |q|^2 */
}

static void set_box(int i, float x0, float y0, float z0, float x1, float y1, float z1)
{
    XBoneInfo *b = &g_info[i];
    float dx, dy, dz;
    b->bounds[0][0] = x0; b->bounds[0][1] = y0; b->bounds[0][2] = z0;
    b->bounds[1][0] = x1; b->bounds[1][1] = y1; b->bounds[1][2] = z1;
    /* Same derivation as xmodel_load_obj.c. */
    b->offset[0] = (x0 + x1) * 0.5f;
    b->offset[1] = (y0 + y1) * 0.5f;
    b->offset[2] = (z0 + z1) * 0.5f;
    dx = x1 - b->offset[0];
    dy = y1 - b->offset[1];
    dz = z1 - b->offset[2];
    b->radiusSquared = dx * dx + dy * dy + dz * dz;
}

static void reset(void)
{
    int i;
    memset(&g_skel, 0, sizeof(g_skel));
    memset(g_info, 0, sizeof(g_info));
    for (i = 0; i < 256; ++i)
        g_prio[i] = 0;
    g_parts.numBones = NB;
    g_parts.numRootBones = 1;
    g_hier.names = g_names;
    g_hier.parentList[0] = 1; /* bone 1's parent is bone 0 */
    g_hier.parentList[1] = 2; /* bone 2's parent is bone 0 */
    g_parts.hierarchy = (XBoneHierarchy *)&g_hier;
    g_parts.partClassification = g_class;
    g_model.parts = (void (*)())&g_parts;
    g_model.boneInfo = g_info;
    memset(&g_obj, 0, sizeof(g_obj));
    g_obj.skel = &g_skel.skel;
    g_obj.numModels = 1;
    g_obj.numBones = NB;
    g_obj.models[0] = &g_model;
    g_obj.modelParents[0] = 0xff;
    for (i = 0; i < NB; ++i)
        set_identity(i, 0, 0, 0);
    /* classifications: 1 = head, 2 = torso, 3 = arm (arbitrary ids) */
    g_class[0] = 2;
    g_class[1] = 1;
    g_class[2] = 3;
}

static int near(float a, float b) { return a - b < 0.0005f && b - a < 0.0005f; }

static DObjTrace run(float sx, float sy, float sz, float ex, float ey, float ez, float limit)
{
    DObjTrace t;
    vec3_t s = {sx, sy, sz};
    vec3_t e = {ex, ey, ez};
    memset(&t, 0, sizeof(t));
    t.fraction = limit;
    DObjTraceline(&g_obj, s, e, g_prio, &t);
    return t;
}

static void expect(int ok, const char *what, float got)
{
    if (ok)
        return;
    out("FAIL: ");
    out(what);
    out(" got*1000=");
    out_int((int)(got * 1000.0f));
    out("\n");
    ++g_fail;
}

static void tests(void)
{
    DObjTrace t;

    /* 1. Straight hit on an axis-aligned box: enter at x=-10 -> 90/200. */
    reset();
    g_prio[2] = 2;
    set_box(0, -10, -10, -10, 10, 10, 10);
    t = run(-100, 0, 0, 100, 0, 0, 1.0f);
    expect(near(t.fraction, 0.45f), "basic fraction", t.fraction);
    expect(t.partGroup == 2, "basic partGroup", t.partGroup);
    expect(t.partName == 101, "basic partName", t.partName);
    expect(near(t.normal[0], -1.0f) && near(t.normal[1], 0) && near(t.normal[2], 0), "basic normal", t.normal[0]);

    /* 2. Miss beside the box (inside the bounding sphere, outside the box). */
    t = run(-100, 12, 12, 100, 12, 12, 1.0f);
    expect(t.fraction == 1.0f, "miss keeps fraction", t.fraction);
    expect(t.partGroup == 0, "miss partGroup", t.partGroup);

    /* 3. A closer world hit (fraction limit 0.3) wins over the bone. */
    t = run(-100, 0, 0, 100, 0, 0, 0.3f);
    expect(near(t.fraction, 0.3f) && t.partGroup == 0, "limit respected", t.fraction);

    /* 4. Translated bone: box centred at y=50 -> ray at y=0 misses, y=50 hits. */
    reset();
    g_prio[2] = 2;
    set_box(0, -10, -10, -10, 10, 10, 10);
    set_identity(0, 0, 50, 0);
    t = run(-100, 0, 0, 100, 0, 0, 1.0f);
    expect(t.fraction == 1.0f, "translated miss", t.fraction);
    t = run(-100, 50, 0, 100, 50, 0, 1.0f);
    expect(near(t.fraction, 0.45f), "translated hit", t.fraction);

    /* 5. Bone rotated +90 deg about Z: local +X box [0,30] lies along world +Y.
     *    A ray along world X at y=20 enters the local -Y face (world x=-5). */
    reset();
    g_prio[2] = 2;
    set_box(0, 0, -5, -5, 30, 5, 5);
    mat(0)->quat[2] = 0.70710678f;
    mat(0)->quat[3] = 0.70710678f;
    t = run(-100, 20, 0, 100, 20, 0, 1.0f);
    expect(near(t.fraction, 0.475f), "rotated fraction", t.fraction);
    expect(near(t.normal[0], -1.0f) && near(t.normal[1], 0.0f), "rotated normal", t.normal[0]);
    t = run(-100, -20, 0, 100, -20, 0, 1.0f);
    expect(t.fraction == 1.0f, "rotated opposite side misses", t.fraction);

    /* 6. Priority: torso (prio 2) in front at x=-10, head (prio 3) behind at
     *    x=+20. The higher priority part wins even though it is farther. */
    reset();
    g_prio[2] = 2;
    g_prio[1] = 3;
    set_box(0, -10, -10, -10, 10, 10, 10);
    set_box(1, 20, -10, -10, 40, 10, 10);
    t = run(-100, 0, 0, 100, 0, 0, 1.0f);
    expect(t.partGroup == 1 && near(t.fraction, 0.6f), "higher priority behind wins", t.fraction);

    /* 7. Same priority: the nearer part wins. */
    g_prio[1] = 2;
    t = run(-100, 0, 0, 100, 0, 0, 1.0f);
    expect(t.partGroup == 2 && near(t.fraction, 0.45f), "same priority nearest", t.fraction);

    /* 8. Priority 0 parts are never hit. */
    g_prio[2] = 0;
    g_prio[1] = 0;
    t = run(-100, 0, 0, 100, 0, 0, 1.0f);
    expect(t.fraction == 1.0f, "priority 0 ignored", t.fraction);

    /* 9. Priority 1 inherits the parent's classification: bone 1 (class 4,
     *    prio 1) reports its parent bone 0's class 2. */
    reset();
    g_class[1] = 4;
    g_prio[4] = 1;
    g_prio[2] = 2;
    set_box(1, -10, -10, -10, 10, 10, 10);
    t = run(-100, 0, 0, 100, 0, 0, 1.0f);
    expect(t.partGroup == 2 && near(t.fraction, 0.45f), "priority 1 inherits parent", t.partGroup);

    /* 10. ignoreCollision on the model disables all its bones. */
    reset();
    g_prio[2] = 2;
    set_box(0, -10, -10, -10, 10, 10, 10);
    g_obj.ignoreCollision = 1;
    t = run(-100, 0, 0, 100, 0, 0, 1.0f);
    expect(t.fraction == 1.0f, "ignoreCollision", t.fraction);

    /* 11. Start and end inside, moving toward the model origin: fraction 0. */
    g_obj.ignoreCollision = 0;
    t = run(5, 0, 0, -5, 0, 0, 1.0f);
    expect(t.fraction == 0.0f && t.partGroup == 2, "start solid toward origin", t.fraction);

    /* 12. Start inside, end outside (leaving): not a hit. */
    t = run(0, 0, 0, 100, 0, 0, 1.0f);
    expect(t.fraction == 1.0f, "leaving box is not a hit", t.fraction);
}

void _start(void)
{
    tests();
    if (g_fail) {
        out("dobj_trace: FAILED\n");
        sys3(1, 1, 0, 0);
    }
    out("dobj_trace: OK\n");
    sys3(1, 0, 0, 0);
}
