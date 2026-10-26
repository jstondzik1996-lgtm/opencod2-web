#ifdef __EMSCRIPTEN__

#include <GLES3/gl3.h>
#include <emscripten.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webgl2_native.h"

#define WEBGL2_WEAK __attribute__((weak))

#ifndef GL_ALPHA_TEST
#define GL_ALPHA_TEST 0x0BC0
#endif
#ifndef GL_MODELVIEW
#define GL_MODELVIEW 0x1700
#endif
#ifndef GL_PROJECTION
#define GL_PROJECTION 0x1701
#endif
#ifndef GL_TEXTURE
#define GL_TEXTURE 0x1702
#endif
#ifndef GL_TEXTURE_ENV
#define GL_TEXTURE_ENV 0x2300
#endif
#ifndef GL_TEXTURE_ENV_MODE
#define GL_TEXTURE_ENV_MODE 0x2200
#endif
#ifndef GL_MODULATE
#define GL_MODULATE 0x2100
#endif
#ifndef GL_REPLACE
#define GL_REPLACE 0x1E01
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY 0x8074
#endif
#ifndef GL_NORMAL_ARRAY
#define GL_NORMAL_ARRAY 0x8075
#endif
#ifndef GL_COLOR_ARRAY
#define GL_COLOR_ARRAY 0x8076
#endif
#ifndef GL_TEXTURE_COORD_ARRAY
#define GL_TEXTURE_COORD_ARRAY 0x8078
#endif
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_TEXTURE_CUBE_MAP
#define GL_TEXTURE_CUBE_MAP 0x8513
#endif
#ifndef GL_QUADS
#define GL_QUADS 0x0007
#endif
#ifndef GL_QUAD_STRIP
#define GL_QUAD_STRIP 0x0008
#endif
#ifndef GL_POLYGON
#define GL_POLYGON 0x0009
#endif

#define WEBGL2_MAX_TEX_UNITS 16

typedef struct {
    int enabled;
    int size;
    GLenum type;
    int stride;
    const unsigned char *pointer;
    int normalized;
    int bgra;
} WebGL2ClientArray;

typedef struct {
    float xyz[3];
    float color[4];
    float tex0[3];
    float tex1[2];
} WebGL2Vertex;

/* One cached uniform value (scalars, ints and vec3). */
typedef struct {
    GLint loc;
    int valid;
    float v[3];
    int iv;
} WebGL2UniformCache;

typedef struct {
    GLuint id;
    GLint mvp_loc;
    int mvp_valid;
    float mvp[16];
    WebGL2UniformCache use_tex0;
    WebGL2UniformCache use_tex1;
    WebGL2UniformCache lmap_scale;
    WebGL2UniformCache use_light;
    WebGL2UniformCache light;
    WebGL2UniformCache alpha_test;
    WebGL2UniformCache alpha_func;
    WebGL2UniformCache alpha_ref;
    WebGL2UniformCache fog_enable;
    WebGL2UniformCache fog_color;
    WebGL2UniformCache fog_start;
    WebGL2UniformCache fog_end;
    WebGL2UniformCache color_order;
    WebGL2UniformCache eye;
} WebGL2Program;

typedef struct {
    WebGL2Program p2d;
    WebGL2Program pcube;
    GLuint vbo;
    GLuint ibo;
    GLuint vao;          /* legacy converted-vertex layout, attributes fixed at init */
    GLuint native_vao;   /* native D3D layout, attributes re-pointed per draw */
    GLenum matrix_mode;
    float modelview[16];
    float projection[16];
    float texture[16];
    float modelview_stack[32][16];
    float projection_stack[32][16];
    int modelview_depth;
    int projection_depth;
    float color[4];
    float texcoord[2][4];
    int active_tex_unit;
    int client_tex_unit;
    int texture_2d_enabled[2];
    int cube_map_enabled[2];
    int tex_env_mode[2];
    int alpha_test;
    GLenum alpha_func;
    float alpha_ref;
    int fog_enable;
    float fog_color[4];
    float fog_start;
    float fog_end;
    float fog_density;
    int fog_mode;
    WebGL2ClientArray vertex_array;
    WebGL2ClientArray color_array;
    WebGL2ClientArray texcoord_array[2];
    WebGL2ClientArray normal_array;
    int light_enabled[8];
    float light_ambient[8][3];
    int in_begin;
    GLenum begin_mode;
    WebGL2Vertex *immediate;
    int immediate_count;
    int immediate_capacity;
} WebGL2CompatState;

static WebGL2CompatState gl2;
static WebGL2Stats s_stats;
static int s_lighting_allowed = 1;

/* ------------------------------------------------------------------------- */
/* Environment switches (read once). Set through Module.ENV / URL parameters. */
/* ------------------------------------------------------------------------- */

static int s_env_loaded;
static int s_env_legacy;      /* COD2_GL_LEGACY: use only the old conversion path */
static int s_env_stats;       /* COD2_GL_STATS: print per-300-frame statistics */
static int s_env_reupload;    /* COD2_GL_REUPLOAD: re-send used range of dynamic buffers */

static void load_env(void)
{
    const char *e;

    if (s_env_loaded)
        return;
    s_env_loaded = 1;
    e = getenv("COD2_GL_LEGACY");
    s_env_legacy = e && e[0] && e[0] != '0';
    e = getenv("COD2_GL_STATS");
    s_env_stats = e && e[0] && e[0] != '0';
    e = getenv("COD2_GL_REUPLOAD");
    s_env_reupload = e && e[0] && e[0] != '0';
    if (s_env_legacy || s_env_stats || s_env_reupload)
        printf("[gl] COD2_GL_LEGACY=%d COD2_GL_STATS=%d COD2_GL_REUPLOAD=%d\n",
               s_env_legacy, s_env_stats, s_env_reupload);
}

int webgl2_native_enabled(void)
{
    load_env();
    return !s_env_legacy;
}

/* ------------------------------------------------------------------------- */
/* GL state cache.                                                            */
/*                                                                            */
/* Every writer of these states goes through the macros in webgl2_compat.h   */
/* (imports/opengl.h), so the cache mirrors the real context. It is still    */
/* dropped once per frame (webgl2_frame_end) as a self-healing measure.      */
/* ------------------------------------------------------------------------- */

#define WEBGL2_CAP_SLOTS 7

static signed char s_caps[WEBGL2_CAP_SLOTS];
static int s_depth_func_valid;
static GLenum s_depth_func;
static int s_depth_mask = -1;
static int s_blend_valid;
static GLenum s_blend_src;
static GLenum s_blend_dst;
static int s_color_mask = -1;
static int s_program_valid;
static GLuint s_program;
static int s_vao_valid;
static GLuint s_vao;
static int s_array_buffer_valid;
static GLuint s_array_buffer;
static int s_active_unit_valid;
static int s_active_unit;
static unsigned char s_bound_tex_valid[2][WEBGL2_MAX_TEX_UNITS];
static GLuint s_bound_tex[2][WEBGL2_MAX_TEX_UNITS];

static int cap_slot(GLenum cap)
{
    switch (cap) {
    case GL_DEPTH_TEST:
        return 0;
    case GL_BLEND:
        return 1;
    case GL_CULL_FACE:
        return 2;
    case GL_SCISSOR_TEST:
        return 3;
    case GL_POLYGON_OFFSET_FILL:
        return 4;
    case GL_DITHER:
        return 5;
    case GL_SAMPLE_ALPHA_TO_COVERAGE:
        return 6;
    default:
        return -1;
    }
}

void webgl2_state_invalidate(void)
{
    memset(s_caps, 0xff, sizeof(s_caps));
    s_depth_func_valid = 0;
    s_depth_mask = -1;
    s_blend_valid = 0;
    s_color_mask = -1;
    s_program_valid = 0;
    s_vao_valid = 0;
    s_array_buffer_valid = 0;
    s_active_unit_valid = 0;
    memset(s_bound_tex_valid, 0, sizeof(s_bound_tex_valid));
}

static void cached_cap(GLenum cap, int on)
{
    int slot = cap_slot(cap);

    if (slot >= 0 && s_caps[slot] == on) {
        s_stats.state_calls_skipped++;
        return;
    }
    if (on)
        glEnable(cap);
    else
        glDisable(cap);
    if (slot >= 0)
        s_caps[slot] = (signed char)on;
    s_stats.state_calls_issued++;
}

static void use_program(GLuint program)
{
    if (s_program_valid && s_program == program) {
        s_stats.state_calls_skipped++;
        return;
    }
    glUseProgram(program);
    s_program = program;
    s_program_valid = 1;
    s_stats.state_calls_issued++;
}

static void bind_vao(GLuint vao)
{
    if (s_vao_valid && s_vao == vao) {
        s_stats.state_calls_skipped++;
        return;
    }
    glBindVertexArray(vao);
    s_vao = vao;
    s_vao_valid = 1;
    s_stats.state_calls_issued++;
}

