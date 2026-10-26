#ifndef WEB_WEBGL2_COMPAT_H
#define WEB_WEBGL2_COMPAT_H

#ifdef __EMSCRIPTEN__

void glActiveTextureARB(unsigned int texture);
void glAlphaFunc(unsigned int func, float ref);
void glBindProgramARB(unsigned int target, unsigned int program);
void glBindTexture(unsigned int target, unsigned int texture);
void glBindVertexArray(unsigned int array);
void glBlendFunc(unsigned int sfactor, unsigned int dfactor);
void glClear(unsigned int mask);
void glClearColor(float red, float green, float blue, float alpha);
void glClearDepth(double depth);
void glClearStencil(int s);
void glClientActiveTextureARB(unsigned int texture);
void glColor4f(float red, float green, float blue, float alpha);
void glColorMask(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha);
void glColorPointer(int size, unsigned int type, int stride, const void *pointer);
void glDepthFunc(unsigned int func);
void glDepthMask(unsigned char flag);
void glDepthRange(double zNear, double zFar);
void glDisableClientState(unsigned int array);
void glEnableClientState(unsigned int array);
void glFogfv(unsigned int pname, const float *params);
unsigned int glGetError(void);
void glGetIntegerv(unsigned int pname, int *data);
void glLoadIdentity(void);
void glLoadMatrixf(const float *m);
void glMatrixMode(unsigned int mode);
void glProgramEnvParameter4fvARB(unsigned int target, unsigned int index, const float *params);
void glScalef(float x, float y, float z);
void glTexCoordPointer(int size, unsigned int type, int stride, const void *pointer);
void glTexEnvfv(unsigned int target, unsigned int pname, const float *params);
void glTexEnvi(unsigned int target, unsigned int pname, int param);
void glTexParameteri(unsigned int target, unsigned int pname, int param);
void glVertexPointer(int size, unsigned int type, int stride, const void *pointer);
void glViewport(int x, int y, int width, int height);

void webgl2_glDisable(unsigned int cap);
void webgl2_glDrawArrays(unsigned int mode, int first, int count);
void webgl2_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices);
void webgl2_glDrawRangeElements(unsigned int mode, unsigned int start, unsigned int end, int count, unsigned int type, const void *indices);
void webgl2_glEnable(unsigned int cap);

void webgl2_set_ff_light(int index, int enable, const float *ambient_rgb);

/*
 * Cached state setters (webgl2_compat.c). Every GL state writer in the
 * renderer includes this header through imports/opengl.h, so routing these
 * entry points through one cache keeps it identical to the real context
 * while dropping the redundant WebGL calls the D3D layer issues per draw.
 */
void webgl2_glBindTexture(unsigned int target, unsigned int texture);
void webgl2_glTexParameteri(unsigned int target, unsigned int pname, int param);
void webgl2_glTexParameterf(unsigned int target, unsigned int pname, float param);
void webgl2_glDeleteTextures(int n, const unsigned int *textures);
void webgl2_glDepthFunc(unsigned int func);
void webgl2_glDepthMask(unsigned char flag);
void webgl2_glBlendFunc(unsigned int sfactor, unsigned int dfactor);
void webgl2_glColorMask(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha);

#ifndef WEBGL2_COMPAT_NO_STATE_MACROS
#define glBindTexture webgl2_glBindTexture
#define glTexParameteri webgl2_glTexParameteri
#define glTexParameterf webgl2_glTexParameterf
#define glDeleteTextures webgl2_glDeleteTextures
#define glDepthFunc webgl2_glDepthFunc
#define glDepthMask webgl2_glDepthMask
#define glBlendFunc webgl2_glBlendFunc
#define glColorMask webgl2_glColorMask
#endif

#endif

#endif
