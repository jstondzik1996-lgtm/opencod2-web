#ifdef __EMSCRIPTEN__

#include <GLES3/gl3.h>
#include <emscripten.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct {
    GLuint program;
    GLuint program_cube;
    GLuint vbo;
    GLuint ibo;
    GLuint vao;
    GLint u_mvp;
    GLint u_use_tex0;
    GLint u_use_tex1;
    GLint u_alpha_test;
    GLint u_alpha_func;
    GLint u_alpha_ref;
    GLint u_texmat;
    GLint u_lmap_scale;
    GLint u_use_light;
    GLint u_light;
    GLint u_fog_enable;
    GLint u_fog_color;
    GLint u_fog_start;
    GLint u_fog_end;
    GLint u_cube_mvp;
    GLint u_cube_eye;
    GLint u_cube_alpha_test;
    GLint u_cube_alpha_func;
    GLint u_cube_alpha_ref;
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

/*
 * The legacy renderer makes many tiny draw calls every frame. Converting its
 * client arrays to WebGL2 used to malloc/free a vertex array on EVERY draw,
 * which creates needless allocator churn (and WASM heap growth) during play.
 *
 * glBufferData uploads synchronously copy the converted vertices before this
 * scratch buffer is reused, so one growable CPU buffer is enough. No GL object
 * or public API changes are required. Index arrays remain independently owned.
 */
static WebGL2Vertex *gl2_vertex_scratch;
static size_t gl2_vertex_scratch_capacity;

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
}

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

static void ensure_program(void)
{
    static const char *vs =
        "#version 300 es\n"
        "layout(location=0) in vec3 a_pos;\n"
        "layout(location=1) in vec4 a_color;\n"
        "layout(location=2) in vec3 a_tex0;\n"
        "layout(location=3) in vec2 a_tex1;\n"
        "uniform mat4 u_mvp;\n"
        "out vec4 v_color;\n"
        "out vec2 v_tex0;\n"
        "out vec2 v_tex1;\n"
        "out float v_eye_z;\n"
        "void main() {\n"
        "  vec4 eye = u_mvp * vec4(a_pos, 1.0);\n"
        "  gl_Position = eye;\n"
        "  v_eye_z = abs(eye.w) > 0.0001 ? abs(eye.w) : abs(eye.z);\n"
        "  v_color = a_color;\n"
        "  v_tex0 = a_tex0.xy;\n"
        "  v_tex1 = a_tex1;\n"
        "}\n";
    static const char *fs =
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
    static const char *vs_cube =
        "#version 300 es\n"
        "layout(location=0) in vec3 a_pos;\n"
        "layout(location=1) in vec4 a_color;\n"
        "layout(location=2) in vec3 a_tex0;\n"
        "uniform mat4 u_mvp;\n"
        "uniform vec3 u_eye;\n"
        "out vec4 v_color;\n"
        "out vec3 v_dir;\n"
        "void main() {\n"
        "  gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
        "  v_color = a_color;\n"
        "  v_dir = a_pos - u_eye;\n"
        "}\n";
    static const char *fs_cube =
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

    gl2_init_state();
    if (gl2.program)
        return;

    gl2.program = link_program(vs, fs, "2d");
    gl2.program_cube = link_program(vs_cube, fs_cube, "cube");

    gl2.u_mvp = glGetUniformLocation(gl2.program, "u_mvp");
    gl2.u_texmat = glGetUniformLocation(gl2.program, "u_texmat");
    gl2.u_use_tex0 = glGetUniformLocation(gl2.program, "u_use_tex0");
    gl2.u_use_tex1 = glGetUniformLocation(gl2.program, "u_use_tex1");
    gl2.u_lmap_scale = glGetUniformLocation(gl2.program, "u_lmap_scale");
    gl2.u_use_light = glGetUniformLocation(gl2.program, "u_use_light");
    gl2.u_light = glGetUniformLocation(gl2.program, "u_light");
    gl2.u_alpha_test = glGetUniformLocation(gl2.program, "u_alpha_test");
    gl2.u_alpha_func = glGetUniformLocation(gl2.program, "u_alpha_func");
    gl2.u_alpha_ref = glGetUniformLocation(gl2.program, "u_alpha_ref");
    gl2.u_fog_enable = glGetUniformLocation(gl2.program, "u_fog_enable");
    gl2.u_fog_color = glGetUniformLocation(gl2.program, "u_fog_color");
    gl2.u_fog_start = glGetUniformLocation(gl2.program, "u_fog_start");
    gl2.u_fog_end = glGetUniformLocation(gl2.program, "u_fog_end");

    gl2.u_cube_mvp = glGetUniformLocation(gl2.program_cube, "u_mvp");
    gl2.u_cube_eye = glGetUniformLocation(gl2.program_cube, "u_eye");
    gl2.u_cube_alpha_test = glGetUniformLocation(gl2.program_cube, "u_alpha_test");
    gl2.u_cube_alpha_func = glGetUniformLocation(gl2.program_cube, "u_alpha_func");
    gl2.u_cube_alpha_ref = glGetUniformLocation(gl2.program_cube, "u_alpha_ref");

    glGenVertexArrays(1, &gl2.vao);
    glGenBuffers(1, &gl2.vbo);
    glGenBuffers(1, &gl2.ibo);

    glUseProgram(gl2.program);
    glUniform1i(glGetUniformLocation(gl2.program, "u_tex0"), 0);
    glUniform1i(glGetUniformLocation(gl2.program, "u_tex1"), 1);

    glUseProgram(gl2.program_cube);
    glUniform1i(glGetUniformLocation(gl2.program_cube, "u_cube"), 0);
}

