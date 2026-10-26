#ifndef TEST_FAKE_GL_H
#define TEST_FAKE_GL_H

#include <GLES3/gl3.h>

#define FAKE_MAX_BUFFERS 4096
#define FAKE_MAX_VAOS 16
#define FAKE_MAX_UNIFORMS 128
#define FAKE_MAX_FETCH 65536

typedef struct {
    int alive;
    GLenum target;
    unsigned char *data;
    size_t size;
} FakeBuffer;

typedef struct {
    int enabled;
    GLuint buffer;
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    uintptr_t offset;
} FakeAttr;

typedef struct {
    FakeAttr attr[4];
    GLuint ebo;
} FakeVAO;

typedef struct {
    GLuint program;
    char name[32];
    int i;
    float f[16];
} FakeUniform;

typedef struct {
    float pos[3];
    float color[4];
    float tex0[2];
    float tex1[2];
    int use_tex0;
    int use_tex1;
    int use_light;
} FakeVertex;

typedef struct {
    int enable;
    int disable;
    int use_program;
    int bind_vao;
    int bind_buffer;
    int buffer_data;
    int buffer_sub_data;
    int delete_buffer;
    int active_texture;
    int bind_texture;
    int tex_parameter;
    int depth_func;
    int depth_mask;
    int blend_func;
    int color_mask;
    int uniform;
    int attrib_enable;
    int attrib_pointer;
    int draw;
} FakeCalls;

typedef struct {
    FakeBuffer buffers[FAKE_MAX_BUFFERS];
    GLuint buffer_count;
    FakeVAO vaos[FAKE_MAX_VAOS];
    GLuint vao_count;
    GLuint cur_vao;
    GLuint array_buffer;
    GLuint cur_program;
    GLuint program_count;
    GLuint shader_count;
    FakeUniform uniforms[FAKE_MAX_UNIFORMS];
    int uniform_count;
    float const_attr[4][4];
    int active_unit;
    GLuint bound_tex[16];
    int depth_test;
    int saw_swizzle_shader;
    FakeCalls calls;
    double bytes_uploaded;
    int errors;
    FakeVertex fetched[FAKE_MAX_FETCH];
    int fetched_count;
} FakeGL;

extern FakeGL fgl;

void fake_gl_reset_counters(void);
int fake_gl_uniform_int(const char *name);

#endif
