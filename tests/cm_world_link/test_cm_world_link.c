/*
 * Host regression test for the server world-sector entity lists in
 * src/PC/qcommon/cm_world.c. It links the real cm_world.c against minimal
 * stubs, as 32-bit x86 (the headers assert the original structure layout).
 *
 * What it checks:
 *  - entity 0 (the first player) can be linked and found;
 *  - CM_AreaEntities always equals a brute-force scan while many entities
 *    are linked, moved across sector boundaries (forcing CM_SortNode
 *    splits/reinsertion) and unlinked;
 *  - CM_UnlinkEntity terminates (run under `timeout`; the old zero-based
 *    writer encoding made its search loop spin forever).
 *
 * This is NOT a combat integration test: no bullets, DObj hit models,
 * snapshots or network are involved. See tools/browser_multiplayer_check.py.
 */
#include "common_types.h"

server_t sv;
clipMap_t cm;
void *imp_sv = &sv;

#define NUM_TEST_ENTS 96
#define WORLD_HALF 4096.0f

static gentity_t g_ents[NUM_TEST_ENTS];
static cmodel_t g_box;
static int g_failures;

/* ---- freestanding runtime -------------------------------------------- */
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
    int neg = v < 0;
    unsigned int u = neg ? (unsigned int)-v : (unsigned int)v;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg)
        buf[--i] = '-';
    out(&buf[i]);
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

/* ---- engine stubs ------------------------------------------------------ */
gentity_t *SV_GEntityForSvEntity(svEntity_t *svEnt) { return &g_ents[svEnt - sv.svEntities]; }
void CM_ModelBounds(clipHandle_t model, vec_t *mins, vec_t *maxs)
{
    (void)model;
    mins[0] = mins[1] = mins[2] = -WORLD_HALF;
    maxs[0] = maxs[1] = maxs[2] = WORLD_HALF;
}
cmodel_t *CM_ClipHandleToModel(clipHandle_t handle) { (void)handle; return &g_box; }
int XModelGetContents(const XModel *model) { (void)model; return 0; }
void Com_DPrintf(const char *fmt, ...) { (void)fmt; }
void CM_CalcTraceEntents(TraceExtents *extents) { (void)extents; }
int SV_PointSightTraceToEntity(const sightpointtrace_t *clip, svEntity_t *check) { (void)clip; (void)check; return 0; }
qboolean CM_TraceBox(const TraceExtents *e, const vec_t *mins, const vec_t *maxs, float f) { (void)e; (void)mins; (void)maxs; (void)f; return 0; }
qboolean CM_TraceStaticModelComplete(cStaticModel_t *sm, const vec_t *s, const vec_t *e, int m) { (void)sm; (void)s; (void)e; (void)m; return 1; }
void CM_TraceStaticModel(cStaticModel_t *sm, trace_t *r, const vec_t *s, const vec_t *e, int m) { (void)sm; (void)r; (void)s; (void)e; (void)m; }
int SV_ClipSightToEntity(const sightclip_t *clip, svEntity_t *check) { (void)clip; (void)check; return 0; }
void SV_PointTraceToEntity(const pointtrace_t *clip, svEntity_t *check, trace_t *trace) { (void)clip; (void)check; (void)trace; }
void SV_ClipMoveToEntity(const moveclip_t *clip, svEntity_t *check, trace_t *trace) { (void)clip; (void)check; (void)trace; }

void CM_LinkWorld(void);
void CM_LinkEntity(svEntity_t *ent, vec_t *absmin, vec_t *absmax, clipHandle_t clipHandle);
void CM_UnlinkEntity(svEntity_t *ent);
int CM_AreaEntities(const vec_t *mins, const vec_t *maxs, int *entityList, int maxcount, int contentmask);

/* ---- helpers ----------------------------------------------------------- */
static unsigned int rng = 12345u;
static float frand(float lo, float hi)
{
    rng = rng * 1103515245u + 12345u;
    return lo + (hi - lo) * (float)((rng >> 8) & 0xFFFF) / 65535.0f;
}

static void fail(const char *what, int a, int b)
{
    out("FAIL: ");
    out(what);
    out(" ");
    out_int(a);
    out(" ");
    out_int(b);
    out("\n");
    ++g_failures;
}

