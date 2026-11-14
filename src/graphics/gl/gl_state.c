#include <GL/gl.h>
#include <GL/ipo_gl.h>

static float clamp01f(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static double clamp01d(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static void set_cap_enabled(GLContext *ctx, GLenum cap, GLboolean enabled) {
    switch (cap) {
        case GL_DEPTH_TEST:     ctx->depth_test_enabled     = enabled; break;
        case GL_ALPHA_TEST:     ctx->alpha_test_enabled     = enabled; break;
        case GL_STENCIL_TEST:   ctx->stencil_test_enabled   = enabled; break;
        case GL_BLEND:          ctx->blend_enabled          = enabled; break;
        case GL_COLOR_LOGIC_OP: ctx->logic_op_enabled       = enabled; break;
        case GL_SCISSOR_TEST:   ctx->scissor_enabled        = enabled; break;
        case GL_CULL_FACE:      ctx->cull_face_enabled      = enabled; break;
        case GL_LIGHTING:       ctx->lighting_enabled       = enabled; break;
        case GL_FOG:            ctx->fog_enabled            = enabled; break;
        case GL_TEXTURE_2D:     ctx->texture_2d_enabled     = enabled; break;
        case GL_TEXTURE_3D:     ctx->texture_3d_enabled     = enabled; break;
        case GL_COLOR_MATERIAL: ctx->color_material_enabled = enabled; break;
        case GL_NORMALIZE:      ctx->normalize_normals      = enabled; break;
        default:
            if (cap >= GL_LIGHT0) {
                size_t l_idx = (size_t)(cap - GL_LIGHT0);
                gl_light_t *l = gl_get_or_create_light(ctx, l_idx);
                if (l) l->enabled = enabled;
                else if (ctx->error_flag == GL_NO_ERROR) ctx->error_flag = GL_OUT_OF_MEMORY;
            } else {
                ctx->error_flag = GL_INVALID_ENUM;
            }
            break;
    }
}

void glEnable(GLenum cap) {
    GLContext *ctx = gl_get_current_context();
    if (ctx) set_cap_enabled(ctx, cap, GL_TRUE);
}

void glDisable(GLenum cap) {
    GLContext *ctx = gl_get_current_context();
    if (ctx) set_cap_enabled(ctx, cap, GL_FALSE);
}

GLboolean glIsEnabled(GLenum cap) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return GL_FALSE;
    switch (cap) {
        case GL_DEPTH_TEST:     return ctx->depth_test_enabled;
        case GL_ALPHA_TEST:     return ctx->alpha_test_enabled;
        case GL_STENCIL_TEST:   return ctx->stencil_test_enabled;
        case GL_BLEND:          return ctx->blend_enabled;
        case GL_COLOR_LOGIC_OP: return ctx->logic_op_enabled;
        case GL_SCISSOR_TEST:   return ctx->scissor_enabled;
        case GL_CULL_FACE:      return ctx->cull_face_enabled;
        case GL_LIGHTING:       return ctx->lighting_enabled;
        case GL_FOG:            return ctx->fog_enabled;
        case GL_TEXTURE_2D:     return ctx->texture_2d_enabled;
        case GL_TEXTURE_3D:     return ctx->texture_3d_enabled;
        case GL_COLOR_MATERIAL: return ctx->color_material_enabled;
        case GL_NORMALIZE:      return ctx->normalize_normals;
        default:
            if (cap >= GL_LIGHT0) {
                size_t l_idx = (size_t)(cap - GL_LIGHT0);
                if (l_idx < ctx->lights_count && ctx->lights) {
                    return ctx->lights[l_idx].enabled;
                }
            }
            return GL_FALSE;
    }
}

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->clear_color[0] = clamp01f(red);
    ctx->clear_color[1] = clamp01f(green);
    ctx->clear_color[2] = clamp01f(blue);
    ctx->clear_color[3] = clamp01f(alpha);
}

void glClearDepth(GLclampd depth) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->clear_depth = clamp01d(depth);
}

void glClearStencil(GLint s) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->clear_stencil = s;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (width < 0 || height < 0) {
        ctx->error_flag = GL_INVALID_VALUE;
        return;
    }
    if (x == 0 && y == 0 && width > 0 && height > 0 &&
        (ctx->width != width || ctx->height != height)) {
        gl_resize_context(ctx, width, height);
    }
    ctx->vp_x = x;
    ctx->vp_y = y;
    ctx->vp_w = width;
    ctx->vp_h = height;
}

