#ifndef __GL_H__
#define __GL_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * GL Data Types
 * ========================================================================= */
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;
typedef void           GLvoid;

/* =========================================================================
 * Constants
 * ========================================================================= */
#define GL_FALSE                          0
#define GL_TRUE                           1

/* Data types */
#define GL_BYTE                           0x1400
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_SHORT                          0x1402
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_INT                            0x1404
#define GL_UNSIGNED_INT                   0x1405
#define GL_FLOAT                          0x1406
#define GL_DOUBLE                         0x140A
#define GL_2_BYTES                        0x1407
#define GL_3_BYTES                        0x1408
#define GL_4_BYTES                        0x1409

/* GL Packed Pixel Types */
#define GL_UNSIGNED_BYTE_3_3_2            0x8032
#define GL_UNSIGNED_BYTE_2_3_3_REV        0x8362
#define GL_UNSIGNED_SHORT_5_6_5           0x8363
#define GL_UNSIGNED_SHORT_5_6_5_REV       0x8364
#define GL_UNSIGNED_SHORT_4_4_4_4         0x8033
#define GL_UNSIGNED_SHORT_4_4_4_4_REV     0x8365
#define GL_UNSIGNED_SHORT_5_5_5_1         0x8034
#define GL_UNSIGNED_SHORT_1_5_5_5_REV     0x8366
#define GL_UNSIGNED_INT_8_8_8_8           0x8035
#define GL_UNSIGNED_INT_8_8_8_8_REV       0x8367

/* Primitives */
#define GL_POINTS                         0x0000
#define GL_LINES                          0x0001
#define GL_LINE_LOOP                      0x0002
#define GL_LINE_STRIP                     0x0003
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_TRIANGLE_FAN                   0x0006
#define GL_QUADS                          0x0007
#define GL_QUAD_STRIP                     0x0008
#define GL_POLYGON                        0x0009

/* Matrix Modes */
#define GL_MATRIX_MODE                    0x0BA0
#define GL_MODELVIEW                      0x1700
#define GL_PROJECTION                     0x1701
#define GL_TEXTURE                        0x1702

/* Depth & Stencil Comparison Functions */
#define GL_NEVER                          0x0200
#define GL_LESS                           0x0201
#define GL_EQUAL                          0x0202
#define GL_LEQUAL                         0x0203
#define GL_GREATER                        0x0204
#define GL_NOTEQUAL                       0x0205
#define GL_GEQUAL                         0x0206
#define GL_ALWAYS                         0x0207

/* Stencil Operations */
#define GL_KEEP                           0x1E00
#define GL_REPLACE                        0x1E01
#define GL_INCR                           0x1E02
#define GL_DECR                           0x1E03
#define GL_INVERT                         0x150A

/* Blending Factors */
#define GL_ZERO                           0
#define GL_ONE                            1
#define GL_SRC_COLOR                      0x0300
#define GL_ONE_MINUS_SRC_COLOR            0x0301
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_DST_ALPHA                      0x0304
#define GL_ONE_MINUS_DST_ALPHA            0x0305
#define GL_DST_COLOR                      0x0306
#define GL_ONE_MINUS_DST_COLOR            0x0307
#define GL_SRC_ALPHA_SATURATE             0x0308

/* GL Blend Equations */
#define GL_BLEND_EQUATION                 0x8009
#define GL_FUNC_ADD                       0x8006
#define GL_MIN                            0x8007
#define GL_MAX                            0x8008
#define GL_FUNC_SUBTRACT                  0x800A
#define GL_FUNC_REVERSE_SUBTRACT          0x800B

/* Buffers & Clear Bits */
#define GL_COLOR_BUFFER_BIT               0x00004000
#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_STENCIL_BUFFER_BIT             0x00000400
#define GL_ACCUM_BUFFER_BIT               0x00000200

/* Enable Caps */
#define GL_CULL_FACE                      0x0B44
#define GL_LIGHTING                       0x0B50
#define GL_COLOR_MATERIAL                 0x0B57
#define GL_FOG                            0x0B60
#define GL_DEPTH_TEST                     0x0B71
#define GL_STENCIL_TEST                   0x0B90
#define GL_NORMALIZE                      0x0BA1
#define GL_ALPHA_TEST                     0x0BC0
#define GL_DITHER                         0x0BD0
#define GL_BLEND                          0x0BE2
#define GL_COLOR_LOGIC_OP                 0x0BF2
#define GL_SCISSOR_TEST                   0x0C11
#define GL_TEXTURE_1D                     0x0DE0
#define GL_TEXTURE_2D                     0x0DE1
#define GL_TEXTURE_3D                     0x806F

/* Culling */
#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_FRONT_AND_BACK                 0x0408
#define GL_CW                             0x0900
#define GL_CCW                            0x0901

