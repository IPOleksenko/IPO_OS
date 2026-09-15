#include <GL/gl.h>
#include <GL/ipo_gl.h>

GLenum glGetError(void) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return GL_NO_ERROR;
    GLenum err = ctx->error_flag;
    ctx->error_flag = GL_NO_ERROR;
    return err;
}

const GLubyte *glGetString(GLenum name) {
    switch (name) {
        case GL_VENDOR:     return (const GLubyte *)"IPO_OS Project";
        case GL_RENDERER:   return (const GLubyte *)"IPO-GL 1.2 Software Rasterizer";
        case GL_VERSION:    return (const GLubyte *)"1.2";
        case GL_EXTENSIONS: return (const GLubyte *)"GL_EXT_bgra GL_EXT_packed_pixels";
        default:            return (const GLubyte *)"";
    }
}

void glGetIntegerv(GLenum pname, GLint *params) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_VIEWPORT:
            params[0] = ctx->vp_x;
            params[1] = ctx->vp_y;
            params[2] = ctx->vp_w;
            params[3] = ctx->vp_h;
            break;
        case GL_SCISSOR_BOX:
            params[0] = ctx->scissor_x;
            params[1] = ctx->scissor_y;
            params[2] = ctx->scissor_w;
            params[3] = ctx->scissor_h;
            break;
        case GL_MATRIX_MODE:
            params[0] = (GLint)ctx->matrix_mode;
            break;
        default:
            break;
    }
}

void glGetFloatv(GLenum pname, GLfloat *params) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_VIEWPORT:
            params[0] = (GLfloat)ctx->vp_x;
            params[1] = (GLfloat)ctx->vp_y;
            params[2] = (GLfloat)ctx->vp_w;
            params[3] = (GLfloat)ctx->vp_h;
            break;
        case GL_MODELVIEW_MATRIX: {
            GLfloat *m = gl_matrix_current(&ctx->modelview_stack);
            if (m) for (int i = 0; i < 16; i++) params[i] = m[i];
            break;
        }
        case GL_PROJECTION_MATRIX: {
            GLfloat *m = gl_matrix_current(&ctx->projection_stack);
            if (m) for (int i = 0; i < 16; i++) params[i] = m[i];
            break;
        }
        default:
            break;
    }
}

void glGetBooleanv(GLenum pname, GLboolean *params) {
    if (!params) return;
    *params = glIsEnabled(pname);
}

void glFlush(void) {}
void glFinish(void) {}