static void place(int i, float x, float y, float z, int contents)
{
    gentity_t *g = &g_ents[i];
    g->r.contents = contents;
    g->r.absmin[0] = x - 15.0f;
    g->r.absmin[1] = y - 15.0f;
    g->r.absmin[2] = z;
    g->r.absmax[0] = x + 15.0f;
    g->r.absmax[1] = y + 15.0f;
    g->r.absmax[2] = z + 70.0f;
    /* The temp box hull as built in cm_load_obj.c: leaf contents -1, the
     * brush carries the entity's real contents. */
    g_box.leaf.brushContents = -1;
    g_box.leaf.terrainContents = 0;
    if (contents)
        CM_LinkEntity(&sv.svEntities[i], g->r.absmin, g->r.absmax, 0xFFF);
    else
        CM_UnlinkEntity(&sv.svEntities[i]);
}

static int linked[NUM_TEST_ENTS];

static void check_area(const vec_t *mins, const vec_t *maxs, int mask, const char *tag)
{
    int list[NUM_TEST_ENTS + 8];
    int seen[NUM_TEST_ENTS];
    int count;
    int i;

    for (i = 0; i < NUM_TEST_ENTS; ++i)
        seen[i] = 0;
    count = CM_AreaEntities(mins, maxs, list, NUM_TEST_ENTS + 8, mask);
    for (i = 0; i < count; ++i) {
        if (list[i] < 0 || list[i] >= NUM_TEST_ENTS) {
            fail(tag, list[i], -1);
            return;
        }
        if (seen[list[i]]++)
            fail("duplicate entity", list[i], count);
    }
    for (i = 0; i < NUM_TEST_ENTS; ++i) {
        gentity_t *g = &g_ents[i];
        int expected = linked[i] && (g->r.contents & mask) &&
                       g->r.absmin[0] <= maxs[0] && mins[0] <= g->r.absmax[0] &&
                       g->r.absmin[1] <= maxs[1] && mins[1] <= g->r.absmax[1] &&
                       g->r.absmin[2] <= maxs[2] && mins[2] <= g->r.absmax[2];
        if (expected != seen[i])
            fail(tag, i, seen[i]);
    }
}

static void check_whole_world(const char *tag)
{
    vec3_t mins = {-WORLD_HALF, -WORLD_HALF, -WORLD_HALF};
    vec3_t maxs = {WORLD_HALF, WORLD_HALF, WORLD_HALF};
    check_area(mins, maxs, -1, tag);
}

static void run(void)
{
    const int PLAYER_CONTENTS = 0x2000000; /* CONTENTS_BODY */
    int i, step;

    CM_LinkWorld();

    /* Entity 0 alone: the old encoding stored it as the 0 sentinel. */
    place(0, 100.0f, 100.0f, 0.0f, PLAYER_CONTENTS);
    linked[0] = 1;
    check_whole_world("entity0 linked");
    place(0, -2500.0f, 1800.0f, 0.0f, PLAYER_CONTENTS);
    check_whole_world("entity0 moved");

    for (i = 1; i < NUM_TEST_ENTS; ++i) {
        place(i, frand(-3900, 3900), frand(-3900, 3900), frand(-200, 200),
              (i % 7 == 0) ? 0x1 : PLAYER_CONTENTS);
        linked[i] = 1;
    }
    check_whole_world("all linked");

    for (step = 0; step < 4000; ++step) {
        i = (int)(frand(0, NUM_TEST_ENTS - 0.01f));
        if (step % 13 == 0) {
            place(i, 0, 0, 0, 0);
            linked[i] = 0;
        } else {
            place(i, frand(-3900, 3900), frand(-3900, 3900), frand(-200, 200),
                  (i % 7 == 0) ? 0x1 : PLAYER_CONTENTS);
            linked[i] = 1;
        }
        if (step % 50 == 0) {
            vec3_t mins, maxs;
            mins[0] = frand(-4096, 3000);
            mins[1] = frand(-4096, 3000);
            mins[2] = -4096;
            maxs[0] = mins[0] + frand(10, 1500);
            maxs[1] = mins[1] + frand(10, 1500);
            maxs[2] = 4096;
            check_area(mins, maxs, PLAYER_CONTENTS, "random box");
            check_whole_world("whole world");
        }
        if (g_failures > 20)
            return;
    }

    for (i = 0; i < NUM_TEST_ENTS; ++i) {
        place(i, 0, 0, 0, 0);
        linked[i] = 0;
    }
    check_whole_world("all unlinked");
}

void _start(void)
{
    run();
    if (g_failures) {
        out("cm_world_link: FAILED\n");
        sys3(1, 1, 0, 0);
    }
    out("cm_world_link: OK\n");
    sys3(1, 0, 0, 0);
}
