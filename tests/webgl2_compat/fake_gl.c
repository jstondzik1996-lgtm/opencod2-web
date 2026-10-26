/*
 * Fake WebGL2 context for tests/webgl2_compat.
 *
 * It keeps the state the compat layer manipulates (buffers with real byte
 * storage, VAOs with attribute pointers and element bindings, programs with
 * uniforms, texture bindings) and "executes" glDrawElements/glDrawArrays the
 * way the GPU front-end would: it fetches every referenced vertex's
 * attributes from the bound buffers (float or normalised unsigned byte) and
 * applies the vertex shader's colour swizzle. Tests compare these fetched
 * vertices between the legacy CPU-conversion path and the native path.
 */
#include "fake_gl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FakeGL fgl;

size_t emscripten_get_heap_size(void)
{
    /* Host pointers are 64-bit; report "everything in bounds". */
    return (size_t)-1 / 2;
}

static FakeVAO *cur_vao(void)
{
    return &fgl.vaos[fgl.cur_vao];
}

void fake_gl_reset_counters(void)
{
    memset(&fgl.calls, 0, sizeof(fgl.calls));
}

void glEnable(GLenum cap)
{
    fgl.calls.enable++;
    if (cap == GL_DEPTH_TEST)
        fgl.depth_test = 1;
}

void glDisable(GLenum cap)
{
    fgl.calls.disable++;
    if (cap == GL_DEPTH_TEST)
        fgl.depth_test = 0;
}

void glUseProgram(GLuint program)
{
    fgl.calls.use_program++;
    fgl.cur_program = program;
}

void glGenVertexArrays(GLsizei n, GLuint *arrays)
{
    GLsizei i;
    for (i = 0; i < n; i++) {
        arrays[i] = ++fgl.vao_count;
        memset(&fgl.vaos[arrays[i]], 0, sizeof(FakeVAO));
    }
}

void glDeleteVertexArrays(GLsizei n, const GLuint *arrays) { (void)n; (void)arrays; }

void glBindVertexArray(GLuint array)
{
    fgl.calls.bind_vao++;
    fgl.cur_vao = array;
}

void glGenBuffers(GLsizei n, GLuint *buffers)
{
    GLsizei i;
    for (i = 0; i < n; i++) {
        buffers[i] = ++fgl.buffer_count;
        fgl.buffers[buffers[i]].alive = 1;
    }
}

void glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
    GLsizei i;
    for (i = 0; i < n; i++) {
        FakeBuffer *b = &fgl.buffers[buffers[i]];
        free(b->data);
        memset(b, 0, sizeof(*b));
        fgl.calls.delete_buffer++;
    }
}

void glBindBuffer(GLenum target, GLuint buffer)
{
    fgl.calls.bind_buffer++;
    if (target == GL_ARRAY_BUFFER) {
        fgl.array_buffer = buffer;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        cur_vao()->ebo = buffer;
    }
    if (buffer) {
        FakeBuffer *b = &fgl.buffers[buffer];
        if (b->target && b->target != target)
            fgl.errors++; /* WebGL forbids rebinding a buffer to the other target */
        b->target = target;
    }
}

static GLuint bound_buffer(GLenum target)
{
    return target == GL_ARRAY_BUFFER ? fgl.array_buffer : cur_vao()->ebo;
}

void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    FakeBuffer *b = &fgl.buffers[bound_buffer(target)];
    (void)usage;
    if (!bound_buffer(target)) {
        fgl.errors++;
        return;
    }
    free(b->data);
    b->data = (unsigned char *)calloc(1, (size_t)size ? (size_t)size : 1);
    b->size = (size_t)size;
    if (data)
        memcpy(b->data, data, (size_t)size);
    fgl.calls.buffer_data++;
    fgl.bytes_uploaded += (double)size;
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    FakeBuffer *b = &fgl.buffers[bound_buffer(target)];
    if (!bound_buffer(target) || (size_t)(offset + size) > b->size) {
        fgl.errors++;
        return;
    }
    memcpy(b->data + offset, data, (size_t)size);
    fgl.calls.buffer_sub_data++;
    fgl.bytes_uploaded += (double)size;
}

void glActiveTexture(GLenum texture)
{
    fgl.calls.active_texture++;
    fgl.active_unit = (int)(texture - GL_TEXTURE0);
}