static void bind_array_buffer(GLuint buf)
{
    if (s_array_buffer_valid && s_array_buffer == buf) {
        s_stats.state_calls_skipped++;
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    s_array_buffer = buf;
    s_array_buffer_valid = 1;
    s_stats.state_calls_issued++;
}

static void set_active_unit(int unit)
{
    if (s_active_unit_valid && s_active_unit == unit) {
        s_stats.state_calls_skipped++;
        return;
    }
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    s_active_unit = unit;
    s_active_unit_valid = 1;
    s_stats.state_calls_issued++;
}

static int tex_target_slot(GLenum target)
{
    if (target == GL_TEXTURE_2D)
        return 0;
    if (target == GL_TEXTURE_CUBE_MAP)
        return 1;
    return -1;
}

static int current_unit_for_cache(void)
{
    if (!s_active_unit_valid || s_active_unit < 0 || s_active_unit >= WEBGL2_MAX_TEX_UNITS)
        return -1;
    return s_active_unit;
}

/* Texture parameters are texture-object state; cache them per texture id. */
typedef struct {
    GLint v[5]; /* wrap S, wrap T, wrap R, min filter, mag filter; 0 = unknown */
    unsigned char known[5];
} WebGL2TexParams;

static WebGL2TexParams *s_texparams;
static unsigned int s_texparams_cap;

static WebGL2TexParams *texparams_for(GLuint id, int create)
{
    if (!id)
        return NULL;
    if (id >= s_texparams_cap) {
        unsigned int cap;
        WebGL2TexParams *next;

        if (!create)
            return NULL;
        if (id > (1u << 22))
            return NULL;
        cap = s_texparams_cap ? s_texparams_cap : 1024;
        while (cap <= id)
            cap *= 2;
        next = (WebGL2TexParams *)realloc(s_texparams, cap * sizeof(*next));
        if (!next)
            return NULL;
        memset(next + s_texparams_cap, 0, (cap - s_texparams_cap) * sizeof(*next));
        s_texparams = next;
        s_texparams_cap = cap;
    }
    return &s_texparams[id];
}

static int texparam_slot(GLenum pname)
{
    switch (pname) {
    case GL_TEXTURE_WRAP_S:
        return 0;
    case GL_TEXTURE_WRAP_T:
        return 1;
    case GL_TEXTURE_WRAP_R:
        return 2;
    case GL_TEXTURE_MIN_FILTER:
        return 3;
    case GL_TEXTURE_MAG_FILTER:
        return 4;
    default:
        return -1;
    }
}

static GLuint bound_texture_for(GLenum target, int *known)
{
    int t = tex_target_slot(target);
    int unit = current_unit_for_cache();

    *known = 0;
    if (t < 0 || unit < 0 || !s_bound_tex_valid[t][unit])
        return 0;
    *known = 1;
    return s_bound_tex[t][unit];
}

WEBGL2_WEAK void webgl2_glBindTexture(unsigned int target, unsigned int texture)
{
    int t = tex_target_slot(target);
    int unit = current_unit_for_cache();

    if (t >= 0 && unit >= 0 && s_bound_tex_valid[t][unit] && s_bound_tex[t][unit] == texture) {
        s_stats.texture_binds_skipped++;
        return;
    }
    glBindTexture(target, texture);
    s_stats.state_calls_issued++;
    if (t >= 0 && unit >= 0) {
        s_bound_tex[t][unit] = texture;
        s_bound_tex_valid[t][unit] = 1;
    } else if (t >= 0) {
        /* Unknown active unit: nothing reliable to record for any unit. */
        memset(s_bound_tex_valid[t], 0, sizeof(s_bound_tex_valid[t]));
    }
}

WEBGL2_WEAK void webgl2_glTexParameteri(unsigned int target, unsigned int pname, int param)
{
    int known;
    GLuint id = bound_texture_for(target, &known);
    int slot = texparam_slot(pname);
    WebGL2TexParams *tp = (known && slot >= 0) ? texparams_for(id, 1) : NULL;

    if (tp && tp->known[slot] && tp->v[slot] == param) {
        s_stats.texparam_calls_skipped++;
        return;
    }
    glTexParameteri(target, pname, param);
    s_stats.state_calls_issued++;
    if (tp) {
        tp->v[slot] = param;
        tp->known[slot] = 1;
    }
}

WEBGL2_WEAK void webgl2_glTexParameterf(unsigned int target, unsigned int pname, float param)
{
    int known;
    GLuint id = bound_texture_for(target, &known);
    int slot = texparam_slot(pname);
    WebGL2TexParams *tp = (known && slot >= 0) ? texparams_for(id, 0) : NULL;

    glTexParameterf(target, pname, param);
    s_stats.state_calls_issued++;
    if (tp)
        tp->known[slot] = 0;
}

WEBGL2_WEAK void webgl2_glDeleteTextures(int n, const unsigned int *textures)
{
    int i;
    int t;
    int u;

    if (!textures || n <= 0)
        return;
    glDeleteTextures(n, textures);
    for (i = 0; i < n; i++) {
        WebGL2TexParams *tp = texparams_for(textures[i], 0);
        if (tp)
            memset(tp, 0, sizeof(*tp));
        /* GL unbinds a deleted texture from every unit (binding becomes 0). */
        for (t = 0; t < 2; t++) {
            for (u = 0; u < WEBGL2_MAX_TEX_UNITS; u++) {
                if (s_bound_tex_valid[t][u] && s_bound_tex[t][u] == textures[i])
                    s_bound_tex[t][u] = 0;
            }
        }
    }
}

WEBGL2_WEAK void webgl2_glDepthFunc(unsigned int func)
{
    if (s_depth_func_valid && s_depth_func == func) {
        s_stats.state_calls_skipped++;
        return;
    }
    glDepthFunc(func);
    s_depth_func = func;
    s_depth_func_valid = 1;
    s_stats.state_calls_issued++;
}

WEBGL2_WEAK void webgl2_glDepthMask(unsigned char flag)
{
    int v = flag ? 1 : 0;

    if (s_depth_mask == v) {
        s_stats.state_calls_skipped++;
        return;
    }
    glDepthMask((GLboolean)v);
    s_depth_mask = v;
    s_stats.state_calls_issued++;
}

WEBGL2_WEAK void webgl2_glBlendFunc(unsigned int sfactor, unsigned int dfactor)
{
    if (s_blend_valid && s_blend_src == sfactor && s_blend_dst == dfactor) {
        s_stats.state_calls_skipped++;
        return;
    }
    glBlendFunc(sfactor, dfactor);
    s_blend_src = sfactor;
    s_blend_dst = dfactor;
    s_blend_valid = 1;
    s_stats.state_calls_issued++;
}

WEBGL2_WEAK void webgl2_glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    int v = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);

    if (s_color_mask == v) {
        s_stats.state_calls_skipped++;
        return;
    }
    glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE, b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
    s_color_mask = v;
    s_stats.state_calls_issued++;
}

/* ------------------------------------------------------------------------- */
/* Scratch buffers                                                            */
/* ------------------------------------------------------------------------- */

/*
 * The legacy renderer makes many tiny draw calls every frame. Converting its
 * client arrays to WebGL2 used to malloc/free a vertex array on EVERY draw,
 * which creates needless allocator churn (and WASM heap growth) during play.
 *
 * glBufferData uploads synchronously copy the converted vertices before this
 * scratch buffer is reused, so one growable CPU buffer is enough.
 */
static WebGL2Vertex *gl2_vertex_scratch;
static size_t gl2_vertex_scratch_capacity;
static unsigned char *gl2_index_scratch;
static size_t gl2_index_scratch_capacity;

static WebGL2Vertex *reserve_compat_vertices(size_t vertex_count)
{
    size_t capacity;
    WebGL2Vertex *next;

    if (!vertex_count || vertex_count > SIZE_MAX / sizeof(WebGL2Vertex))
        return NULL;
    if (vertex_count <= gl2_vertex_scratch_capacity)
        return gl2_vertex_scratch;

    capacity = gl2_vertex_scratch_capacity ? gl2_vertex_scratch_capacity : 256;
    while (capacity < vertex_count) {
        if (capacity > SIZE_MAX / 2) {
            capacity = vertex_count;
            break;
        }
        capacity *= 2;
    }
    next = (WebGL2Vertex *)realloc(gl2_vertex_scratch,
                                   capacity * sizeof(WebGL2Vertex));
    if (!next)
        return NULL;
    gl2_vertex_scratch = next;
    gl2_vertex_scratch_capacity = capacity;
    return next;
}

static void *reserve_index_bytes(size_t bytes)
{
    size_t capacity;
    unsigned char *next;

    if (!bytes)
        return NULL;
    if (bytes <= gl2_index_scratch_capacity)
        return gl2_index_scratch;
    capacity = gl2_index_scratch_capacity ? gl2_index_scratch_capacity : 4096;
    while (capacity < bytes) {
        if (capacity > SIZE_MAX / 2) {
            capacity = bytes;
            break;
        }
        capacity *= 2;
    }
    next = (unsigned char *)realloc(gl2_index_scratch, capacity);
    if (!next)
        return NULL;
    gl2_index_scratch = next;
    gl2_index_scratch_capacity = capacity;
    return next;
}

/* ------------------------------------------------------------------------- */
/* Matrices                                                                   */
/* ------------------------------------------------------------------------- */

static void mat_identity(float *m)
{
    memset(m, 0, sizeof(float) * 16);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void mat_mul(float *out, const float *a, const float *b)
{
    float r[16];
    int row;
    int col;

    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            r[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }

    memcpy(out, r, sizeof(r));
}

static float *current_matrix(void)
{
    if (gl2.matrix_mode == GL_PROJECTION)
        return gl2.projection;
    if (gl2.matrix_mode == GL_TEXTURE)
        return gl2.texture;
    return gl2.modelview;
}

static void mat_postmul_current(const float *rhs)
{
    float *cur = current_matrix();
    mat_mul(cur, cur, rhs);
}

static void gl2_init_state(void)
{
    static int initialized;

    if (initialized)
        return;

    initialized = 1;
    gl2.matrix_mode = GL_MODELVIEW;
    mat_identity(gl2.modelview);
    mat_identity(gl2.projection);
    mat_identity(gl2.texture);
    gl2.color[0] = 1.0f;
    gl2.color[1] = 1.0f;
    gl2.color[2] = 1.0f;
    gl2.color[3] = 1.0f;
    gl2.active_tex_unit = 0;
    gl2.client_tex_unit = 0;
    gl2.tex_env_mode[0] = GL_MODULATE;
    gl2.tex_env_mode[1] = GL_MODULATE;
    gl2.alpha_func = GL_ALWAYS;
    gl2.alpha_ref = 0.0f;
    gl2.fog_enable = 0;
    gl2.fog_color[0] = 0.0f;
    gl2.fog_color[1] = 0.0f;
    gl2.fog_color[2] = 0.0f;
    gl2.fog_color[3] = 1.0f;
    gl2.fog_start = 0.0f;
    gl2.fog_end = 4000.0f;
    gl2.fog_density = 0.0f;
    gl2.fog_mode = 3;
    webgl2_state_invalidate();
}

/* ------------------------------------------------------------------------- */
/* Shaders and uniforms                                                       */
/* ------------------------------------------------------------------------- */

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, sizeof(log), &len, log);
        fprintf(stderr, "webgl2 compat shader compile failed: %.*s\n", (int)len, log);
    }
    return shader;
}

static GLuint link_program(const char *vs_src, const char *fs_src, const char *label)
{
    GLuint vert;
    GLuint frag;
    GLuint program;
    GLint ok = 0;

    vert = compile_shader(GL_VERTEX_SHADER, vs_src);
    frag = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glBindAttribLocation(program, 0, "a_pos");
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        GLsizei len = 0;
        glGetProgramInfoLog(program, sizeof(log), &len, log);
        fprintf(stderr, "webgl2 compat %s link failed: %.*s [o1-gfx]\n", label, (int)len, log);
    }
    glDeleteShader(vert);
    glDeleteShader(frag);
    return program;
}

static void uniform_init(WebGL2UniformCache *u, GLuint program, const char *name)
{
    u->loc = glGetUniformLocation(program, name);
    u->valid = 0;
}

static void uniform_int(WebGL2UniformCache *u, int v)
{
    if (u->loc < 0)
        return;
    if (u->valid && u->iv == v) {
        s_stats.uniform_calls_skipped++;
        return;
    }
    glUniform1i(u->loc, v);
    u->iv = v;
    u->valid = 1;
    s_stats.uniform_calls_issued++;
}

static void uniform_float(WebGL2UniformCache *u, float v)
{
    if (u->loc < 0)
        return;
    if (u->valid && u->v[0] == v) {
        s_stats.uniform_calls_skipped++;
        return;
    }
    glUniform1f(u->loc, v);
    u->v[0] = v;
    u->valid = 1;
    s_stats.uniform_calls_issued++;
}

