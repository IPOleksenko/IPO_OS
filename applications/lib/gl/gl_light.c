#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <string.h>

void glShadeModel(GLenum mode) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (mode == GL_FLAT || mode == GL_SMOOTH) {
        ctx->shade_model = mode;
    } else {
        ctx->error_flag = GL_INVALID_ENUM;
    }
}

void glLightf(GLenum light, GLenum pname, GLfloat param) {
    glLightfv(light, pname, &param);
}

void glLighti(GLenum light, GLenum pname, GLint param) {
    GLfloat f = (GLfloat)param;
    glLightfv(light, pname, &f);
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !params) return;
    if (light < GL_LIGHT0 || light > GL_LIGHT7) {
        ctx->error_flag = GL_INVALID_ENUM;
        return;
    }

    gl_light_t *l = &ctx->lights[light - GL_LIGHT0];
    switch (pname) {
        case GL_AMBIENT:               memcpy(l->ambient, params, 4 * sizeof(GLfloat)); break;
        case GL_DIFFUSE:               memcpy(l->diffuse, params, 4 * sizeof(GLfloat)); break;
        case GL_SPECULAR:              memcpy(l->specular, params, 4 * sizeof(GLfloat)); break;
        case GL_POSITION:              memcpy(l->position, params, 4 * sizeof(GLfloat)); break;
        case GL_SPOT_DIRECTION:        memcpy(l->spot_direction, params, 3 * sizeof(GLfloat)); break;
        case GL_SPOT_EXPONENT:         l->spot_exponent = params[0]; break;
        case GL_SPOT_CUTOFF:           l->spot_cutoff = params[0]; break;
        case GL_CONSTANT_ATTENUATION:  l->constant_attenuation = params[0]; break;
        case GL_LINEAR_ATTENUATION:    l->linear_attenuation = params[0]; break;
        case GL_QUADRATIC_ATTENUATION: l->quadratic_attenuation = params[0]; break;
        default:                       ctx->error_flag = GL_INVALID_ENUM; break;
    }
}

void glMaterialf(GLenum face, GLenum pname, GLfloat param) {
    glMaterialfv(face, pname, &param);
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !params) return;

    gl_material_t *m = (face == GL_BACK) ? &ctx->back_material : &ctx->front_material;

    switch (pname) {
        case GL_AMBIENT:
            memcpy(m->ambient, params, 4 * sizeof(GLfloat));
            break;
        case GL_DIFFUSE:
            memcpy(m->diffuse, params, 4 * sizeof(GLfloat));
            break;
        case GL_SPECULAR:
            memcpy(m->specular, params, 4 * sizeof(GLfloat));
            break;
        case GL_EMISSION:
            memcpy(m->emission, params, 4 * sizeof(GLfloat));
            break;
        case GL_SHININESS:
            m->shininess = params[0];
            break;
        case GL_AMBIENT_AND_DIFFUSE:
            memcpy(m->ambient, params, 4 * sizeof(GLfloat));
            memcpy(m->diffuse, params, 4 * sizeof(GLfloat));
            break;
        default:
            ctx->error_flag = GL_INVALID_ENUM;
            break;
    }
}