/* Polygon Modes */
#define GL_POINT                          0x1B00
#define GL_LINE                           0x1B01
#define GL_FILL                           0x1B02

/* Shading Models */
#define GL_FLAT                           0x1D00
#define GL_SMOOTH                         0x1D01

/* Lighting */
#define GL_LIGHT0                         0x4000
#define GL_LIGHT1                         0x4001
#define GL_LIGHT2                         0x4002
#define GL_LIGHT3                         0x4003
#define GL_LIGHT4                         0x4004
#define GL_LIGHT5                         0x4005
#define GL_LIGHT6                         0x4006
#define GL_LIGHT7                         0x4007
#define GL_AMBIENT                        0x1200
#define GL_DIFFUSE                        0x1201
#define GL_SPECULAR                       0x1202
#define GL_POSITION                       0x1203
#define GL_SPOT_DIRECTION                 0x1204
#define GL_SPOT_EXPONENT                  0x1205
#define GL_SPOT_CUTOFF                    0x1206
#define GL_CONSTANT_ATTENUATION           0x1207
#define GL_LINEAR_ATTENUATION             0x1208
#define GL_QUADRATIC_ATTENUATION          0x1209
#define GL_EMISSION                       0x1600
#define GL_SHININESS                      0x1601
#define GL_AMBIENT_AND_DIFFUSE            0x1602

/* Fog */
#define GL_FOG_INDEX                      0x0B61
#define GL_FOG_DENSITY                    0x0B62
#define GL_FOG_START                      0x0B63
#define GL_FOG_END                        0x0B64
#define GL_FOG_MODE                       0x0B65
#define GL_FOG_COLOR                      0x0B66
#define GL_EXP                            0x0800
#define GL_EXP2                           0x0801
#define GL_LINEAR                         0x2601

/* Logic Ops */
#define GL_CLEAR                          0x1500
#define GL_AND                            0x1501
#define GL_AND_REVERSE                    0x1502
#define GL_COPY                           0x1503
#define GL_AND_INVERTED                   0x1504
#define GL_NOOP                           0x1505
#define GL_XOR                            0x1506
#define GL_OR                             0x1507
#define GL_NOR                            0x1508
#define GL_EQUIV                          0x1509
#define GL_OR_REVERSE                     0x150B
#define GL_COPY_INVERTED                  0x150C
#define GL_OR_INVERTED                    0x150D
#define GL_NAND                           0x150E
#define GL_SET                            0x150F

/* Texturing */
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_TEXTURE_WRAP_R                 0x8072
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_NEAREST                        0x2600
#define GL_NEAREST_MIPMAP_NEAREST         0x2700
#define GL_LINEAR_MIPMAP_NEAREST          0x2701
#define GL_NEAREST_MIPMAP_LINEAR          0x2702
#define GL_LINEAR_MIPMAP_LINEAR           0x2703
#define GL_REPEAT                         0x2901
#define GL_CLAMP                          0x2900
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_TEXTURE_ENV                    0x2300
#define GL_TEXTURE_ENV_MODE               0x2200
#define GL_MODULATE                       0x2100
#define GL_DECAL                          0x2101

/* Formats */
#define GL_ALPHA                          0x1906
#define GL_RGB                            0x1907
#define GL_RGBA                           0x1908
#define GL_LUMINANCE                      0x1909
#define GL_LUMINANCE_ALPHA                0x190A
#define GL_BGR                            0x80E0
#define GL_BGRA                           0x80E1
#define GL_DEPTH_COMPONENT                0x1902

/* Display Lists */
#define GL_COMPILE                        0x1300
#define GL_COMPILE_AND_EXECUTE            0x1301

/* Vertex Arrays */
#define GL_VERTEX_ARRAY                   0x8074
#define GL_NORMAL_ARRAY                   0x8075
#define GL_COLOR_ARRAY                    0x8076
#define GL_INDEX_ARRAY                    0x8077
#define GL_TEXTURE_COORD_ARRAY            0x8078

/* String Queries */
#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_EXTENSIONS                     0x1F03

/* Errors */
#define GL_NO_ERROR                       0
#define GL_INVALID_ENUM                   0x0500
#define GL_INVALID_VALUE                  0x0501
#define GL_INVALID_OPERATION              0x0502
#define GL_STACK_OVERFLOW                 0x0503
#define GL_STACK_UNDERFLOW                0x0504
#define GL_OUT_OF_MEMORY                  0x0505

/* State Queries */
#define GL_VIEWPORT                       0x0BA2
#define GL_MODELVIEW_MATRIX               0x0BA6
#define GL_PROJECTION_MATRIX              0x0BA7
#define GL_TEXTURE_MATRIX                 0x0BA8
#define GL_SCISSOR_BOX                    0x0C10