void glDepthRange(GLclampd nearVal, GLclampd farVal) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->depth_near = clamp01d(nearVal);
    ctx->depth_far  = clamp01d(farVal);
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (width < 0 || height < 0) {
        ctx->error_flag = GL_INVALID_VALUE;
        return;
    }
    ctx->scissor_x = x;
    ctx->scissor_y = y;
    ctx->scissor_w = width;
    ctx->scissor_h = height;
}

void glPointSize(GLfloat size) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (size <= 0.0f) {
        ctx->error_flag = GL_INVALID_VALUE;
        return;
    }
    ctx->point_size = size;
}

void glLineWidth(GLfloat width) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (width <= 0.0f) {
        ctx->error_flag = GL_INVALID_VALUE;
        return;
    }
    ctx->line_width = width;
}

void glPolygonMode(GLenum face, GLenum mode) {
    (void)face;
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (mode == GL_POINT || mode == GL_LINE || mode == GL_FILL) {
        ctx->polygon_mode = mode;
    } else {
        ctx->error_flag = GL_INVALID_ENUM;
    }
}

void glCullFace(GLenum mode) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (mode == GL_FRONT || mode == GL_BACK || mode == GL_FRONT_AND_BACK) {
        ctx->cull_face_mode = mode;
    } else {
        ctx->error_flag = GL_INVALID_ENUM;
    }
}

void glFrontFace(GLenum mode) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (mode == GL_CW || mode == GL_CCW) {
        ctx->front_face = mode;
    } else {
        ctx->error_flag = GL_INVALID_ENUM;
    }
}

void glDepthFunc(GLenum func) {
    GLContext *ctx = gl_get_current_context();
    if (ctx) ctx->depth_func = func;
}

void glDepthMask(GLboolean flag) {
    GLContext *ctx = gl_get_current_context();
    if (ctx) ctx->depth_mask = flag;
}

void glAlphaFunc(GLenum func, GLclampf ref) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->alpha_func = func;
    ctx->alpha_ref  = clamp01f(ref);
}

void glBlendFunc(GLenum sfactor, GLenum dfactor) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->blend_sfactor = sfactor;
    ctx->blend_dfactor = dfactor;
}

void glBlendEquation(GLenum mode) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (mode == GL_FUNC_ADD || mode == GL_FUNC_SUBTRACT ||
        mode == GL_FUNC_REVERSE_SUBTRACT || mode == GL_MIN || mode == GL_MAX) {
        ctx->blend_equation = mode;
    } else {
        ctx->error_flag = GL_INVALID_ENUM;
    }
}

void glLogicOp(GLenum opcode) {
    GLContext *ctx = gl_get_current_context();
    if (ctx) ctx->logic_opcode = opcode;
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->stencil_func = func;
    ctx->stencil_ref  = ref;
    ctx->stencil_mask = mask;
}

void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->stencil_fail  = fail;
    ctx->stencil_zfail = zfail;
    ctx->stencil_zpass = zpass;
}

void glStencilMask(GLuint mask) {
    GLContext *ctx = gl_get_current_context();
    if (ctx) ctx->stencil_writemask = mask;
}

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
        case GL_BLEND_EQUATION:
            params[0] = (GLint)ctx->blend_equation;
            break;
        default:
            ctx->error_flag = GL_INVALID_ENUM;
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
        case GL_TEXTURE_MATRIX: {
            GLfloat *m = gl_matrix_current(&ctx->texture_stack);
            if (m) for (int i = 0; i < 16; i++) params[i] = m[i];
            break;
        }
        default:
            ctx->error_flag = GL_INVALID_ENUM;
            break;
    }
}

void glGetBooleanv(GLenum pname, GLboolean *params) {
    if (!params) return;
    *params = glIsEnabled(pname);
}

void glFlush(void) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (ctx->in_begin) {
        if (ctx->error_flag == GL_NO_ERROR) ctx->error_flag = GL_INVALID_OPERATION;
        return;
    }

    /* The software rasterizer executes every command synchronously. */
    __sync_synchronize();
}

void glFinish(void) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (ctx->in_begin) {
        if (ctx->error_flag == GL_NO_ERROR) ctx->error_flag = GL_INVALID_OPERATION;
        return;
    }

    /* Rendering is immediate; the full barrier makes framebuffer writes visible. */
    __sync_synchronize();
}