static void uniform_vec3(WebGL2UniformCache *u, float x, float y, float z)
{
    if (u->loc < 0)
        return;
    if (u->valid && u->v[0] == x && u->v[1] == y && u->v[2] == z) {
        s_stats.uniform_calls_skipped++;
        return;
    }
    glUniform3f(u->loc, x, y, z);
    u->v[0] = x;
    u->v[1] = y;
    u->v[2] = z;
    u->valid = 1;
    s_stats.uniform_calls_issued++;
}

static void uniform_mvp(WebGL2Program *p, const float *m)
{
    if (p->mvp_loc < 0)
        return;
    if (p->mvp_valid && !memcmp(p->mvp, m, sizeof(p->mvp))) {
        s_stats.uniform_calls_skipped++;
        return;
    }
    glUniformMatrix4fv(p->mvp_loc, 1, GL_FALSE, m);
    memcpy(p->mvp, m, sizeof(p->mvp));
    p->mvp_valid = 1;
    s_stats.uniform_calls_issued++;
}

/*
 * The vertex colour arrives either as converted floats (legacy path, RGBA)
 * or as normalised bytes straight from the Direct3D vertex (native path) in
 * one of three byte orders; u_color_order swizzles it back to RGBA.
 */
static const char *s_vs_2d =
    "#version 300 es\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec4 a_color;\n"
    "layout(location=2) in vec3 a_tex0;\n"
    "layout(location=3) in vec2 a_tex1;\n"
    "uniform mat4 u_mvp;\n"
    "uniform int u_color_order;\n"
    "out vec4 v_color;\n"
    "out vec2 v_tex0;\n"
    "out vec2 v_tex1;\n"
    "out float v_eye_z;\n"
    "vec4 swizzleColor(vec4 c) {\n"
    "  if (u_color_order == 1) return c.zyxw;\n"
    "  if (u_color_order == 2) return c.yzwx;\n"
    "  return c;\n"
    "}\n"
    "void main() {\n"
    "  vec4 eye = u_mvp * vec4(a_pos, 1.0);\n"
    "  gl_Position = eye;\n"
    "  v_eye_z = abs(eye.w) > 0.0001 ? abs(eye.w) : abs(eye.z);\n"
    "  v_color = swizzleColor(a_color);\n"
    "  v_tex0 = a_tex0.xy;\n"
    "  v_tex1 = a_tex1;\n"
    "}\n";

static const char *s_fs_2d =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 v_color;\n"
    "in vec2 v_tex0;\n"
    "in vec2 v_tex1;\n"
    "in float v_eye_z;\n"
    "uniform sampler2D u_tex0;\n"
    "uniform sampler2D u_tex1;\n"
    "uniform int u_use_tex0;\n"
    "uniform int u_use_tex1;\n"
    "uniform float u_lmap_scale;\n"
    "uniform int u_use_light;\n"
    "uniform vec3 u_light;\n"
    "uniform int u_alpha_test;\n"
    "uniform int u_alpha_func;\n"
    "uniform float u_alpha_ref;\n"
    "uniform int u_fog_enable;\n"
    "uniform vec3 u_fog_color;\n"
    "uniform float u_fog_start;\n"
    "uniform float u_fog_end;\n"
    "out vec4 fragColor;\n"
    "bool alphaPass(float a) {\n"
    "  if (u_alpha_func == 512) return false;\n"
    "  if (u_alpha_func == 513) return a < u_alpha_ref;\n"
    "  if (u_alpha_func == 514) return abs(a - u_alpha_ref) < 0.0001;\n"
    "  if (u_alpha_func == 515) return a <= u_alpha_ref;\n"
    "  if (u_alpha_func == 516) return a > u_alpha_ref;\n"
    "  if (u_alpha_func == 517) return abs(a - u_alpha_ref) >= 0.0001;\n"
    "  if (u_alpha_func == 518) return a >= u_alpha_ref;\n"
    "  return true;\n"
    "}\n"
    "void main() {\n"
    "  /* Opaque lightmapped world: colormap * lightmap * overbright. Vertex RGB is not\n"
    "   * multiplied (cod2-demo-viewer + TECHNIQUE_LIGHTMAP_*). Vertex alpha still used. */\n"
    "  vec4 c = (u_use_tex1 != 0) ? vec4(1.0, 1.0, 1.0, v_color.a) : v_color;\n"
    "  vec4 t0 = vec4(1.0);\n"
    "  if (u_use_tex0 != 0) {\n"
    "    t0 = texture(u_tex0, v_tex0);\n"
    "    c *= t0;\n"
    "  }\n"
    "  if (u_use_tex1 != 0) {\n"
    "    vec4 t1 = texture(u_tex1, v_tex1);\n"
    "    c.rgb *= t1.rgb * u_lmap_scale;\n"
    "  }\n"
    "  if (u_use_light != 0) {\n"
    "    c.rgb *= u_light;\n"
    "  }\n"
    "  if (u_alpha_test != 0 && !alphaPass(t0.a * v_color.a)) discard;\n"
    "  if (u_fog_enable != 0) {\n"
    "    float denom = u_fog_end - u_fog_start;\n"
    "    float f = (abs(denom) > 0.001) ? clamp((u_fog_end - v_eye_z) / denom, 0.0, 1.0) : 1.0;\n"
    "    c.rgb = mix(u_fog_color, c.rgb, f);\n"
    "  }\n"
    "  fragColor = c;\n"
    "}\n";

static const char *s_vs_cube =
    "#version 300 es\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec4 a_color;\n"
    "layout(location=2) in vec3 a_tex0;\n"
    "uniform mat4 u_mvp;\n"
    "uniform vec3 u_eye;\n"
    "uniform int u_color_order;\n"
    "out vec4 v_color;\n"
    "out vec3 v_dir;\n"
    "vec4 swizzleColor(vec4 c) {\n"
    "  if (u_color_order == 1) return c.zyxw;\n"
    "  if (u_color_order == 2) return c.yzwx;\n"
    "  return c;\n"
    "}\n"
    "void main() {\n"
    "  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "  v_color = swizzleColor(a_color);\n"
    "  v_dir = a_pos - u_eye;\n"
    "}\n";

static const char *s_fs_cube =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 v_color;\n"
    "in vec3 v_dir;\n"
    "uniform samplerCube u_cube;\n"
    "uniform int u_alpha_test;\n"
    "uniform int u_alpha_func;\n"
    "uniform float u_alpha_ref;\n"
    "out vec4 fragColor;\n"
    "bool alphaPass(float a) {\n"
    "  if (u_alpha_func == 512) return false;\n"
    "  if (u_alpha_func == 513) return a < u_alpha_ref;\n"
    "  if (u_alpha_func == 514) return abs(a - u_alpha_ref) < 0.0001;\n"
    "  if (u_alpha_func == 515) return a <= u_alpha_ref;\n"
    "  if (u_alpha_func == 516) return a > u_alpha_ref;\n"
    "  if (u_alpha_func == 517) return abs(a - u_alpha_ref) >= 0.0001;\n"
    "  if (u_alpha_func == 518) return a >= u_alpha_ref;\n"
    "  return true;\n"
    "}\n"
    "void main() {\n"
    "  vec4 c = v_color * texture(u_cube, normalize(v_dir));\n"
    "  if (u_alpha_test != 0 && !alphaPass(c.a)) discard;\n"
    "  fragColor = c;\n"
    "}\n";

static void program_init(WebGL2Program *p, const char *vs, const char *fs, const char *label)
{
    memset(p, 0, sizeof(*p));
    p->id = link_program(vs, fs, label);
    p->mvp_loc = glGetUniformLocation(p->id, "u_mvp");
    uniform_init(&p->use_tex0, p->id, "u_use_tex0");
    uniform_init(&p->use_tex1, p->id, "u_use_tex1");
    uniform_init(&p->lmap_scale, p->id, "u_lmap_scale");
    uniform_init(&p->use_light, p->id, "u_use_light");
    uniform_init(&p->light, p->id, "u_light");
    uniform_init(&p->alpha_test, p->id, "u_alpha_test");
    uniform_init(&p->alpha_func, p->id, "u_alpha_func");
    uniform_init(&p->alpha_ref, p->id, "u_alpha_ref");
    uniform_init(&p->fog_enable, p->id, "u_fog_enable");
    uniform_init(&p->fog_color, p->id, "u_fog_color");
    uniform_init(&p->fog_start, p->id, "u_fog_start");
    uniform_init(&p->fog_end, p->id, "u_fog_end");
    uniform_init(&p->color_order, p->id, "u_color_order");
    uniform_init(&p->eye, p->id, "u_eye");
}

static void ensure_program(void)
{
    gl2_init_state();
    if (gl2.p2d.id)
        return;

    program_init(&gl2.p2d, s_vs_2d, s_fs_2d, "2d");
    program_init(&gl2.pcube, s_vs_cube, s_fs_cube, "cube");

    use_program(gl2.p2d.id);
    glUniform1i(glGetUniformLocation(gl2.p2d.id, "u_tex0"), 0);
    glUniform1i(glGetUniformLocation(gl2.p2d.id, "u_tex1"), 1);
    use_program(gl2.pcube.id);
    glUniform1i(glGetUniformLocation(gl2.pcube.id, "u_cube"), 0);

    glGenBuffers(1, &gl2.vbo);
    glGenBuffers(1, &gl2.ibo);

    /*
     * Legacy layout: the converted-vertex buffer never changes identity, only
     * its contents (glBufferData), so the attribute pointers live in the VAO
     * once instead of being re-specified on every draw.
     */
    glGenVertexArrays(1, &gl2.vao);
    bind_vao(gl2.vao);
    bind_array_buffer(gl2.vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(WebGL2Vertex), (void *)offsetof(WebGL2Vertex, xyz));
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(WebGL2Vertex), (void *)offsetof(WebGL2Vertex, color));
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(WebGL2Vertex), (void *)offsetof(WebGL2Vertex, tex0));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(WebGL2Vertex), (void *)offsetof(WebGL2Vertex, tex1));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl2.ibo);

    glGenVertexArrays(1, &gl2.native_vao);
}

