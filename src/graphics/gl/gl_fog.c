#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <string.h>

void glFogf(GLenum pname, GLfloat param) {
    glFogfv(pname, &param);
}

void glFogi(GLenum pname, GLint param) {
    GLfloat f = (GLfloat)param;
    glFogfv(pname, &f);
}

void glFogfv(GLenum pname, const GLfloat *params) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !params) return;

    switch (pname) {
        case GL_FOG_MODE:
            ctx->fog_mode = (GLenum)params[0];
            break;
        case GL_FOG_DENSITY:
            ctx->fog_density = params[0];
            break;
        case GL_FOG_START:
            ctx->fog_start = params[0];
            break;
        case GL_FOG_END:
            ctx->fog_end = params[0];
            break;
        case GL_FOG_COLOR:
            memcpy(ctx->fog_color, params, 4 * sizeof(GLfloat));
            break;
        default:
            ctx->error_flag = GL_INVALID_ENUM;
            break;
    }
}