void glBindTexture(GLenum target, GLuint texture)
{
    (void)target;
    fgl.calls.bind_texture++;
    fgl.bound_tex[fgl.active_unit & 15] = texture;
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    (void)target; (void)pname; (void)param;
    fgl.calls.tex_parameter++;
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    (void)target; (void)pname; (void)param;
    fgl.calls.tex_parameter++;
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    GLsizei i;
    int u;
    for (i = 0; i < n; i++)
        for (u = 0; u < 16; u++)
            if (fgl.bound_tex[u] == textures[i])
                fgl.bound_tex[u] = 0;
}

void glDepthFunc(GLenum func) { (void)func; fgl.calls.depth_func++; }
void glDepthMask(GLboolean flag) { (void)flag; fgl.calls.depth_mask++; }
void glBlendFunc(GLenum s, GLenum d) { (void)s; (void)d; fgl.calls.blend_func++; }
void glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { (void)a; (void)b; (void)c; (void)d; }
void glBlendEquation(GLenum mode) { (void)mode; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)r; (void)g; (void)b; (void)a; fgl.calls.color_mask++; }
void glClearDepthf(GLfloat d) { (void)d; }
void glDepthRangef(GLfloat n, GLfloat f) { (void)n; (void)f; }

GLuint glCreateShader(GLenum type) { (void)type; return ++fgl.shader_count; }
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
{
    (void)shader; (void)count; (void)length;
    /* Remember whether the program uses the colour swizzle (sanity check). */
    if (string && string[0] && strstr(string[0], "swizzleColor"))
        fgl.saw_swizzle_shader = 1;
}
void glCompileShader(GLuint shader) { (void)shader; }
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) { (void)shader; (void)pname; *params = 1; }
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
    (void)shader; (void)bufSize; (void)infoLog;
    if (length)
        *length = 0;
}
void glDeleteShader(GLuint shader) { (void)shader; }
GLuint glCreateProgram(void) { return ++fgl.program_count; }
void glAttachShader(GLuint program, GLuint shader) { (void)program; (void)shader; }
void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) { (void)program; (void)index; (void)name; }
void glLinkProgram(GLuint program) { (void)program; }
void glGetProgramiv(GLuint program, GLenum pname, GLint *params) { (void)program; (void)pname; *params = 1; }
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
    (void)program; (void)bufSize; (void)infoLog;
    if (length)
        *length = 0;
}

GLint glGetUniformLocation(GLuint program, const GLchar *name)
{
    int i;
    for (i = 0; i < fgl.uniform_count; i++) {
        if (fgl.uniforms[i].program == program && !strcmp(fgl.uniforms[i].name, name))
            return i;
    }
    if (fgl.uniform_count >= FAKE_MAX_UNIFORMS)
        return -1;
    i = fgl.uniform_count++;
    fgl.uniforms[i].program = program;
    snprintf(fgl.uniforms[i].name, sizeof(fgl.uniforms[i].name), "%s", name);
    return i;
}

static FakeUniform *uniform_at(GLint location)
{
    if (location < 0 || location >= fgl.uniform_count) {
        fgl.errors++;
        return NULL;
    }
    if (fgl.uniforms[location].program != fgl.cur_program)
        fgl.errors++; /* uniform set while another program is current */
    return &fgl.uniforms[location];
}

void glUniform1i(GLint location, GLint v0)
{
    FakeUniform *u = uniform_at(location);
    fgl.calls.uniform++;
    if (u)
        u->i = v0;
}

void glUniform1f(GLint location, GLfloat v0)
{
    FakeUniform *u = uniform_at(location);
    fgl.calls.uniform++;
    if (u)
        u->f[0] = v0;
}

void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2)
{
    FakeUniform *u = uniform_at(location);
    fgl.calls.uniform++;
    if (u) {
        u->f[0] = v0;
        u->f[1] = v1;
        u->f[2] = v2;
    }
}

void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    FakeUniform *u = uniform_at(location);
    (void)count; (void)transpose;
    fgl.calls.uniform++;
    if (u)
        memcpy(u->f, value, sizeof(float) * 16);
}

int fake_gl_uniform_int(const char *name)
{
    int i;
    for (i = 0; i < fgl.uniform_count; i++)
        if (fgl.uniforms[i].program == fgl.cur_program && !strcmp(fgl.uniforms[i].name, name))
            return fgl.uniforms[i].i;
    return -12345;
}

void glEnableVertexAttribArray(GLuint index)
{
    fgl.calls.attrib_enable++;
    cur_vao()->attr[index].enabled = 1;
}