static void apply_uniforms(int use_tex0, int use_tex1, int use_cube, int color_order)
{
    float mvp[16];
    WebGL2Program *p;

    mat_mul(mvp, gl2.projection, gl2.modelview);
    if (use_cube) {
        const float *m = gl2.modelview;
        float eye[3];

        /* Camera world position from orthonormal modelview: -R^T * t */
        eye[0] = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]);
        eye[1] = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]);
        eye[2] = -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14]);
        p = &gl2.pcube;
        use_program(p->id);
        uniform_mvp(p, mvp);
        uniform_vec3(&p->eye, eye[0], eye[1], eye[2]);
        uniform_int(&p->alpha_test, gl2.alpha_test);
        uniform_int(&p->alpha_func, (int)gl2.alpha_func);
        uniform_float(&p->alpha_ref, gl2.alpha_ref);
        uniform_int(&p->color_order, color_order);
        return;
    }

    p = &gl2.p2d;
    use_program(p->id);
    uniform_mvp(p, mvp);
    uniform_int(&p->use_tex0, use_tex0);
    uniform_int(&p->use_tex1, use_tex1);
    uniform_float(&p->lmap_scale, use_tex1 ? 2.0f : 1.0f);
    {
        float lr = 0.0f, lg = 0.0f, lb = 0.0f;
        int li;
        int any = 0;
        int use_light;

        for (li = 0; li < 8; li++) {
            if (!gl2.light_enabled[li])
                continue;
            any = 1;
            lr += gl2.light_ambient[li][0];
            lg += gl2.light_ambient[li][1];
            lb += gl2.light_ambient[li][2];
        }
        use_light = (s_lighting_allowed && !use_tex1 && any && (lr + lg + lb) > 0.02f);
        uniform_int(&p->use_light, use_light);
        if (use_light)
            uniform_vec3(&p->light, lr * 2.0f, lg * 2.0f, lb * 2.0f);
    }
    uniform_int(&p->alpha_test, gl2.alpha_test);
    uniform_int(&p->alpha_func, (int)gl2.alpha_func);
    uniform_float(&p->alpha_ref, gl2.alpha_ref);
    uniform_int(&p->fog_enable, gl2.fog_enable);
    if (gl2.fog_enable) {
        uniform_vec3(&p->fog_color, gl2.fog_color[0], gl2.fog_color[1], gl2.fog_color[2]);
        uniform_float(&p->fog_start, gl2.fog_start);
        uniform_float(&p->fog_end, gl2.fog_end);
    }
    uniform_int(&p->color_order, color_order);
}

static void bind_compat_arrays(const WebGL2Vertex *verts, int vert_count, int use_tex0, int use_tex1, int use_cube)
{
    ensure_program();
    apply_uniforms(use_tex0, use_tex1, use_cube, WEBGL2_COLOR_RGBA);
    bind_vao(gl2.vao);
    bind_array_buffer(gl2.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vert_count * sizeof(WebGL2Vertex)), verts, GL_STREAM_DRAW);
    s_stats.legacy_bytes_uploaded += (double)vert_count * sizeof(WebGL2Vertex);
}

/* ------------------------------------------------------------------------- */
/* Client array conversion (legacy path)                                      */
/* ------------------------------------------------------------------------- */

static int component_size(GLenum type)
{
    switch (type) {
    case GL_BYTE:
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_SHORT:
    case GL_UNSIGNED_SHORT:
        return 2;
    case GL_INT:
    case GL_UNSIGNED_INT:
    case GL_FLOAT:
        return 4;
    default:
        return 4;
    }
}

static float read_component(const unsigned char *src, GLenum type, int normalized)
{
    switch (type) {
    case GL_BYTE:
        return normalized ? (*(const int8_t *)src / 127.0f) : (float)*(const int8_t *)src;
    case GL_UNSIGNED_BYTE:
        return normalized ? (*(const uint8_t *)src / 255.0f) : (float)*(const uint8_t *)src;
    case GL_SHORT:
        return normalized ? (*(const int16_t *)src / 32767.0f) : (float)*(const int16_t *)src;
    case GL_UNSIGNED_SHORT:
        return normalized ? (*(const uint16_t *)src / 65535.0f) : (float)*(const uint16_t *)src;
    case GL_INT:
        return (float)*(const int32_t *)src;
    case GL_UNSIGNED_INT:
        return (float)*(const uint32_t *)src;
    case GL_FLOAT:
    default:
        return *(const float *)src;
    }
}

static void array_set(WebGL2ClientArray *array, int size, GLenum type, int stride, const void *pointer, int normalized)
{
    array->bgra = size == GL_BGRA;
    array->size = array->bgra ? 4 : size;
    array->type = type;
    array->stride = stride ? stride : array->size * component_size(type);
    array->pointer = (const unsigned char *)pointer;
    array->normalized = normalized;
}

static void fill_vertex_from_arrays(WebGL2Vertex *dst, unsigned int index)
{
    int i;

    dst->xyz[0] = 0.0f;
    dst->xyz[1] = 0.0f;
    dst->xyz[2] = 0.0f;
    dst->color[0] = gl2.color[0];
    dst->color[1] = gl2.color[1];
    dst->color[2] = gl2.color[2];
    dst->color[3] = gl2.color[3];
    dst->tex0[0] = 0.0f;
    dst->tex0[1] = 0.0f;
    dst->tex0[2] = 0.0f;
    dst->tex1[0] = 0.0f;
    dst->tex1[1] = 0.0f;

    if (gl2.vertex_array.enabled && gl2.vertex_array.pointer) {
        const unsigned char *base = gl2.vertex_array.pointer + index * gl2.vertex_array.stride;
        int comps = gl2.vertex_array.size < 3 ? gl2.vertex_array.size : 3;
        if (gl2.vertex_array.type == GL_FLOAT) {
            memcpy(dst->xyz, base, (size_t)comps * sizeof(float));
        } else {
            for (i = 0; i < comps; i++)
                dst->xyz[i] = read_component(base + i * component_size(gl2.vertex_array.type), gl2.vertex_array.type, 0);
        }
    }

    if (gl2.color_array.enabled && gl2.color_array.pointer) {
        const unsigned char *base = gl2.color_array.pointer + index * gl2.color_array.stride;
        float c[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        int comps = gl2.color_array.size < 4 ? gl2.color_array.size : 4;
        for (i = 0; i < comps; i++)
            c[i] = read_component(base + i * component_size(gl2.color_array.type), gl2.color_array.type, gl2.color_array.normalized);
        if (gl2.color_array.bgra) {
            dst->color[0] = c[2];
            dst->color[1] = c[1];
            dst->color[2] = c[0];
            dst->color[3] = c[3];
        } else {
            memcpy(dst->color, c, sizeof(c));
        }
    }

    for (i = 0; i < 2; i++) {
        if (gl2.texcoord_array[i].enabled && gl2.texcoord_array[i].pointer) {
            const unsigned char *base = gl2.texcoord_array[i].pointer + index * gl2.texcoord_array[i].stride;
            float *tex = i == 0 ? dst->tex0 : dst->tex1;
            tex[0] = read_component(base, gl2.texcoord_array[i].type, 0);
            if (gl2.texcoord_array[i].size > 1)
                tex[1] = read_component(base + component_size(gl2.texcoord_array[i].type), gl2.texcoord_array[i].type, 0);
            if (i == 0 && gl2.texcoord_array[i].size > 2)
                dst->tex0[2] = read_component(base + 2 * component_size(gl2.texcoord_array[i].type), gl2.texcoord_array[i].type, 0);
        }
    }
    if (gl2.cube_map_enabled[0] && !gl2.texture_2d_enabled[0] && gl2.texcoord_array[0].size < 3) {
        dst->tex0[0] = dst->xyz[0];
        dst->tex0[1] = dst->xyz[1];
        dst->tex0[2] = dst->xyz[2];
    }
}

static unsigned int read_index_value(const void *indices, GLenum type, int i)
{
    const unsigned char *p = (const unsigned char *)indices;

    switch (type) {
    case GL_UNSIGNED_BYTE:
        return p[i];
    case GL_UNSIGNED_INT:
        return ((const uint32_t *)indices)[i];
    case GL_UNSIGNED_SHORT:
    default:
        return ((const uint16_t *)indices)[i];
    }
}

static int index_type_size(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_UNSIGNED_INT:
        return 4;
    case GL_UNSIGNED_SHORT:
    default:
        return 2;
    }
}

static uint32_t source_index_value(const void *indices, GLenum type, int i, int first)
{
    if (!indices)
        return (uint32_t)(first + i);
    return read_index_value(indices, type, i);
}

static uint32_t *build_legacy_triangle_indices(GLenum mode, int first, int count, GLenum type, const void *indices, int *out_count)
{
    uint32_t *out;
    int i;
    int n;

    *out_count = 0;
    if (mode == GL_QUADS) {
        int quad_count = count / 4;
        if (quad_count <= 0)
            return NULL;

        out = (uint32_t *)malloc((size_t)quad_count * 6 * sizeof(uint32_t));
        if (!out)
            return NULL;

        for (i = 0; i < quad_count; i++) {
            uint32_t a = source_index_value(indices, type, i * 4 + 0, first);
            uint32_t b = source_index_value(indices, type, i * 4 + 1, first);
            uint32_t c = source_index_value(indices, type, i * 4 + 2, first);
            uint32_t d = source_index_value(indices, type, i * 4 + 3, first);
            int o = i * 6;
            out[o + 0] = a;
            out[o + 1] = b;
            out[o + 2] = c;
            out[o + 3] = a;
            out[o + 4] = c;
            out[o + 5] = d;
        }
        *out_count = quad_count * 6;
        return out;
    }

    if (mode == GL_QUAD_STRIP) {
        int quad_count = (count - 2) / 2;
        if (quad_count <= 0)
            return NULL;

        out = (uint32_t *)malloc((size_t)quad_count * 6 * sizeof(uint32_t));
        if (!out)
            return NULL;

        for (i = 0; i < quad_count; i++) {
            uint32_t a = source_index_value(indices, type, i * 2 + 0, first);
            uint32_t b = source_index_value(indices, type, i * 2 + 1, first);
            uint32_t c = source_index_value(indices, type, i * 2 + 2, first);
            uint32_t d = source_index_value(indices, type, i * 2 + 3, first);
            int o = i * 6;
            out[o + 0] = a;
            out[o + 1] = b;
            out[o + 2] = c;
            out[o + 3] = b;
            out[o + 4] = d;
            out[o + 5] = c;
        }
        *out_count = quad_count * 6;
        return out;
    }

    if (mode == GL_POLYGON) {
        if (count <= 0)
            return NULL;
        out = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
        if (!out)
            return NULL;
        for (n = 0; n < count; n++)
            out[n] = source_index_value(indices, type, n, first);
        *out_count = count;
        return out;
    }

    return NULL;
}

static GLenum webgl_draw_mode(GLenum mode)
{
    if (mode == GL_QUADS || mode == GL_QUAD_STRIP)
        return GL_TRIANGLES;
    if (mode == GL_POLYGON)
        return GL_TRIANGLE_FAN;
    return mode;
}

int webgl2_rebase_indices16(const unsigned short *src, int count,
                            unsigned short *dst, unsigned int *out_min, unsigned int *out_max)
{
    unsigned int lo = 0xFFFFFFFFu;
    unsigned int hi = 0;
    int i;

    if (!src || count <= 0)
        return 0;
    for (i = 0; i < count; i++) {
        unsigned int v = src[i];
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
    }
    if (dst) {
        for (i = 0; i < count; i++)
            dst[i] = (unsigned short)(src[i] - lo);
    }
    if (out_min)
        *out_min = lo;
    if (out_max)
        *out_max = hi;
    return 1;
}

