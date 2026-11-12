/* OpenGL ES 1.1 subset on citro3d (gfx_c3d.c).
 *
 * Only what the iOS game code calls: client-side vertex/normal/texcoord arrays + glDrawArrays,
 * matrix mode / load / frustum / ortho / rotate / scale, one light, linear fog, blend, depth,
 * cull, scissor, 2D textures (plus .t3x upload and framebuffer copy). The game keeps its GL
 * calls; this file is what they land on. Screen space is the 400x240 top screen in GL
 * orientation (origin bottom-left); gfx_c3d.c applies the 3DS screen rotation. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef void GLvoid;

#define GL_FALSE 0
#define GL_TRUE 1

/* primitives */
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_LOOP 0x0002
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006

/* types */
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_FLOAT 0x1406
#define GL_FIXED 0x140C
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_5_6_5 0x8363

/* clear */
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_COLOR_BUFFER_BIT 0x4000

/* capabilities */
#define GL_FOG 0x0B60
#define GL_LIGHTING 0x0B50
#define GL_TEXTURE_2D 0x0DE1
#define GL_CULL_FACE 0x0B44
#define GL_ALPHA_TEST 0x0BC0
#define GL_BLEND 0x0BE2
#define GL_DITHER 0x0BD0
#define GL_STENCIL_TEST 0x0B90
#define GL_DEPTH_TEST 0x0B71
#define GL_LIGHT0 0x4000
#define GL_POINT_SMOOTH 0x0B10
#define GL_LINE_SMOOTH 0x0B20
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_MATERIAL 0x0B57
#define GL_NORMALIZE 0x0BA1
#define GL_RESCALE_NORMAL 0x803A
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

/* depth / alpha funcs */
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207

/* blend */
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308

/* faces */
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408

/* matrices */
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_TEXTURE_MATRIX 0x0BA8
#define GL_VIEWPORT 0x0BA2

/* light */
#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_LIGHT_MODEL_AMBIENT 0x0B53

/* fog */
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_MODE 0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_EXP 0x0800
#define GL_EXP2 0x0801
#define GL_LINEAR 0x2601

/* textures */
#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_GENERATE_MIPMAP 0x8191
#define GL_TEXTURE0 0x84C0

void glActiveTexture(GLenum texture);
void glBindTexture(GLenum target, GLuint texture);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glClear(GLbitfield mask);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
                      GLsizei width, GLsizei height, GLint border);
void glCullFace(GLenum mode);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glDepthRangef(GLclampf zNear, GLclampf zFar);
void glDisable(GLenum cap);
void glDisableClientState(GLenum array);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glEnable(GLenum cap);
void glEnableClientState(GLenum array);
void glFlush(void);
void glFogf(GLenum pname, GLfloat param);
void glFogfv(GLenum pname, const GLfloat *params);
void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
void glGenTextures(GLsizei n, GLuint *textures);
void glGetFloatv(GLenum pname, GLfloat *params);
void glLightModelfv(GLenum pname, const GLfloat *params);
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glMatrixMode(GLenum mode);
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer);
void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                  GLvoid *pixels);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                  GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

/* ---- 3DS extensions used by the platform layer ---- */
typedef enum { GFX_SCREEN_TOP = 0, GFX_SCREEN_BOTTOM = 1 } GfxScreen;

void gfxGL_Init(void);
void gfxGL_Exit(void);
/* Frame bracket: everything the game draws between these goes to `screen`. */
void gfxGL_BeginFrame(void);
void gfxGL_SetScreen(GfxScreen screen);
void gfxGL_EndFrame(void);
/* Stereo: interocular strength from the 3D slider (0 = off). Applied to perspective draws. */
void gfxGL_SetStereo(float iod);
/* Image shown on the bottom screen on frames that draw nothing there: a .t3x whose w x h
 * image sits in the top-left corner of the (POT) texture. */
void gfxGL_SetBottomIdle(const char *t3x_path, int w, int h);
/* Load a tex3ds .t3x into the bound texture; returns false on error.
 * w/h receive the texture (power-of-two) size. */
bool glTexImageT3X(const char *path, int *w, int *h);
/* Vertex arrays that live in linear memory are read by the GPU in place (the owner flushes
 * them after writing, with gfxGL_Flush); anything else is copied into a per-frame ring. */
void *gfxGL_LinearAlloc(uint32_t size);
void gfxGL_LinearFree(void *p);
void gfxGL_Flush(const void *p, uint32_t size);
/* Draw statistics of the last frame, for the debug overlay. */
int gfxGL_LastDrawCount(void);
