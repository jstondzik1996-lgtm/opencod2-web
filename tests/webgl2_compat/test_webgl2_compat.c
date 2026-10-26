/*
 * Host regression tests for src/web/webgl2_compat.c (no Emscripten, no game
 * assets). The REAL compat layer is compiled against a fake WebGL2 context
 * (fake_gl.c) that executes draws by fetching vertex attributes exactly as a
 * GPU front-end would.
 *
 * Checks:
 *  1. The native draw path (GL mirrors of the Direct3D buffers, attribute
 *     pointers on the original layout, colour swizzle in the shader) feeds
 *     the shader exactly the same vertices as the legacy CPU conversion path
 *     for the real CoD2 layouts: world 0x44 (lightmap), static-model cache
 *     0x40 (3 floats), skinned GfxVertex 0x40 (4 floats), 2D/UI streams,
 *     all three colour byte orders, base vertex / min vertex / start index.
 *  2. Static data is uploaded once; Lock()ed ranges are re-uploaded and
 *     nothing else; released buffers are recreated.
 *  3. Unrepresentable layouts are rejected (caller falls back to legacy).
 *  4. Redundant GL state/uniform/texture calls are skipped, and the cache is
 *     resynchronised by webgl2_state_invalidate().
 *  5. The legacy path converts only the referenced vertex range.
 *  6. Fixed-function lights are not applied to 2D draws.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_gl.h"
#include "webgl2_native.h"

/* Compat entry points (defined in src/web/webgl2_compat.c). */
void webgl2_glEnable(unsigned int cap);
void webgl2_glDisable(unsigned int cap);
void webgl2_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices);
void glActiveTextureARB(unsigned int texture);
void glClientActiveTextureARB(unsigned int texture);
void glEnableClientState(unsigned int array);
void glDisableClientState(unsigned int array);
void glVertexPointer(int size, unsigned int type, int stride, const void *pointer);
void glColorPointer(int size, unsigned int type, int stride, const void *pointer);
void glTexCoordPointer(int size, unsigned int type, int stride, const void *pointer);
void glColor4f(float r, float g, float b, float a);
void webgl2_set_ff_light(int index, int enable, const float *ambient_rgb);
void webgl2_glBindTexture(unsigned int target, unsigned int texture);
void webgl2_glTexParameteri(unsigned int target, unsigned int pname, int param);
void webgl2_glDeleteTextures(int n, const unsigned int *textures);
void webgl2_glDepthMask(unsigned char flag);
void webgl2_glBlendFunc(unsigned int s, unsigned int d);