static void bind_compat_arrays(const WebGL2Vertex *verts, int vert_count, int use_tex0, int use_tex1, int use_cube)
{
    float mvp[16];

    ensure_program();

    mat_mul(mvp, gl2.projection, gl2.modelview);
    if (use_cube) {
        const float *m = gl2.modelview;
        float eye[3];
        /* Camera world position from orthonormal modelview: -R^T * t */
        eye[0] = -(m[0] * m[12] + m[1] * m[13] + m[2] * m[14]);
        eye[1] = -(m[4] * m[12] + m[5] * m[13] + m[6] * m[14]);
        eye[2] = -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14]);
        glUseProgram(gl2.program_cube);
        glUniformMatrix4fv(gl2.u_cube_mvp, 1, GL_FALSE, mvp);
        if (gl2.u_cube_eye >= 0)
            glUniform3fv(gl2.u_cube_eye, 1, eye);
        glUniform1i(gl2.u_cube_alpha_test, gl2.alpha_test);
        glUniform1i(gl2.u_cube_alpha_func, gl2.alpha_func);
        glUniform1f(gl2.u_cube_alpha_ref, gl2.alpha_ref);
    } else {
        glUseProgram(gl2.program);
        glUniformMatrix4fv(gl2.u_mvp, 1, GL_FALSE, mvp);
        if (gl2.u_texmat >= 0)
            glUniformMatrix4fv(gl2.u_texmat, 1, GL_FALSE, gl2.texture);
        glUniform1i(gl2.u_use_tex0, use_tex0);
        glUniform1i(gl2.u_use_tex1, use_tex1);
        if (gl2.u_lmap_scale >= 0)
            glUniform1f(gl2.u_lmap_scale, use_tex1 ? 2.0f : 1.0f);
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
            use_light = (!use_tex1 && any && (lr + lg + lb) > 0.02f);
            if (gl2.u_use_light >= 0)
                glUniform1i(gl2.u_use_light, use_light);
            if (gl2.u_light >= 0)
                glUniform3f(gl2.u_light, lr * 2.0f, lg * 2.0f, lb * 2.0f);
        }
        glUniform1i(gl2.u_alpha_test, gl2.alpha_test);
        glUniform1i(gl2.u_alpha_func, gl2.alpha_func);
        glUniform1f(gl2.u_alpha_ref, gl2.alpha_ref);
        if (gl2.u_fog_enable >= 0)
            glUniform1i(gl2.u_fog_enable, gl2.fog_enable);
        if (gl2.u_fog_color >= 0)
            glUniform3f(gl2.u_fog_color, gl2.fog_color[0], gl2.fog_color[1], gl2.fog_color[2]);
        if (gl2.u_fog_start >= 0)
            glUniform1f(gl2.u_fog_start, gl2.fog_start);
        if (gl2.u_fog_end >= 0)
            glUniform1f(gl2.u_fog_end, gl2.fog_end);
    }

    glBindVertexArray(gl2.vao);
    glBindBuffer(GL_ARRAY_BUFFER, gl2.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vert_count * sizeof(WebGL2Vertex)), verts, GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(WebGL2Vertex), (void *)offsetof(WebGL2Vertex, xyz));
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(WebGL2Vertex), (void *)offsetof(WebGL2Vertex, color));
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(WebGL2Vertex), (void *)offsetof(WebGL2Vertex, tex0));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(WebGL2Vertex), (void *)offsetof(WebGL2Vertex, tex1));
}

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
        for (i = 0; i < comps; i++)
            dst->xyz[i] = read_component(base + i * component_size(gl2.vertex_array.type), gl2.vertex_array.type, 0);
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