void glDisableVertexAttribArray(GLuint index)
{
    fgl.calls.attrib_enable++;
    cur_vao()->attr[index].enabled = 0;
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)
{
    FakeAttr *a = &cur_vao()->attr[index];
    fgl.calls.attrib_pointer++;
    if (!fgl.array_buffer)
        fgl.errors++; /* client-side arrays are not allowed in WebGL2 */
    a->buffer = fgl.array_buffer;
    a->size = size;
    a->type = type;
    a->normalized = normalized;
    a->stride = stride;
    a->offset = (uintptr_t)pointer;
}

void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    fgl.const_attr[index][0] = x;
    fgl.const_attr[index][1] = y;
    fgl.const_attr[index][2] = z;
    fgl.const_attr[index][3] = w;
}

static void fetch_attr(GLuint index, unsigned int vertex, float out[4])
{
    FakeAttr *a = &cur_vao()->attr[index];
    int c;

    if (!a->enabled) {
        memcpy(out, fgl.const_attr[index], sizeof(float) * 4);
        return;
    }
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
    {
        FakeBuffer *b = &fgl.buffers[a->buffer];
        int compSize = a->type == GL_FLOAT ? 4 : 1;
        int stride = a->stride ? a->stride : a->size * compSize;
        size_t at = a->offset + (size_t)vertex * (size_t)stride;

        if (at + (size_t)(a->size * compSize) > b->size) {
            fgl.errors++; /* WebGL would reject the draw (INVALID_OPERATION) */
            return;
        }
        for (c = 0; c < a->size; c++) {
            if (a->type == GL_FLOAT) {
                memcpy(&out[c], b->data + at + c * 4, 4);
            } else {
                unsigned char v = b->data[at + c];
                out[c] = a->normalized ? v / 255.0f : (float)v;
            }
        }
    }
}

static void emit_vertex(unsigned int v)
{
    FakeVertex *o;
    float c[4];
    int order;

    if (fgl.fetched_count >= FAKE_MAX_FETCH) {
        fgl.errors++;
        return;
    }
    o = &fgl.fetched[fgl.fetched_count++];
    {
        float p[4];
        fetch_attr(0, v, p);
        memcpy(o->pos, p, sizeof(o->pos));
    }
    fetch_attr(1, v, c);
    order = fake_gl_uniform_int("u_color_order");
    if (order == 1) {
        o->color[0] = c[2]; o->color[1] = c[1]; o->color[2] = c[0]; o->color[3] = c[3];
    } else if (order == 2) {
        o->color[0] = c[1]; o->color[1] = c[2]; o->color[2] = c[3]; o->color[3] = c[0];
    } else {
        memcpy(o->color, c, sizeof(c));
    }
    {
        float t[4];
        fetch_attr(2, v, t);
        o->tex0[0] = t[0];
        o->tex0[1] = t[1];
        fetch_attr(3, v, t);
        o->tex1[0] = t[0];
        o->tex1[1] = t[1];
    }
    o->use_tex0 = fake_gl_uniform_int("u_use_tex0");
    o->use_tex1 = fake_gl_uniform_int("u_use_tex1");
    o->use_light = fake_gl_uniform_int("u_use_light");
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    FakeBuffer *eb = &fgl.buffers[cur_vao()->ebo];
    size_t off = (uintptr_t)indices;
    int isz = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_BYTE ? 1 : 2;
    GLsizei i;

    (void)mode;
    fgl.calls.draw++;
    if (!cur_vao()->ebo || off + (size_t)count * isz > eb->size) {
        fgl.errors++;
        return;
    }
    for (i = 0; i < count; i++) {
        unsigned int v;
        if (isz == 4) {
            uint32_t x;
            memcpy(&x, eb->data + off + (size_t)i * 4, 4);
            v = x;
        } else if (isz == 2) {
            uint16_t x;
            memcpy(&x, eb->data + off + (size_t)i * 2, 2);
            v = x;
        } else {
            v = eb->data[off + i];
        }
        emit_vertex(v);
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    GLsizei i;
    (void)mode;
    fgl.calls.draw++;
    for (i = 0; i < count; i++)
        emit_vertex((unsigned int)(first + i));
}

void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data)
{
    (void)target; (void)level; (void)internalformat; (void)width; (void)height; (void)border; (void)imageSize; (void)data;
}

void glCompressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLsizei imageSize, const void *data)
{
    (void)target; (void)level; (void)internalformat; (void)width; (void)height; (void)depth; (void)border; (void)imageSize; (void)data;
}