#define GL_VERTEX_ARRAY 0x8074
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        g_checks++;                                       \
        if (!(cond)) {                                    \
            g_failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* ---- synthetic Direct3D buffers ------------------------------------------ */

typedef struct {
    const char *name;
    int stride;
    int pos_offset;
    int pos_components;
    int color_offset;   /* -1 none */
    int color_order;    /* WEBGL2_COLOR_* */
    int tex0_offset;    /* -1 none */
    int tex1_offset;    /* -1 none */
} Layout;

static const Layout k_layouts[] = {
    { "world 0x44 lightmapped (BGRA)", 0x44, 0x00, 3, 0x18, WEBGL2_COLOR_BGRA, 0x1c, 0x24 },
    { "static model cache 0x40 (BGRA)", 0x40, 0x00, 3, 0x18, WEBGL2_COLOR_BGRA, 0x1c, -1 },
    { "skinned GfxVertex 0x40 xyzw (BGRA)", 0x40, 0x00, 4, 0x1c, WEBGL2_COLOR_BGRA, 0x20, -1 },
    { "dx7 0x24 (ARGB order path)", 0x24, 0x00, 3, 0x18, WEBGL2_COLOR_ARGB, 0x1c, -1 },
    { "2D 0x18 (RGBA)", 0x18, 0x00, 3, 0x0c, WEBGL2_COLOR_RGBA, -1, -1 },
    { "position only 0x14", 0x14, 0x00, 3, -1, WEBGL2_COLOR_RGBA, 0x0c, -1 },
};

#define VB_VERTS 600
#define IB_INDICES 900

static unsigned char *g_vb;
static unsigned int g_vb_size;
static unsigned short *g_ib;
static unsigned int g_ib_size;

static void put_float(unsigned char *p, float v) { memcpy(p, &v, 4); }

static void fill_vb(const Layout *L, unsigned int seed)
{
    unsigned int v;

    g_vb_size = (unsigned int)L->stride * VB_VERTS;
    free(g_vb);
    g_vb = (unsigned char *)calloc(1, g_vb_size);
    for (v = 0; v < VB_VERTS; v++) {
        unsigned char *p = g_vb + v * (unsigned int)L->stride;
        put_float(p + L->pos_offset + 0, (float)v * 1.5f + (float)seed);
        put_float(p + L->pos_offset + 4, (float)v * -0.25f);
        put_float(p + L->pos_offset + 8, 100.0f + (float)(v % 7));
        if (L->pos_components == 4)
            put_float(p + L->pos_offset + 12, 1.0f);
        if (L->color_offset >= 0) {
            p[L->color_offset + 0] = (unsigned char)(v * 3 + seed);
            p[L->color_offset + 1] = (unsigned char)(v * 5 + 17);
            p[L->color_offset + 2] = (unsigned char)(v * 7 + 101);
            p[L->color_offset + 3] = (unsigned char)(255 - v);
        }
        if (L->tex0_offset >= 0) {
            put_float(p + L->tex0_offset + 0, (float)v / 64.0f);
            put_float(p + L->tex0_offset + 4, 1.0f - (float)v / 128.0f);
        }
        if (L->tex1_offset >= 0) {
            put_float(p + L->tex1_offset + 0, 0.5f + (float)v / 1024.0f);
            put_float(p + L->tex1_offset + 4, 0.25f + (float)v / 2048.0f);
        }
    }
}

static void fill_ib(void)
{
    unsigned int i;

    g_ib_size = IB_INDICES * 2;
    free(g_ib);
    g_ib = (unsigned short *)calloc(1, g_ib_size);
    for (i = 0; i < IB_INDICES; i++)
        g_ib[i] = (unsigned short)((i * 37u) % 64u); /* relative to MinVertexIndex 0 */
}

/* Mirrors CDirect3DDevice_ConvertColorArray: RGBA bytes indexed by vertex. */
static unsigned char *convert_colors_like_device(const unsigned char *vertBase, const Layout *L,
                                                 unsigned int firstVertex, unsigned int numVertices)
{
    unsigned int n = firstVertex + numVertices;
    unsigned char *out = (unsigned char *)calloc(n ? n : 1, 4);
    unsigned int i;

    for (i = firstVertex; i < n; i++) {
        const unsigned char *s = vertBase + i * (unsigned int)L->stride + L->color_offset;
        unsigned char *d = out + i * 4;
        if (L->color_order == WEBGL2_COLOR_RGBA) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        } else if (L->color_order == WEBGL2_COLOR_ARGB) {
            d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0];
        } else {
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
        }
    }
    return out;
}

static void set_texturing(int tex0, int tex1)
{
    glActiveTextureARB(GL_TEXTURE0 + 1);
    if (tex1)
        webgl2_glEnable(GL_TEXTURE_2D);
    else
        webgl2_glDisable(GL_TEXTURE_2D);
    glActiveTextureARB(GL_TEXTURE0);
    if (tex0)
        webgl2_glEnable(GL_TEXTURE_2D);
    else
        webgl2_glDisable(GL_TEXTURE_2D);
}

typedef struct {
    unsigned int base_vertex;
    unsigned int min_vertex;
    unsigned int num_vertices;
    unsigned int start_index;
    int index_count;
} DrawArgs;

static void legacy_draw(const Layout *L, const DrawArgs *a)
{
    const unsigned char *vertBase = g_vb + a->base_vertex * (unsigned int)L->stride;
    unsigned char *colors = NULL;

    set_texturing(L->tex0_offset >= 0, L->tex1_offset >= 0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(L->pos_components, GL_FLOAT, L->stride, vertBase + L->pos_offset);
    if (L->color_offset >= 0) {
        colors = convert_colors_like_device(vertBase, L, a->min_vertex, a->num_vertices);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, colors);
    }
    if (L->tex1_offset >= 0) {
        glClientActiveTextureARB(GL_TEXTURE0 + 1);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, L->stride, vertBase + L->tex1_offset);
        glClientActiveTextureARB(GL_TEXTURE0);
    }
    if (L->tex0_offset >= 0) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, L->stride, vertBase + L->tex0_offset);
    }
    webgl2_glDrawElements(GL_TRIANGLES, a->index_count, GL_UNSIGNED_SHORT, g_ib + a->start_index);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glClientActiveTextureARB(GL_TEXTURE0 + 1);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glClientActiveTextureARB(GL_TEXTURE0);
    free(colors);
}