WEBGL2_WEAK void webgl2_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices)
{
    WebGL2Vertex *verts;
    uint32_t *legacy_indices;
    unsigned int max_index = 0;
    unsigned int min_index = 0xFFFFFFFFu;
    unsigned int vert_count;
    int i;
    int legacy_count = 0;
    int use_tex0;
    int use_tex1;
    int use_cube;

    gl2_init_state();
    if (!gl2.vertex_array.enabled || !gl2.vertex_array.pointer || !indices || count <= 0) {
        glDrawElements(webgl_draw_mode(mode), count, type, indices);
        return;
    }

    for (i = 0; i < count; i++) {
        unsigned int index = read_index_value(indices, type, i);
        if (index > max_index)
            max_index = index;
        if (index < min_index)
            min_index = index;
    }
    if (max_index > 1048576)
        return;

    /*
     * A wasm trap ("memory access out of bounds") loses the call site. Check
     * every client array's last touched byte against the linear memory size
     * and report the offender instead of trapping.
     */
    {
        uintptr_t heapSize = (uintptr_t)emscripten_get_heap_size();
        const WebGL2ClientArray *arrays[4];
        const char *names[4] = { "vertex", "color", "tex0", "tex1" };
        int a;
        arrays[0] = &gl2.vertex_array;
        arrays[1] = &gl2.color_array;
        arrays[2] = &gl2.texcoord_array[0];
        arrays[3] = &gl2.texcoord_array[1];
        for (a = 0; a < 4; a++) {
            const WebGL2ClientArray *ar = arrays[a];
            int enabled = (a == 0) ? gl2.vertex_array.enabled
                        : (a == 1) ? gl2.color_array.enabled
                                   : gl2.texcoord_array[a - 2].enabled;
            if (!enabled || !ar->pointer)
                continue;
            if ((uintptr_t)ar->pointer + (uintptr_t)max_index * (unsigned)ar->stride + 64 > heapSize) {
                static int oobReports;
                if (oobReports < 12) {
                    oobReports++;
                    printf("[o1-oob] %s array OOB: ptr=%p stride=%d maxIdx=%u end=0x%llx heap=0x%llx count=%d [o1-oob]\n",
                           names[a], (const void *)ar->pointer, ar->stride, max_index,
                           (unsigned long long)((uintptr_t)ar->pointer + (uintptr_t)max_index * (unsigned)ar->stride),
                           (unsigned long long)heapSize, count);
                }
                return;
            }
        }
        if ((uintptr_t)indices + (size_t)count * index_type_size(type) + 8 > heapSize) {
            static int oobIdxReports;
            if (oobIdxReports < 12) {
                oobIdxReports++;
                printf("[o1-oob] index array OOB: ptr=%p count=%d heap=0x%llx [o1-oob]\n",
                       indices, count, (unsigned long long)heapSize);
            }
            return;
        }
    }

    /*
     * Convert only the vertices the indices reference ([min, max]) and rebase
     * the indices; converting from vertex 0 wasted work whenever a draw used
     * the end of a larger buffer.
     */
    vert_count = max_index - min_index + 1;
    verts = reserve_compat_vertices((size_t)vert_count);
    if (!verts)
        return;
    for (i = 0; i < (int)vert_count; i++)
        fill_vertex_from_arrays(&verts[i], min_index + (unsigned int)i);

    use_tex0 = gl2.texture_2d_enabled[0] && gl2.texcoord_array[0].enabled;
    use_tex1 = gl2.texture_2d_enabled[1] && gl2.texcoord_array[1].enabled;
    use_cube = gl2.cube_map_enabled[0] && !gl2.texture_2d_enabled[0];
    bind_compat_arrays(verts, (int)vert_count, use_tex0, use_tex1, use_cube);

    legacy_indices = build_legacy_triangle_indices(mode, 0, count, type, indices, &legacy_count);
    if (legacy_indices) {
        for (i = 0; i < legacy_count; i++)
            legacy_indices[i] -= min_index;
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(legacy_count * sizeof(uint32_t)), legacy_indices, GL_STREAM_DRAW);
        glDrawElements(webgl_draw_mode(mode), legacy_count, GL_UNSIGNED_INT, 0);
        s_stats.legacy_bytes_uploaded += (double)legacy_count * sizeof(uint32_t);
        free(legacy_indices);
    } else if (vert_count <= 65536u) {
        unsigned short *idx16 = (unsigned short *)reserve_index_bytes((size_t)count * 2u);
        if (!idx16)
            return;
        for (i = 0; i < count; i++)
            idx16[i] = (unsigned short)(read_index_value(indices, type, i) - min_index);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * 2, idx16, GL_STREAM_DRAW);
        glDrawElements(webgl_draw_mode(mode), count, GL_UNSIGNED_SHORT, 0);
        s_stats.legacy_bytes_uploaded += (double)count * 2.0;
    } else {
        uint32_t *idx32 = (uint32_t *)reserve_index_bytes((size_t)count * 4u);
        if (!idx32)
            return;
        for (i = 0; i < count; i++)
            idx32[i] = read_index_value(indices, type, i) - min_index;
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * 4, idx32, GL_STREAM_DRAW);
        glDrawElements(webgl_draw_mode(mode), count, GL_UNSIGNED_INT, 0);
        s_stats.legacy_bytes_uploaded += (double)count * 4.0;
    }
    s_stats.legacy_draws++;
}

WEBGL2_WEAK void webgl2_glDrawArrays(unsigned int mode, int first, int count)
{
    WebGL2Vertex *verts;
    uint32_t *legacy_indices;
    int i;
    int legacy_count = 0;
    int use_tex0;
    int use_tex1;
    int use_cube;

    gl2_init_state();
    if (!gl2.vertex_array.enabled || !gl2.vertex_array.pointer || count <= 0) {
        glDrawArrays(webgl_draw_mode(mode), first, count);
        return;
    }

    verts = reserve_compat_vertices((size_t)count);
    if (!verts)
        return;

    for (i = 0; i < count; i++)
        fill_vertex_from_arrays(&verts[i], (unsigned int)(first + i));

    use_tex0 = gl2.texture_2d_enabled[0] && gl2.texcoord_array[0].enabled;
    use_tex1 = gl2.texture_2d_enabled[1] && gl2.texcoord_array[1].enabled;
    use_cube = gl2.cube_map_enabled[0] && !gl2.texture_2d_enabled[0];
    bind_compat_arrays(verts, count, use_tex0, use_tex1, use_cube);
    legacy_indices = build_legacy_triangle_indices(mode, 0, count, GL_UNSIGNED_INT, NULL, &legacy_count);
    if (legacy_indices) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(legacy_count * sizeof(uint32_t)), legacy_indices, GL_STREAM_DRAW);
        glDrawElements(webgl_draw_mode(mode), legacy_count, GL_UNSIGNED_INT, 0);
        free(legacy_indices);
    } else {
        glDrawArrays(webgl_draw_mode(mode), 0, count);
    }
    s_stats.legacy_draws++;
}

WEBGL2_WEAK void webgl2_glDrawRangeElements(unsigned int mode, unsigned int start, unsigned int end, int count, unsigned int type, const void *indices)
{
    (void)start;
    (void)end;
    webgl2_glDrawElements(mode, count, type, indices);
}

/* ------------------------------------------------------------------------- */
/* Native path: GL mirrors of Direct3D vertex/index buffer memory            */
/* ------------------------------------------------------------------------- */

typedef struct {
    const unsigned char *base;  /* key part 1 (NULL = empty slot) */
    GLenum target;              /* key part 2 */
    GLuint buf;
    unsigned int size;
    unsigned int dirty_lo;
    unsigned int dirty_hi;      /* dirty range [lo, hi); lo >= hi means clean */
} WebGL2Mirror;

static WebGL2Mirror *s_mirrors;
static unsigned int s_mirror_cap;   /* power of two */
static unsigned int s_mirror_count;