/* =========================================================================
 * Function Prototypes
 * ========================================================================= */

/* State & Error */
GLenum          glGetError(void);
const GLubyte*  glGetString(GLenum name);
void            glGetIntegerv(GLenum pname, GLint *params);
void            glGetFloatv(GLenum pname, GLfloat *params);
void            glGetBooleanv(GLenum pname, GLboolean *params);
GLboolean       glIsEnabled(GLenum cap);

void            glEnable(GLenum cap);
void            glDisable(GLenum cap);
void            glClear(GLbitfield mask);
void            glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void            glClearDepth(GLclampd depth);
void            glClearStencil(GLint s);

/* Viewport & Scissor */
void            glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void            glDepthRange(GLclampd nearVal, GLclampd farVal);
void            glScissor(GLint x, GLint y, GLsizei width, GLsizei height);

/* Matrix operations */
void            glMatrixMode(GLenum mode);
void            glLoadIdentity(void);
void            glLoadMatrixf(const GLfloat *m);
void            glMultMatrixf(const GLfloat *m);
void            glPushMatrix(void);
void            glPopMatrix(void);
void            glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void            glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void            glScalef(GLfloat x, GLfloat y, GLfloat z);
void            glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble nearVal, GLdouble farVal);
void            glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble nearVal, GLdouble farVal);

/* Immediate geometry */
void            glBegin(GLenum mode);
void            glEnd(void);
void            glVertex2f(GLfloat x, GLfloat y);
void            glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void            glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void            glVertex3fv(const GLfloat *v);
void            glColor3f(GLfloat red, GLfloat green, GLfloat blue);
void            glColor3ub(GLubyte red, GLubyte green, GLubyte blue);
void            glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void            glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
void            glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);
void            glNormal3fv(const GLfloat *v);
void            glTexCoord1f(GLfloat s);
void            glTexCoord2f(GLfloat s, GLfloat t);
void            glTexCoord3f(GLfloat s, GLfloat t, GLfloat r);
void            glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q);
void            glTexCoord2fv(const GLfloat *v);

/* Vertex Arrays */
void            glEnableClientState(GLenum array);
void            glDisableClientState(GLenum array);
void            glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void            glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void            glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer);
void            glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
void            glDrawArrays(GLenum mode, GLint first, GLsizei count);
void            glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);

/* Rasterization Control */
void            glPointSize(GLfloat size);
void            glLineWidth(GLfloat width);
void            glPolygonMode(GLenum face, GLenum mode);
void            glCullFace(GLenum mode);
void            glFrontFace(GLenum mode);
void            glShadeModel(GLenum mode);

/* Tests & Operations */
void            glDepthFunc(GLenum func);
void            glDepthMask(GLboolean flag);
void            glAlphaFunc(GLenum func, GLclampf ref);
void            glBlendFunc(GLenum sfactor, GLenum dfactor);
void            glBlendEquation(GLenum mode);
void            glLogicOp(GLenum opcode);
void            glStencilFunc(GLenum func, GLint ref, GLuint mask);
void            glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
void            glStencilMask(GLuint mask);

/* Lighting & Materials */
void            glLightf(GLenum light, GLenum pname, GLfloat param);
void            glLighti(GLenum light, GLenum pname, GLint param);
void            glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void            glMaterialf(GLenum face, GLenum pname, GLfloat param);
void            glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);

/* Fog */
void            glFogf(GLenum pname, GLfloat param);
void            glFogi(GLenum pname, GLint param);
void            glFogfv(GLenum pname, const GLfloat *params);

/* Textures */
void            glGenTextures(GLsizei n, GLuint *textures);
void            glBindTexture(GLenum target, GLuint texture);
void            glDeleteTextures(GLsizei n, const GLuint *textures);
void            glTexParameteri(GLenum target, GLenum pname, GLint param);
void            glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void            glTexEnvi(GLenum target, GLenum pname, GLint param);
void            glTexEnvf(GLenum target, GLenum pname, GLfloat param);

void            glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void            glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void            glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const GLvoid *pixels);

void            glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels);
void            glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
void            glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const GLvoid *pixels);

void            glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
void            glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);

/* Framebuffer Transfer */
void            glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels);

/* Display Lists */
GLuint          glGenLists(GLsizei range);
void            glNewList(GLuint list, GLenum mode);
void            glEndList(void);
void            glCallList(GLuint list);
void            glCallLists(GLsizei n, GLenum type, const GLvoid *lists);
void            glDeleteLists(GLuint list, GLsizei range);
GLboolean       glIsList(GLuint list);

void            glFlush(void);
void            glFinish(void);

#ifdef __cplusplus
}
#endif

#endif /* __GL_H__ */