static WebGL2NativeDraw make_native(const Layout *L, const DrawArgs *a)
{
    WebGL2NativeDraw d;

    memset(&d, 0, sizeof(d));
    d.vb_base = g_vb;
    d.vb_size = g_vb_size;
    d.ib_base = g_ib;
    d.ib_size = g_ib_size;
    d.base_offset = a->base_vertex * (unsigned int)L->stride;
    d.stride = L->stride;
    d.pos_offset = L->pos_offset;
    d.pos_components = L->pos_components;
    d.color_offset = L->color_offset;
    d.color_order = L->color_order;
    d.tex0_offset = L->tex0_offset;
    d.tex1_offset = L->tex1_offset;
    d.min_vertex = a->min_vertex;
    d.num_vertices = a->num_vertices;
    d.index_offset = a->start_index * 2u;
    d.index_count = a->index_count;
    return d;
}

static int native_draw(const Layout *L, const DrawArgs *a)
{
    WebGL2NativeDraw d = make_native(L, a);

    set_texturing(L->tex0_offset >= 0, L->tex1_offset >= 0);
    return webgl2_draw_native(&d);
}

static FakeVertex g_saved[FAKE_MAX_FETCH];
static int g_saved_count;

static void begin_capture(void) { fgl.fetched_count = 0; }
static void save_capture(void)
{
    g_saved_count = fgl.fetched_count;
    memcpy(g_saved, fgl.fetched, sizeof(FakeVertex) * (size_t)g_saved_count);
}

static int same_vertex(const FakeVertex *a, const FakeVertex *b)
{
    int i;
    for (i = 0; i < 3; i++)
        if (a->pos[i] != b->pos[i])
            return 0;
    for (i = 0; i < 4; i++)
        if (fabsf(a->color[i] - b->color[i]) > 1e-6f)
            return 0;
    for (i = 0; i < 2; i++)
        if (a->tex0[i] != b->tex0[i] || a->tex1[i] != b->tex1[i])
            return 0;
    return a->use_tex0 == b->use_tex0 && a->use_tex1 == b->use_tex1;
}

static void compare_with_saved(const char *what)
{
    int i;
    int first_bad = -1;

    CHECK(fgl.fetched_count == g_saved_count, "%s: native fetched %d vertices, legacy %d",
          what, fgl.fetched_count, g_saved_count);
    for (i = 0; i < fgl.fetched_count && i < g_saved_count; i++) {
        if (!same_vertex(&fgl.fetched[i], &g_saved[i])) {
            first_bad = i;
            break;
        }
    }
    CHECK(first_bad < 0, "%s: vertex %d differs (native pos %.2f col %.3f,%.3f,%.3f,%.3f / legacy pos %.2f col %.3f,%.3f,%.3f,%.3f)",
          what, first_bad,
          first_bad >= 0 ? fgl.fetched[first_bad].pos[0] : 0.0f,
          first_bad >= 0 ? fgl.fetched[first_bad].color[0] : 0.0f,
          first_bad >= 0 ? fgl.fetched[first_bad].color[1] : 0.0f,
          first_bad >= 0 ? fgl.fetched[first_bad].color[2] : 0.0f,
          first_bad >= 0 ? fgl.fetched[first_bad].color[3] : 0.0f,
          first_bad >= 0 ? g_saved[first_bad].pos[0] : 0.0f,
          first_bad >= 0 ? g_saved[first_bad].color[0] : 0.0f,
          first_bad >= 0 ? g_saved[first_bad].color[1] : 0.0f,
          first_bad >= 0 ? g_saved[first_bad].color[2] : 0.0f,
          first_bad >= 0 ? g_saved[first_bad].color[3] : 0.0f);
}

/* ---- tests ---------------------------------------------------------------- */