static unsigned int mirror_hash(const void *base, GLenum target)
{
    uintptr_t p = (uintptr_t)base;
    uint32_t h = (uint32_t)(p >> 3) ^ (uint32_t)(p >> 17) ^ ((uint32_t)target * 0x9E3779B1u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

static void mirror_insert_raw(WebGL2Mirror *table, unsigned int cap, const WebGL2Mirror *m)
{
    unsigned int i = mirror_hash(m->base, m->target) & (cap - 1);

    while (table[i].base)
        i = (i + 1) & (cap - 1);
    table[i] = *m;
}

static int mirror_grow(void)
{
    unsigned int cap = s_mirror_cap ? s_mirror_cap * 2 : 1024;
    WebGL2Mirror *next = (WebGL2Mirror *)calloc(cap, sizeof(*next));
    unsigned int i;

    if (!next)
        return 0;
    for (i = 0; i < s_mirror_cap; i++) {
        if (s_mirrors[i].base)
            mirror_insert_raw(next, cap, &s_mirrors[i]);
    }
    free(s_mirrors);
    s_mirrors = next;
    s_mirror_cap = cap;
    return 1;
}

static WebGL2Mirror *mirror_find(const void *base, GLenum target)
{
    unsigned int i;

    if (!s_mirror_cap || !base)
        return NULL;
    i = mirror_hash(base, target) & (s_mirror_cap - 1);
    while (s_mirrors[i].base) {
        if (s_mirrors[i].base == base && s_mirrors[i].target == target)
            return &s_mirrors[i];
        i = (i + 1) & (s_mirror_cap - 1);
    }
    return NULL;
}

static void mirror_remove_at(unsigned int i)
{
    unsigned int j;

    /* Backward-shift deletion keeps linear probing tombstone-free. */
    s_mirrors[i].base = NULL;
    s_mirror_count--;
    j = (i + 1) & (s_mirror_cap - 1);
    while (s_mirrors[j].base) {
        WebGL2Mirror m = s_mirrors[j];
        s_mirrors[j].base = NULL;
        mirror_insert_raw(s_mirrors, s_mirror_cap, &m);
        j = (j + 1) & (s_mirror_cap - 1);
    }
}

static void mirror_mark_dirty(WebGL2Mirror *m, unsigned int offset, unsigned int size)
{
    unsigned int end;

    if (offset >= m->size)
        return;
    if (!size || size > m->size - offset)
        size = m->size - offset;
    end = offset + size;
    if (m->dirty_lo >= m->dirty_hi) {
        m->dirty_lo = offset;
        m->dirty_hi = end;
    } else {
        if (offset < m->dirty_lo)
            m->dirty_lo = offset;
        if (end > m->dirty_hi)
            m->dirty_hi = end;
    }
}

void webgl2_buffer_written(const void *base, unsigned int offset, unsigned int size)
{
    WebGL2Mirror *m;

    m = mirror_find(base, GL_ARRAY_BUFFER);
    if (m)
        mirror_mark_dirty(m, offset, size);
    m = mirror_find(base, GL_ELEMENT_ARRAY_BUFFER);
    if (m)
        mirror_mark_dirty(m, offset, size);
}

void webgl2_buffer_released(const void *base)
{
    int pass;

    for (pass = 0; pass < 2; pass++) {
        GLenum target = pass ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
        WebGL2Mirror *m = mirror_find(base, target);
        if (!m)
            continue;
        if (m->buf) {
            if (target == GL_ARRAY_BUFFER && s_array_buffer_valid && s_array_buffer == m->buf)
                s_array_buffer_valid = 0;
            glDeleteBuffers(1, &m->buf);
        }
        mirror_remove_at((unsigned int)(m - s_mirrors));
        if (s_stats.mirror_buffers)
            s_stats.mirror_buffers--;
    }
}

/* Element-array binding is VAO state: callers bind the native VAO first. */
static void bind_mirror_buffer(GLenum target, GLuint buf)
{
    static GLuint s_native_ebo;
    static int s_native_ebo_valid;

    if (target == GL_ARRAY_BUFFER) {
        bind_array_buffer(buf);
        return;
    }
    if (s_native_ebo_valid && s_native_ebo == buf) {
        s_stats.state_calls_skipped++;
        return;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf);
    s_native_ebo = buf;
    s_native_ebo_valid = 1;
    s_stats.state_calls_issued++;
}

static GLuint mirror_prepare(const void *base, unsigned int size, GLenum target, int force_lo_valid,
                             unsigned int force_lo, unsigned int force_hi)
{
    WebGL2Mirror *m = mirror_find(base, target);
    double *counter = (target == GL_ARRAY_BUFFER) ? &s_stats.vertex_bytes_uploaded
                                                  : &s_stats.index_bytes_uploaded;

    if (m && m->size != size) {
        /* Same address, different allocation size: treat as a new buffer. */
        webgl2_buffer_released(base);
        m = NULL;
    }
    if (!m) {
        WebGL2Mirror fresh;

        if ((s_mirror_count + 1) * 2 > s_mirror_cap && !mirror_grow())
            return 0;
        memset(&fresh, 0, sizeof(fresh));
        fresh.base = (const unsigned char *)base;
        fresh.target = target;
        fresh.size = size;
        glGenBuffers(1, &fresh.buf);
        if (!fresh.buf)
            return 0;
        bind_mirror_buffer(target, fresh.buf);
        glBufferData(target, (GLsizeiptr)size, base, GL_DYNAMIC_DRAW);
        *counter += size;
        s_stats.mirror_full_uploads++;
        s_stats.mirror_buffers++;
        mirror_insert_raw(s_mirrors, s_mirror_cap, &fresh);
        s_mirror_count++;
        return fresh.buf;
    }

    if (force_lo_valid)
        mirror_mark_dirty(m, force_lo, force_hi > force_lo ? force_hi - force_lo : 0);
    bind_mirror_buffer(target, m->buf);
    if (m->dirty_lo < m->dirty_hi) {
        unsigned int lo = m->dirty_lo;
        unsigned int len = m->dirty_hi - m->dirty_lo;

        glBufferSubData(target, (GLintptr)lo, (GLsizeiptr)len, (const unsigned char *)base + lo);
        *counter += len;
        s_stats.mirror_partial_uploads++;
        m->dirty_lo = m->dirty_hi = 0;
    }
    return m->buf;
}

typedef struct {
    int known;
    int enabled;
    GLuint buf;
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    uintptr_t offset;
    int const_known;
    float const_v[4];
} WebGL2AttribCache;

static WebGL2AttribCache s_native_attr[4];

static void native_attrib_pointer(GLuint index, GLuint buf, GLint size, GLenum type, GLboolean normalized,
                                  GLsizei stride, uintptr_t offset)
{
    WebGL2AttribCache *a = &s_native_attr[index];

    if (!a->known || !a->enabled) {
        glEnableVertexAttribArray(index);
        a->enabled = 1;
        s_stats.state_calls_issued++;
    }
    if (a->known && a->buf == buf && a->size == size && a->type == type &&
        a->normalized == normalized && a->stride == stride && a->offset == offset) {
        s_stats.state_calls_skipped++;
        return;
    }
    glVertexAttribPointer(index, size, type, normalized, stride, (const void *)offset);
    a->known = 1;
    a->buf = buf;
    a->size = size;
    a->type = type;
    a->normalized = normalized;
    a->stride = stride;
    a->offset = offset;
    s_stats.state_calls_issued++;
}

static void native_attrib_constant(GLuint index, float x, float y, float z, float w)
{
    WebGL2AttribCache *a = &s_native_attr[index];

    if (!a->known || a->enabled) {
        glDisableVertexAttribArray(index);
        a->enabled = 0;
        a->known = 1;
        a->buf = 0;
        s_stats.state_calls_issued++;
    }
    /* Constant attribute values are context state, not VAO state. */
    if (a->const_known && a->const_v[0] == x && a->const_v[1] == y && a->const_v[2] == z && a->const_v[3] == w) {
        s_stats.state_calls_skipped++;
        return;
    }
    glVertexAttrib4f(index, x, y, z, w);
    a->const_known = 1;
    a->const_v[0] = x;
    a->const_v[1] = y;
    a->const_v[2] = z;
    a->const_v[3] = w;
    s_stats.state_calls_issued++;
}

static int native_reject(const char *why, const WebGL2NativeDraw *d)
{
    static int reports;

    s_stats.native_rejects++;
    if (reports < 8) {
        reports++;
        printf("[gl-native] fallback to legacy path: %s (stride=%d vb=%u ib=%u base=%u idx=%u+%d)\n",
               why, d ? d->stride : -1, d ? d->vb_size : 0u, d ? d->ib_size : 0u,
               d ? d->base_offset : 0u, d ? d->index_offset : 0u, d ? d->index_count : -1);
    }
    return 0;
}

int webgl2_draw_native(const WebGL2NativeDraw *d)
{
    GLuint vbuf;
    GLuint ibuf;
    uintptr_t base;
    int use_tex0;
    int use_tex1;
    int use_cube;
    unsigned int used_lo;
    unsigned int used_hi;

    if (!webgl2_native_enabled())
        return 0;
    if (!d || !d->vb_base || !d->ib_base || d->index_count <= 0 || d->stride <= 0)
        return native_reject("incomplete draw", d);
    if ((d->stride & 3) || (d->base_offset & 3) || (d->pos_offset & 3) ||
        (d->tex0_offset > 0 && (d->tex0_offset & 3)) || (d->tex1_offset > 0 && (d->tex1_offset & 3)))
        return native_reject("unaligned layout", d);
    if (d->index_offset & 1u)
        return native_reject("unaligned indices", d);
    if ((unsigned long long)d->index_offset + (unsigned long long)d->index_count * 2ull > d->ib_size)
        return native_reject("index range past buffer", d);
    if (d->base_offset >= d->vb_size)
        return native_reject("base vertex past buffer", d);
    used_lo = d->base_offset + d->min_vertex * (unsigned int)d->stride;
    {
        unsigned long long hi = (unsigned long long)d->base_offset +
                                ((unsigned long long)d->min_vertex + d->num_vertices) * (unsigned long long)d->stride;
        if (hi > (unsigned long long)d->vb_size + (unsigned long long)d->stride)
            return native_reject("vertex range past buffer", d);
        used_hi = hi > d->vb_size ? d->vb_size : (unsigned int)hi;
    }

    gl2_init_state();
    ensure_program();
    bind_vao(gl2.native_vao);

    ibuf = mirror_prepare(d->ib_base, d->ib_size, GL_ELEMENT_ARRAY_BUFFER, s_env_reupload,
                          d->index_offset, d->index_offset + (unsigned int)d->index_count * 2u);
    vbuf = mirror_prepare(d->vb_base, d->vb_size, GL_ARRAY_BUFFER, s_env_reupload, used_lo, used_hi);
    if (!ibuf || !vbuf)
        return native_reject("mirror allocation failed", d);
    bind_array_buffer(vbuf);

    base = (uintptr_t)d->base_offset;
    use_cube = gl2.cube_map_enabled[0] && !gl2.texture_2d_enabled[0];
    use_tex0 = gl2.texture_2d_enabled[0] && d->tex0_offset >= 0;
    use_tex1 = gl2.texture_2d_enabled[1] && d->tex1_offset >= 0;

    native_attrib_pointer(0, vbuf, 3, GL_FLOAT, GL_FALSE, d->stride, base + (uintptr_t)d->pos_offset);
    if (d->color_offset >= 0)
        native_attrib_pointer(1, vbuf, 4, GL_UNSIGNED_BYTE, GL_TRUE, d->stride, base + (uintptr_t)d->color_offset);
    else
        native_attrib_constant(1, gl2.color[0], gl2.color[1], gl2.color[2], gl2.color[3]);
    if (!use_cube && d->tex0_offset >= 0)
        native_attrib_pointer(2, vbuf, 2, GL_FLOAT, GL_FALSE, d->stride, base + (uintptr_t)d->tex0_offset);
    else
        native_attrib_constant(2, 0.0f, 0.0f, 0.0f, 1.0f);
    if (d->tex1_offset >= 0)
        native_attrib_pointer(3, vbuf, 2, GL_FLOAT, GL_FALSE, d->stride, base + (uintptr_t)d->tex1_offset);
    else
        native_attrib_constant(3, 0.0f, 0.0f, 0.0f, 1.0f);

    apply_uniforms(use_tex0, use_tex1, use_cube,
                   d->color_offset >= 0 ? d->color_order : WEBGL2_COLOR_RGBA);
    glDrawElements(GL_TRIANGLES, d->index_count, GL_UNSIGNED_SHORT, (const void *)(uintptr_t)d->index_offset);
    s_stats.native_draws++;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Statistics                                                                 */
/* ------------------------------------------------------------------------- */

void webgl2_get_stats(WebGL2Stats *out)
{
    if (out)
        *out = s_stats;
}

void webgl2_reset_stats(void)
{
    unsigned int buffers = s_stats.mirror_buffers;

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.mirror_buffers = buffers;
}

void webgl2_frame_end(void)
{
    static WebGL2Stats last;
    static unsigned int last_frame;

    load_env();
    s_stats.frames++;
    webgl2_state_invalidate();
    if (s_env_stats && s_stats.frames - last_frame >= 300) {
        double frames = (double)(s_stats.frames - last_frame);
        printf("[gl-stats] per frame: native=%.0f legacy=%.0f rejects=%.1f | upload KB: vtx=%.1f idx=%.1f legacy=%.1f | "
               "GL calls issued=%.0f skipped=%.0f | uniforms issued=%.0f skipped=%.0f | texbind skipped=%.0f texparam skipped=%.0f | "
               "mirrors=%u full=%u partial/frame=%.0f\n",
               (s_stats.native_draws - last.native_draws) / frames,
               (s_stats.legacy_draws - last.legacy_draws) / frames,
               (s_stats.native_rejects - last.native_rejects) / frames,
               (s_stats.vertex_bytes_uploaded - last.vertex_bytes_uploaded) / frames / 1024.0,
               (s_stats.index_bytes_uploaded - last.index_bytes_uploaded) / frames / 1024.0,
               (s_stats.legacy_bytes_uploaded - last.legacy_bytes_uploaded) / frames / 1024.0,
               (s_stats.state_calls_issued - last.state_calls_issued) / frames,
               (s_stats.state_calls_skipped - last.state_calls_skipped) / frames,
               (s_stats.uniform_calls_issued - last.uniform_calls_issued) / frames,
               (s_stats.uniform_calls_skipped - last.uniform_calls_skipped) / frames,
               (s_stats.texture_binds_skipped - last.texture_binds_skipped) / frames,
               (s_stats.texparam_calls_skipped - last.texparam_calls_skipped) / frames,
               s_stats.mirror_buffers, s_stats.mirror_full_uploads,
               (s_stats.mirror_partial_uploads - last.mirror_partial_uploads) / frames);
        last = s_stats;
        last_frame = s_stats.frames;
    }
}

/* Console / test harness: Module.ccall('cod2_gl_stats_json', 'string'). */
EMSCRIPTEN_KEEPALIVE const char *cod2_gl_stats_json(void)
{
    static char buf[768];

    snprintf(buf, sizeof(buf),
             "{\"frames\":%u,\"nativeDraws\":%u,\"legacyDraws\":%u,\"nativeRejects\":%u,"
             "\"mirrorBuffers\":%u,\"mirrorFullUploads\":%u,\"mirrorPartialUploads\":%u,"
             "\"vertexBytes\":%.0f,\"indexBytes\":%.0f,\"legacyBytes\":%.0f,"
             "\"stateIssued\":%u,\"stateSkipped\":%u,\"uniformIssued\":%u,\"uniformSkipped\":%u,"
             "\"texBindSkipped\":%u,\"texParamSkipped\":%u,\"nativeEnabled\":%d}",
             s_stats.frames, s_stats.native_draws, s_stats.legacy_draws, s_stats.native_rejects,
             s_stats.mirror_buffers, s_stats.mirror_full_uploads, s_stats.mirror_partial_uploads,
             s_stats.vertex_bytes_uploaded, s_stats.index_bytes_uploaded, s_stats.legacy_bytes_uploaded,
             s_stats.state_calls_issued, s_stats.state_calls_skipped,
             s_stats.uniform_calls_issued, s_stats.uniform_calls_skipped,
             s_stats.texture_binds_skipped, s_stats.texparam_calls_skipped,
             webgl2_native_enabled());
    return buf;
}

void webgl2_set_lighting_allowed(int allowed)
{
    s_lighting_allowed = allowed ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Fixed-function emulation entry points                                      */
/* ------------------------------------------------------------------------- */

WEBGL2_WEAK void webgl2_glEnable(unsigned int cap)
{
    gl2_init_state();
    if (cap == GL_TEXTURE_2D) {
        gl2.texture_2d_enabled[gl2.active_tex_unit] = 1;
        gl2.cube_map_enabled[gl2.active_tex_unit] = 0;
        return;
    }
    if (cap == GL_TEXTURE_CUBE_MAP) {
        gl2.cube_map_enabled[gl2.active_tex_unit] = 1;
        gl2.texture_2d_enabled[gl2.active_tex_unit] = 0;
        return;
    }
    if (cap == GL_ALPHA_TEST) {
        gl2.alpha_test = 1;
        return;
    }
    if (cap == 0x0B60) {
        gl2.fog_enable = 1;
        return;
    }
    if (cap == 0x0B50 || cap == 0x0B57 || cap == 0x0B90)
        return;
    cached_cap(cap, 1);
}

WEBGL2_WEAK void webgl2_glDisable(unsigned int cap)
{
    gl2_init_state();
    if (cap == GL_TEXTURE_2D) {
        gl2.texture_2d_enabled[gl2.active_tex_unit] = 0;
        return;
    }
    if (cap == GL_TEXTURE_CUBE_MAP) {
        gl2.cube_map_enabled[gl2.active_tex_unit] = 0;
        return;
    }
    if (cap == GL_ALPHA_TEST) {
        gl2.alpha_test = 0;
        return;
    }
    if (cap == 0x0B60) {
        gl2.fog_enable = 0;
        return;
    }
    if (cap == 0x0B50 || cap == 0x0B57 || cap == 0x0B90)
        return;
    cached_cap(cap, 0);
}

WEBGL2_WEAK void glActiveTextureARB(unsigned int texture)
{
    gl2_init_state();
    if (texture >= GL_TEXTURE0 && texture <= GL_TEXTURE1)
        gl2.active_tex_unit = (int)(texture - GL_TEXTURE0);
    if (texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + WEBGL2_MAX_TEX_UNITS) {
        set_active_unit((int)(texture - GL_TEXTURE0));
    } else {
        glActiveTexture(texture);
        s_active_unit_valid = 0;
    }
}

WEBGL2_WEAK void glClientActiveTextureARB(unsigned int texture)
{
    gl2_init_state();
    if (texture >= GL_TEXTURE0 && texture <= GL_TEXTURE1)
        gl2.client_tex_unit = (int)(texture - GL_TEXTURE0);
}

WEBGL2_WEAK void glEnableClientState(unsigned int array)
{
    gl2_init_state();
    if (array == GL_VERTEX_ARRAY)
        gl2.vertex_array.enabled = 1;
    else if (array == GL_COLOR_ARRAY)
        gl2.color_array.enabled = 1;
    else if (array == GL_TEXTURE_COORD_ARRAY)
        gl2.texcoord_array[gl2.client_tex_unit].enabled = 1;
    else if (array == GL_NORMAL_ARRAY)
        gl2.normal_array.enabled = 1;
}

WEBGL2_WEAK void glDisableClientState(unsigned int array)
{
    gl2_init_state();
    if (array == GL_VERTEX_ARRAY)
        gl2.vertex_array.enabled = 0;
    else if (array == GL_COLOR_ARRAY)
        gl2.color_array.enabled = 0;
    else if (array == GL_TEXTURE_COORD_ARRAY)
        gl2.texcoord_array[gl2.client_tex_unit].enabled = 0;
    else if (array == GL_NORMAL_ARRAY)
        gl2.normal_array.enabled = 0;
}

WEBGL2_WEAK void glVertexPointer(int size, unsigned int type, int stride, const void *pointer)
{
    gl2_init_state();
    array_set(&gl2.vertex_array, size, type, stride, pointer, 0);
}

WEBGL2_WEAK void glColorPointer(int size, unsigned int type, int stride, const void *pointer)
{
    gl2_init_state();
    array_set(&gl2.color_array, size, type, stride, pointer, type != GL_FLOAT);
}

WEBGL2_WEAK void glTexCoordPointer(int size, unsigned int type, int stride, const void *pointer)
{
    gl2_init_state();
    array_set(&gl2.texcoord_array[gl2.client_tex_unit], size, type, stride, pointer, 0);
}

WEBGL2_WEAK int glNormalPointer(unsigned int type, int stride, const void *pointer)
{
    gl2_init_state();
    array_set(&gl2.normal_array, 3, type, stride, pointer, 0);
    return 0;
}

WEBGL2_WEAK void glBegin(unsigned int mode)
{
    gl2_init_state();
    gl2.in_begin = 1;
    gl2.begin_mode = mode;
    gl2.immediate_count = 0;
}

static void push_immediate_vertex(float x, float y, float z)
{
    WebGL2Vertex *v;

    if (!gl2.in_begin)
        return;
    if (gl2.immediate_count == gl2.immediate_capacity) {
        int new_capacity = gl2.immediate_capacity ? gl2.immediate_capacity * 2 : 256;
        WebGL2Vertex *new_data = (WebGL2Vertex *)realloc(gl2.immediate, (size_t)new_capacity * sizeof(WebGL2Vertex));
        if (!new_data)
            return;
        gl2.immediate = new_data;
        gl2.immediate_capacity = new_capacity;
    }

    v = &gl2.immediate[gl2.immediate_count++];
    v->xyz[0] = x;
    v->xyz[1] = y;
    v->xyz[2] = z;
    memcpy(v->color, gl2.color, sizeof(v->color));
    v->tex0[0] = gl2.texcoord[0][0];
    v->tex0[1] = gl2.texcoord[0][1];
    v->tex0[2] = gl2.texcoord[0][2];
    v->tex1[0] = gl2.texcoord[1][0];
    v->tex1[1] = gl2.texcoord[1][1];
}

WEBGL2_WEAK void glEnd(void)
{
    uint32_t *legacy_indices;
    int legacy_count = 0;
    int use_tex0;
    int use_tex1;
    int use_cube;

    gl2_init_state();
    if (!gl2.in_begin)
        return;

    gl2.in_begin = 0;
    if (gl2.immediate_count <= 0)
        return;

    use_tex0 = gl2.texture_2d_enabled[0];
    use_tex1 = gl2.texture_2d_enabled[1];
    use_cube = gl2.cube_map_enabled[0] && !gl2.texture_2d_enabled[0];
    bind_compat_arrays(gl2.immediate, gl2.immediate_count, use_tex0, use_tex1, use_cube);
    legacy_indices = build_legacy_triangle_indices(gl2.begin_mode, 0, gl2.immediate_count, GL_UNSIGNED_INT, NULL, &legacy_count);
    if (legacy_indices) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(legacy_count * sizeof(uint32_t)), legacy_indices, GL_STREAM_DRAW);
        glDrawElements(webgl_draw_mode(gl2.begin_mode), legacy_count, GL_UNSIGNED_INT, 0);
        free(legacy_indices);
    } else {
        glDrawArrays(webgl_draw_mode(gl2.begin_mode), 0, gl2.immediate_count);
    }
    s_stats.legacy_draws++;
}

WEBGL2_WEAK void glVertex2f(float x, float y)
{
    push_immediate_vertex(x, y, 0.0f);
}

WEBGL2_WEAK void glVertex3f(float x, float y, float z)
{
    push_immediate_vertex(x, y, z);
}

WEBGL2_WEAK void glColor4f(float r, float g, float b, float a)
{
    gl2_init_state();
    gl2.color[0] = r;
    gl2.color[1] = g;
    gl2.color[2] = b;
    gl2.color[3] = a;
}

WEBGL2_WEAK void glTexCoord2f(float s, float t)
{
    gl2_init_state();
    gl2.texcoord[gl2.active_tex_unit][0] = s;
    gl2.texcoord[gl2.active_tex_unit][1] = t;
}

WEBGL2_WEAK void glMatrixMode(unsigned int mode)
{
    gl2_init_state();
    /*
     * Desktop GL has a real TEXTURE matrix stack. CDirect3DDevice switches
     * to GL_TEXTURE and glLoadIdentity() before every indexed draw (and
     * again when binding a lightmap). If TEXTURE is aliased onto
     * MODELVIEW, that identity wipe drops world-space BSP verts off screen
     * while viewmodel verts near the origin fill the frame.
     */
    if (mode == GL_MODELVIEW || mode == GL_PROJECTION || mode == GL_TEXTURE)
        gl2.matrix_mode = mode;
}

WEBGL2_WEAK void glLoadIdentity(void)
{
    gl2_init_state();
    mat_identity(current_matrix());
}

WEBGL2_WEAK void glLoadMatrixf(const float *m)
{
    gl2_init_state();
    if (m)
        memcpy(current_matrix(), m, sizeof(float) * 16);
}

WEBGL2_WEAK void glPushMatrix(void)
{
    gl2_init_state();
    if (gl2.matrix_mode == GL_TEXTURE)
        return;
    if (gl2.matrix_mode == GL_PROJECTION) {
        if (gl2.projection_depth < 32)
            memcpy(gl2.projection_stack[gl2.projection_depth++], gl2.projection, sizeof(gl2.projection));
    } else {
        if (gl2.modelview_depth < 32)
            memcpy(gl2.modelview_stack[gl2.modelview_depth++], gl2.modelview, sizeof(gl2.modelview));
    }
}

WEBGL2_WEAK void glPopMatrix(void)
{
    gl2_init_state();
    if (gl2.matrix_mode == GL_TEXTURE)
        return;
    if (gl2.matrix_mode == GL_PROJECTION) {
        if (gl2.projection_depth > 0)
            memcpy(gl2.projection, gl2.projection_stack[--gl2.projection_depth], sizeof(gl2.projection));
    } else {
        if (gl2.modelview_depth > 0)
            memcpy(gl2.modelview, gl2.modelview_stack[--gl2.modelview_depth], sizeof(gl2.modelview));
    }
}

WEBGL2_WEAK void glScalef(float x, float y, float z)
{
    float s[16];

    gl2_init_state();
    mat_identity(s);
    s[0] = x;
    s[5] = y;
    s[10] = z;
    mat_postmul_current(s);
}

WEBGL2_WEAK void glOrtho(double left, double right, double bottom, double top, double zNear, double zFar)
{
    float o[16];
    float rl = (float)(right - left);
    float tb = (float)(top - bottom);
    float fn = (float)(zFar - zNear);

    gl2_init_state();
    mat_identity(o);
    if (rl == 0.0f || tb == 0.0f || fn == 0.0f)
        return;
    o[0] = 2.0f / rl;
    o[5] = 2.0f / tb;
    o[10] = -2.0f / fn;
    o[12] = -(float)(right + left) / rl;
    o[13] = -(float)(top + bottom) / tb;
    o[14] = -(float)(zFar + zNear) / fn;
    mat_postmul_current(o);
}

WEBGL2_WEAK void glAlphaFunc(unsigned int func, float ref)
{
    gl2_init_state();
    gl2.alpha_func = func;
    gl2.alpha_ref = ref;
}

WEBGL2_WEAK void glTexEnvi(unsigned int target, unsigned int pname, int param)
{
    gl2_init_state();
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE)
        gl2.tex_env_mode[gl2.active_tex_unit] = param;
}

WEBGL2_WEAK void glTexEnvfv(unsigned int target, unsigned int pname, const float *params)
{
    if (params)
        glTexEnvi(target, pname, (int)params[0]);
}

WEBGL2_WEAK void glTexEnvf(unsigned int target, unsigned int pname, float param)
{
    glTexEnvi(target, pname, (int)param);
}

WEBGL2_WEAK void glClearDepth(double depth) { glClearDepthf((float)depth); }
WEBGL2_WEAK void glDepthRange(double zNear, double zFar) { glDepthRangef((float)zNear, (float)zFar); }
WEBGL2_WEAK int glBlendEquationEXT(unsigned int mode) { glBlendEquation(mode); return 0; }
WEBGL2_WEAK int glBlendFuncSeparateEXT(unsigned int srcRGB, unsigned int dstRGB, unsigned int srcAlpha, unsigned int dstAlpha)
{
    glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
    s_blend_valid = 0;
    return 0;
}
WEBGL2_WEAK int glCompressedTexImage2DARB(unsigned int target, int level, unsigned int internalformat, int width, int height, int border, int imageSize, const void *data)
{
    glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data);
    return 0;
}
WEBGL2_WEAK int glCompressedTexImage3DARB(unsigned int target, int level, unsigned int internalformat, int width, int height, int depth, int border, int imageSize, const void *data)
{
    glCompressedTexImage3D(target, level, internalformat, width, height, depth, border, imageSize, data);
    return 0;
}
WEBGL2_WEAK int glVertexAttribPointerARB(unsigned int index, int size, unsigned int type, unsigned char normalized, int stride, const void *pointer)
{
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
    return 0;
}
WEBGL2_WEAK int glEnableVertexAttribArrayARB(unsigned int index) { glEnableVertexAttribArray(index); return 0; }
WEBGL2_WEAK int glDisableVertexAttribArrayARB(unsigned int index) { glDisableVertexAttribArray(index); return 0; }
WEBGL2_WEAK int glBindVertexArrayAPPLE(unsigned int array) { bind_vao(array); return 0; }
WEBGL2_WEAK int glGenVertexArraysAPPLE(int n, unsigned int *arrays) { glGenVertexArrays(n, arrays); return 0; }
WEBGL2_WEAK int glDeleteVertexArraysAPPLE(int n, const unsigned int *arrays)
{
    glDeleteVertexArrays(n, arrays);
    s_vao_valid = 0;
    return 0;
}