WEBGL2_WEAK void webgl2_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices)
{
    WebGL2Vertex *verts;
    void *index_copy;
    uint32_t *legacy_indices;
    unsigned int max_index = 0;
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

    verts = reserve_compat_vertices((size_t)max_index + 1);
    legacy_indices = build_legacy_triangle_indices(mode, 0, count, type, indices, &legacy_count);
    index_copy = legacy_indices ? (void *)legacy_indices : malloc((size_t)count * index_type_size(type));
    if (!verts || !index_copy) {
        free(index_copy);
        return;
    }

    for (i = 0; i <= (int)max_index; i++)
        fill_vertex_from_arrays(&verts[i], (unsigned int)i);
    if (!legacy_indices)
        memcpy(index_copy, indices, (size_t)count * index_type_size(type));

    use_tex0 = gl2.texture_2d_enabled[0] && gl2.texcoord_array[0].enabled;
    use_tex1 = gl2.texture_2d_enabled[1] && gl2.texcoord_array[1].enabled;
    use_cube = gl2.cube_map_enabled[0] && !gl2.texture_2d_enabled[0];
    bind_compat_arrays(verts, (int)max_index + 1, use_tex0, use_tex1, use_cube);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl2.ibo);
    if (legacy_indices) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(legacy_count * sizeof(uint32_t)), index_copy, GL_STREAM_DRAW);
        glDrawElements(webgl_draw_mode(mode), legacy_count, GL_UNSIGNED_INT, 0);
    } else {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(count * index_type_size(type)), index_copy, GL_STREAM_DRAW);
        glDrawElements(webgl_draw_mode(mode), count, type, 0);
    }

    free(index_copy);
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
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl2.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(legacy_count * sizeof(uint32_t)), legacy_indices, GL_STREAM_DRAW);
        glDrawElements(webgl_draw_mode(mode), legacy_count, GL_UNSIGNED_INT, 0);
        free(legacy_indices);
    } else {
        glDrawArrays(webgl_draw_mode(mode), 0, count);
    }

}

WEBGL2_WEAK void webgl2_glDrawRangeElements(unsigned int mode, unsigned int start, unsigned int end, int count, unsigned int type, const void *indices)
{
    (void)start;
    (void)end;
    webgl2_glDrawElements(mode, count, type, indices);
}

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
    glEnable(cap);
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
    glDisable(cap);
}

WEBGL2_WEAK void glActiveTextureARB(unsigned int texture)
{
    gl2_init_state();
    if (texture >= GL_TEXTURE0 && texture <= GL_TEXTURE1)
        gl2.active_tex_unit = (int)(texture - GL_TEXTURE0);
    glActiveTexture(texture);
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
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl2.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(legacy_count * sizeof(uint32_t)), legacy_indices, GL_STREAM_DRAW);
        glDrawElements(webgl_draw_mode(gl2.begin_mode), legacy_count, GL_UNSIGNED_INT, 0);
        free(legacy_indices);
    } else {
        glDrawArrays(webgl_draw_mode(gl2.begin_mode), 0, gl2.immediate_count);
    }
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
WEBGL2_WEAK int glBindVertexArrayAPPLE(unsigned int array) { glBindVertexArray(array); return 0; }
WEBGL2_WEAK int glGenVertexArraysAPPLE(int n, unsigned int *arrays) { glGenVertexArrays(n, arrays); return 0; }
WEBGL2_WEAK int glDeleteVertexArraysAPPLE(int n, const unsigned int *arrays) { glDeleteVertexArrays(n, arrays); return 0; }

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