static void test_native_matches_legacy(void)
{
    static const DrawArgs args[] = {
        { 0, 0, 64, 0, 90 },
        { 37, 0, 64, 12, 300 },
        { 200, 0, 64, 600, 297 },
        { 500, 0, 64, 3, 6 },
    };
    size_t li;
    size_t ai;

    for (li = 0; li < sizeof(k_layouts) / sizeof(k_layouts[0]); li++) {
        const Layout *L = &k_layouts[li];
        fill_vb(L, (unsigned int)li * 11u);
        fill_ib();
        for (ai = 0; ai < sizeof(args) / sizeof(args[0]); ai++) {
            char what[160];
            int ok;

            snprintf(what, sizeof(what), "%s, base %u start %u", L->name, args[ai].base_vertex, args[ai].start_index);
            begin_capture();
            legacy_draw(L, &args[ai]);
            save_capture();
            CHECK(g_saved_count == args[ai].index_count, "%s: legacy path drew %d of %d indices",
                  what, g_saved_count, args[ai].index_count);
            begin_capture();
            ok = native_draw(L, &args[ai]);
            CHECK(ok, "%s: native path rejected a valid draw", what);
            compare_with_saved(what);
        }
        /* buffers are freed by fill_*; tell the compat layer like the D3D destructors do */
        webgl2_buffer_released(g_vb);
        webgl2_buffer_released(g_ib);
    }
    CHECK(fgl.saw_swizzle_shader, "colour swizzle missing from the vertex shader sources");
    CHECK(fgl.errors == 0, "fake WebGL context recorded %d API errors", fgl.errors);
}

static void test_mirror_uploads(void)
{
    const Layout *L = &k_layouts[0];
    DrawArgs a = { 10, 0, 64, 0, 30 };
    WebGL2Stats before;
    WebGL2Stats after;
    double expected_full = 0.0;

    fill_vb(L, 3);
    fill_ib();
    webgl2_get_stats(&before);
    native_draw(L, &a);
    webgl2_get_stats(&after);
    expected_full = (double)g_vb_size + (double)g_ib_size;
    CHECK(after.mirror_full_uploads - before.mirror_full_uploads == 2, "first draw should create 2 mirrors, made %u",
          after.mirror_full_uploads - before.mirror_full_uploads);
    CHECK((after.vertex_bytes_uploaded + after.index_bytes_uploaded) -
              (before.vertex_bytes_uploaded + before.index_bytes_uploaded) == expected_full,
          "first draw uploads both buffers once");

    /* Same static data again: no upload at all. */
    before = after;
    native_draw(L, &a);
    native_draw(L, &a);
    webgl2_get_stats(&after);
    CHECK(after.vertex_bytes_uploaded == before.vertex_bytes_uploaded &&
              after.index_bytes_uploaded == before.index_bytes_uploaded,
          "unchanged buffers must not be re-uploaded (vtx +%.0f idx +%.0f)",
          after.vertex_bytes_uploaded - before.vertex_bytes_uploaded,
          after.index_bytes_uploaded - before.index_bytes_uploaded);

    /* A Lock()ed write is re-uploaded, exactly the locked range. */
    {
        unsigned int off = (10u + g_ib[1]) * (unsigned int)L->stride; /* a vertex the draw references */
        float newx = 12345.0f;
        memcpy(g_vb + off, &newx, 4);
        webgl2_buffer_written(g_vb, off, (unsigned int)L->stride);
        before = after;
        begin_capture();
        native_draw(L, &a);
        webgl2_get_stats(&after);
        CHECK(after.vertex_bytes_uploaded - before.vertex_bytes_uploaded == (double)L->stride,
              "locked range re-upload: expected %d bytes, got %.0f", L->stride,
              after.vertex_bytes_uploaded - before.vertex_bytes_uploaded);
        {
            int i;
            int seen = 0;
            for (i = 0; i < fgl.fetched_count; i++)
                if (fgl.fetched[i].pos[0] == newx)
                    seen = 1;
            CHECK(seen, "draw after Lock() must see the new vertex data");
        }
    }

    /* Index buffer Lock(): same behaviour. */
    {
        g_ib[0] = 63;
        webgl2_buffer_written(g_ib, 0, 2);
        before = after;
        begin_capture();
        native_draw(L, &a);
        webgl2_get_stats(&after);
        CHECK(after.index_bytes_uploaded - before.index_bytes_uploaded == 2.0, "locked index range re-upload");
        CHECK(fgl.fetched_count > 0 && fgl.fetched[0].pos[0] == (float)(10 + 63) * 1.5f + 3.0f,
              "draw after index Lock() must use the new index");
    }

    /* Size 0 lock means "to the end of the buffer". */
    webgl2_buffer_written(g_vb, g_vb_size - (unsigned int)L->stride * 2u, 0);
    before = after;
    native_draw(L, &a);
    webgl2_get_stats(&after);
    CHECK(after.vertex_bytes_uploaded - before.vertex_bytes_uploaded == (double)L->stride * 2.0,
          "size-0 lock re-uploads to the end");

    /* Released and reallocated (new contents, maybe same address): fresh mirror. */
    webgl2_buffer_released(g_vb);
    webgl2_buffer_released(g_ib);
    fill_vb(L, 99);
    fill_ib();
    begin_capture();
    legacy_draw(L, &a);
    save_capture();
    begin_capture();
    native_draw(L, &a);
    compare_with_saved("after release/realloc");
    webgl2_buffer_released(g_vb);
    webgl2_buffer_released(g_ib);
    CHECK(fgl.errors == 0, "fake WebGL context recorded %d API errors", fgl.errors);
}