WEBGL2_WEAK void glBindProgramARB(unsigned int target, unsigned int program) { (void)target; (void)program; }
WEBGL2_WEAK int glColorMaterial(unsigned int face, unsigned int mode) { (void)face; (void)mode; return 0; }
WEBGL2_WEAK int glCombinerParameterfvNV(unsigned int pname, const float *params) { (void)pname; (void)params; return 0; }
WEBGL2_WEAK int glCombinerStageParameterfvNV(unsigned int stage, unsigned int pname, const float *params) { (void)stage; (void)pname; (void)params; return 0; }
WEBGL2_WEAK int glDeleteFencesAPPLE(int n, const unsigned int *fences) { (void)n; (void)fences; return 0; }
WEBGL2_WEAK int glDeleteProgramsARB(int n, const unsigned int *programs) { (void)n; (void)programs; return 0; }
WEBGL2_WEAK int glDrawBuffer(unsigned int mode) { (void)mode; return 0; }
WEBGL2_WEAK int glFinishFenceAPPLE(unsigned int fence) { (void)fence; return 0; }
WEBGL2_WEAK int glFlushVertexArrayRangeAPPLE(int length, const void *pointer) { (void)length; (void)pointer; return 0; }
WEBGL2_WEAK void glFogf(unsigned int pname, float param)
{
    gl2_init_state();
    if (pname == 0x0B62) /* GL_FOG_DENSITY */
        gl2.fog_density = param;
    else if (pname == 0x0B63) /* GL_FOG_START */
        gl2.fog_start = param;
    else if (pname == 0x0B64) /* GL_FOG_END */
        gl2.fog_end = param;
}
WEBGL2_WEAK void glFogfv(unsigned int pname, const float *params)
{
    gl2_init_state();
    if (!params)
        return;
    if (pname == 0x0B66) { /* GL_FOG_COLOR */
        gl2.fog_color[0] = params[0];
        gl2.fog_color[1] = params[1];
        gl2.fog_color[2] = params[2];
        gl2.fog_color[3] = params[3];
    }
}
WEBGL2_WEAK int glFogi(unsigned int pname, int param)
{
    gl2_init_state();
    if (pname == 0x0B65) /* GL_FOG_MODE */
        gl2.fog_mode = param;
    return 0;
}
WEBGL2_WEAK int glGenFencesAPPLE(int n, unsigned int *fences)
{
    int i;
    if (fences) {
        for (i = 0; i < n; i++)
            fences[i] = (unsigned int)(i + 1);
    }
    return 0;
}
WEBGL2_WEAK int glGenProgramsARB(int n, unsigned int *programs)
{
    int i;
    if (programs) {
        for (i = 0; i < n; i++)
            programs[i] = (unsigned int)(i + 1);
    }
    return 0;
}
WEBGL2_WEAK int glGetProgramivARB(unsigned int target, unsigned int pname, int *params)
{
    (void)target;
    (void)pname;
    if (params)
        *params = 1;
    return 0;
}
WEBGL2_WEAK int glLightModelfv(unsigned int pname, const float *params) { (void)pname; (void)params; return 0; }
WEBGL2_WEAK int glLightModeli(unsigned int pname, int param) { (void)pname; (void)param; return 0; }
WEBGL2_WEAK int glLightf(unsigned int light, unsigned int pname, float param) { (void)light; (void)pname; (void)param; return 0; }
WEBGL2_WEAK int glLightfv(unsigned int light, unsigned int pname, const float *params) { (void)light; (void)pname; (void)params; return 0; }
WEBGL2_WEAK int glMaterialf(unsigned int face, unsigned int pname, float param) { (void)face; (void)pname; (void)param; return 0; }
WEBGL2_WEAK int glMaterialfv(unsigned int face, unsigned int pname, const float *params) { (void)face; (void)pname; (void)params; return 0; }
WEBGL2_WEAK int glPointParameterfARB(unsigned int pname, float param) { (void)pname; (void)param; return 0; }
WEBGL2_WEAK int glPointParameterfvARB(unsigned int pname, const float *params) { (void)pname; (void)params; return 0; }
WEBGL2_WEAK int glPolygonMode(unsigned int face, unsigned int mode) { (void)face; (void)mode; return 0; }
WEBGL2_WEAK int glPopAttrib(void) { return 0; }
WEBGL2_WEAK int glPopClientAttrib(void) { return 0; }
WEBGL2_WEAK void glProgramEnvParameter4fvARB(unsigned int target, unsigned int index, const float *params) { (void)target; (void)index; (void)params; }
WEBGL2_WEAK int glProgramStringARB(unsigned int target, unsigned int format, int len, const void *string) { (void)target; (void)format; (void)len; (void)string; return 0; }
WEBGL2_WEAK int glPushAttrib(unsigned int mask) { (void)mask; return 0; }
WEBGL2_WEAK int glPushClientAttrib(unsigned int mask) { (void)mask; return 0; }
WEBGL2_WEAK int glSetFenceAPPLE(unsigned int fence) { (void)fence; return 0; }
WEBGL2_WEAK int glShadeModel(unsigned int mode) { (void)mode; return 0; }
WEBGL2_WEAK int glTestFenceAPPLE(unsigned int fence) { (void)fence; return 1; }
WEBGL2_WEAK int glTexGenfv(unsigned int coord, unsigned int pname, const float *params) { (void)coord; (void)pname; (void)params; return 0; }
WEBGL2_WEAK int glTexGeni(unsigned int coord, unsigned int pname, int param) { (void)coord; (void)pname; (void)param; return 0; }
WEBGL2_WEAK int glVertexArrayParameteriAPPLE(unsigned int pname, int param) { (void)pname; (void)param; return 0; }
WEBGL2_WEAK int glVertexArrayRangeAPPLE(int length, const void *pointer) { (void)length; (void)pointer; return 0; }

void webgl2_set_ff_light(int index, int enable, const float *ambient_rgb)
{
    gl2_init_state();
    if (index < 0 || index >= 8)
        return;
    if (enable >= 0)
        gl2.light_enabled[index] = enable;
    if (ambient_rgb) {
        gl2.light_ambient[index][0] = ambient_rgb[0];
        gl2.light_ambient[index][1] = ambient_rgb[1];
        gl2.light_ambient[index][2] = ambient_rgb[2];
    }
}

#endif