static void test_rejects(void)
{
    Layout odd = { "odd stride", 0x22, 0, 3, -1, 0, -1, -1 };
    DrawArgs a = { 0, 0, 16, 0, 6 };
    WebGL2NativeDraw d;

    fill_vb(&k_layouts[1], 0);
    fill_ib();
    d = make_native(&odd, &a);
    CHECK(!webgl2_draw_native(&d), "unaligned stride must fall back to the legacy path");
    d = make_native(&k_layouts[1], &a);
    d.index_offset = g_ib_size; /* past the end */
    CHECK(!webgl2_draw_native(&d), "index range past the buffer must be rejected");
    d = make_native(&k_layouts[1], &a);
    d.base_offset = g_vb_size + 64;
    CHECK(!webgl2_draw_native(&d), "base vertex past the buffer must be rejected");
    d = make_native(&k_layouts[1], &a);
    d.min_vertex = VB_VERTS;
    d.num_vertices = 10;
    CHECK(!webgl2_draw_native(&d), "vertex range past the buffer must be rejected");
    webgl2_buffer_released(g_vb);
    webgl2_buffer_released(g_ib);
}

static void test_state_cache(void)
{
    int before;

    webgl2_state_invalidate();
    fake_gl_reset_counters();
    webgl2_glEnable(GL_DEPTH_TEST);
    webgl2_glEnable(GL_DEPTH_TEST);
    webgl2_glEnable(GL_DEPTH_TEST);
    CHECK(fgl.calls.enable == 1, "repeated glEnable(DEPTH_TEST) issued %d real calls", fgl.calls.enable);
    webgl2_glDisable(GL_DEPTH_TEST);
    webgl2_glDisable(GL_DEPTH_TEST);
    CHECK(fgl.calls.disable == 1 && !fgl.depth_test, "repeated glDisable issued %d real calls", fgl.calls.disable);
    webgl2_state_invalidate();
    webgl2_glDisable(GL_DEPTH_TEST);
    CHECK(fgl.calls.disable == 2, "after invalidate the next call must reach GL");

    webgl2_glDepthMask(1);
    webgl2_glDepthMask(1);
    webgl2_glBlendFunc(1, 0x0303);
    webgl2_glBlendFunc(1, 0x0303);
    CHECK(fgl.calls.depth_mask == 1 && fgl.calls.blend_func == 1, "depth mask / blend func not cached");

    /* Texture binds and parameters. */
    fake_gl_reset_counters();
    glActiveTextureARB(GL_TEXTURE0);
    webgl2_glBindTexture(GL_TEXTURE_2D, 5);
    webgl2_glBindTexture(GL_TEXTURE_2D, 5);
    CHECK(fgl.calls.bind_texture == 1, "redundant texture bind reached GL (%d)", fgl.calls.bind_texture);
    webgl2_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x2901);
    webgl2_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x2901);
    webgl2_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    CHECK(fgl.calls.tex_parameter == 2, "texture parameter cache: expected 2 real calls, got %d", fgl.calls.tex_parameter);
    glActiveTextureARB(GL_TEXTURE0 + 1);
    webgl2_glBindTexture(GL_TEXTURE_2D, 5);
    CHECK(fgl.calls.bind_texture == 2 && fgl.bound_tex[1] == 5, "bind on another unit must reach GL");
    glActiveTextureARB(GL_TEXTURE0);
    before = fgl.calls.bind_texture;
    {
        unsigned int t = 5;
        webgl2_glDeleteTextures(1, &t);
    }
    webgl2_glBindTexture(GL_TEXTURE_2D, 5);
    CHECK(fgl.calls.bind_texture == before + 1 && fgl.bound_tex[0] == 5,
          "after glDeleteTextures the binding is 0 in GL; re-binding must not be skipped");
    /* A deleted texture's parameters are unknown again. */
    webgl2_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
    CHECK(fgl.calls.tex_parameter == 3, "parameters of a deleted texture must be set again");
}

static void test_legacy_range(void)
{
    const Layout *L = &k_layouts[1];
    unsigned short idx[6] = { 500, 501, 502, 500, 502, 503 };
    WebGL2Stats before;
    WebGL2Stats after;
    const unsigned char *vertBase;

    fill_vb(L, 1);
    vertBase = g_vb;
    webgl2_get_stats(&before);
    begin_capture();
    set_texturing(1, 0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, L->stride, vertBase);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, L->stride, vertBase + L->tex0_offset);
    webgl2_glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    webgl2_get_stats(&after);
    /* 4 converted vertices (48 bytes each) + 6 16-bit indices. */
    CHECK(after.legacy_bytes_uploaded - before.legacy_bytes_uploaded == 4.0 * 48.0 + 12.0,
          "legacy path converted %.0f bytes for a 4-vertex draw at the end of a buffer",
          after.legacy_bytes_uploaded - before.legacy_bytes_uploaded);
    CHECK(fgl.fetched_count == 6 && fgl.fetched[0].pos[0] == 500.0f * 1.5f + 1.0f &&
              fgl.fetched[5].pos[0] == 503.0f * 1.5f + 1.0f,
          "rebased legacy indices must still address the right vertices");
}

static void test_lighting_gate(void)
{
    const Layout *L = &k_layouts[1];
    DrawArgs a = { 0, 0, 16, 0, 3 };
    float ambient[3] = { 0.5f, 0.4f, 0.3f };

    fill_vb(L, 2);
    fill_ib();
    webgl2_set_ff_light(0, 1, ambient);
    webgl2_set_lighting_allowed(1);
    begin_capture();
    native_draw(L, &a);
    CHECK(fgl.fetched_count && fgl.fetched[0].use_light == 1, "3D draw with an enabled light must be lit");
    webgl2_set_lighting_allowed(0);
    begin_capture();
    native_draw(L, &a);
    CHECK(fgl.fetched_count && fgl.fetched[0].use_light == 0, "2D draw must ignore fixed-function lights");
    webgl2_set_lighting_allowed(1);
    webgl2_set_ff_light(0, 0, NULL);
    webgl2_buffer_released(g_vb);
    webgl2_buffer_released(g_ib);
}

static void test_rebase_helper(void)
{
    unsigned short src[5] = { 40, 41, 45, 40, 44 };
    unsigned short dst[5];
    unsigned int lo = 0, hi = 0;

    CHECK(webgl2_rebase_indices16(src, 5, dst, &lo, &hi) && lo == 40 && hi == 45 &&
              dst[0] == 0 && dst[2] == 5 && dst[4] == 4,
          "index rebase helper");
}

static void test_uniform_cache(void)
{
    const Layout *L = &k_layouts[1];
    DrawArgs a = { 0, 0, 16, 0, 3 };
    int first;

    fill_vb(L, 4);
    fill_ib();
    native_draw(L, &a);
    fake_gl_reset_counters();
    native_draw(L, &a);
    first = fgl.calls.uniform;
    CHECK(first == 0, "identical consecutive draws re-sent %d uniforms", first);
    CHECK(fgl.calls.attrib_pointer == 0, "identical consecutive draws re-specified %d attribute pointers",
          fgl.calls.attrib_pointer);
    webgl2_buffer_released(g_vb);
    webgl2_buffer_released(g_ib);
}

int main(void)
{
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    test_native_matches_legacy();
    test_mirror_uploads();
    test_rejects();
    test_state_cache();
    test_legacy_range();
    test_lighting_gate();
    test_rebase_helper();
    test_uniform_cache();

    if (g_failures) {
        printf("webgl2_compat: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("webgl2_compat: OK (%d checks)\n", g_checks);
    return 0;
}
