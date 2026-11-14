#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <memory/kmalloc.h>
#include <limits.h>
#include <string.h>

void glBegin(GLenum mode) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (ctx->in_begin) {
        ctx->error_flag = GL_INVALID_OPERATION;
        return;
    }
    ctx->prim_mode = mode;
    ctx->in_begin  = true;
    ctx->vert_buf.count = 0;
}

void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !ctx->in_begin) return;

    if (ctx->vert_buf.count >= ctx->vert_buf.capacity) {
        if (ctx->vert_buf.capacity > INT_MAX / 2) {
            ctx->error_flag = GL_OUT_OF_MEMORY;
            return;
        }
        int new_cap = ctx->vert_buf.capacity ? ctx->vert_buf.capacity * 2 : 64;
        gl_vertex_t *new_data = krealloc(ctx->vert_buf.data, (size_t)new_cap * sizeof(gl_vertex_t));
        if (!new_data) {
            ctx->error_flag = GL_OUT_OF_MEMORY;
            return;
        }
        ctx->vert_buf.data = new_data;
        ctx->vert_buf.capacity = new_cap;
    }

    gl_vertex_t *v = &ctx->vert_buf.data[ctx->vert_buf.count++];
    v->obj[0] = x; v->obj[1] = y; v->obj[2] = z; v->obj[3] = w;

    memcpy(v->color, ctx->cur_color, 4 * sizeof(GLfloat));

    GLfloat *tx = gl_matrix_current(&ctx->texture_stack);
    if (tx) {
        const GLfloat *tc = ctx->cur_texcoord;
        v->tex[0] = tx[0] * tc[0] + tx[4] * tc[1] + tx[8]  * tc[2] + tx[12] * tc[3];
        v->tex[1] = tx[1] * tc[0] + tx[5] * tc[1] + tx[9]  * tc[2] + tx[13] * tc[3];
        v->tex[2] = tx[2] * tc[0] + tx[6] * tc[1] + tx[10] * tc[2] + tx[14] * tc[3];
        v->tex[3] = tx[3] * tc[0] + tx[7] * tc[1] + tx[11] * tc[2] + tx[15] * tc[3];
    } else {
        memcpy(v->tex, ctx->cur_texcoord, 4 * sizeof(GLfloat));
    }

    /* Transform normal by ModelView */
    GLfloat *mv = gl_matrix_current(&ctx->modelview_stack);
    if (mv) {
        v->norm[0] = mv[0] * ctx->cur_normal[0] + mv[4] * ctx->cur_normal[1] + mv[8]  * ctx->cur_normal[2];
        v->norm[1] = mv[1] * ctx->cur_normal[0] + mv[5] * ctx->cur_normal[1] + mv[9]  * ctx->cur_normal[2];
        v->norm[2] = mv[2] * ctx->cur_normal[0] + mv[6] * ctx->cur_normal[1] + mv[10] * ctx->cur_normal[2];

        /* eye = ModelView * obj */
        v->eye[0] = mv[0] * x + mv[4] * y + mv[8]  * z + mv[12] * w;
        v->eye[1] = mv[1] * x + mv[5] * y + mv[9]  * z + mv[13] * w;
        v->eye[2] = mv[2] * x + mv[6] * y + mv[10] * z + mv[14] * w;
        v->eye[3] = mv[3] * x + mv[7] * y + mv[11] * z + mv[15] * w;
    } else {
        memcpy(v->norm, ctx->cur_normal, 3 * sizeof(GLfloat));
        memcpy(v->eye, v->obj, 4 * sizeof(GLfloat));
    }

    gl_evaluate_vertex_lighting(ctx, v);

    /* clip = Projection * eye */
    GLfloat *pj = gl_matrix_current(&ctx->projection_stack);
    if (pj) {
        v->clip[0] = pj[0] * v->eye[0] + pj[4] * v->eye[1] + pj[8]  * v->eye[2] + pj[12] * v->eye[3];
        v->clip[1] = pj[1] * v->eye[0] + pj[5] * v->eye[1] + pj[9]  * v->eye[2] + pj[13] * v->eye[3];
        v->clip[2] = pj[2] * v->eye[0] + pj[6] * v->eye[1] + pj[10] * v->eye[2] + pj[14] * v->eye[3];
        v->clip[3] = pj[3] * v->eye[0] + pj[7] * v->eye[1] + pj[11] * v->eye[2] + pj[15] * v->eye[3];
    } else {
        memcpy(v->clip, v->eye, 4 * sizeof(GLfloat));
    }
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    glVertex4f(x, y, z, 1.0f);
}

void glVertex2f(GLfloat x, GLfloat y) {
    glVertex4f(x, y, 0.0f, 1.0f);
}

void glVertex3fv(const GLfloat *v) {
    if (v) glVertex4f(v[0], v[1], v[2], 1.0f);
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->cur_color[0] = red;
    ctx->cur_color[1] = green;
    ctx->cur_color[2] = blue;
    ctx->cur_color[3] = alpha;
}

void glColor3f(GLfloat red, GLfloat green, GLfloat blue) {
    glColor4f(red, green, blue, 1.0f);
}

void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) {
    glColor4f(red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f);
}

void glColor3ub(GLubyte red, GLubyte green, GLubyte blue) {
    glColor4ub(red, green, blue, 255);
}

void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->cur_normal[0] = nx;
    ctx->cur_normal[1] = ny;
    ctx->cur_normal[2] = nz;
}

void glNormal3fv(const GLfloat *v) {
    if (v) glNormal3f(v[0], v[1], v[2]);
}

void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->cur_texcoord[0] = s;
    ctx->cur_texcoord[1] = t;
    ctx->cur_texcoord[2] = r;
    ctx->cur_texcoord[3] = q;
}

void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) {
    glTexCoord4f(s, t, r, 1.0f);
}

void glTexCoord2f(GLfloat s, GLfloat t) {
    glTexCoord4f(s, t, 0.0f, 1.0f);
}

void glTexCoord1f(GLfloat s) {
    glTexCoord4f(s, 0.0f, 0.0f, 1.0f);
}

void glTexCoord2fv(const GLfloat *v) {
    if (v) glTexCoord2f(v[0], v[1]);
}

void glEnd(void) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (!ctx->in_begin) {
        ctx->error_flag = GL_INVALID_OPERATION;
        return;
    }
    ctx->in_begin = false;

    int n = ctx->vert_buf.count;
    gl_vertex_t *v = ctx->vert_buf.data;
    gl_vertex_t scratch[4];

    switch (ctx->prim_mode) {
        case GL_TRIANGLES:
            for (int i = 0; i + 2 < n; i += 3) {
                scratch[0] = v[i]; scratch[1] = v[i + 1]; scratch[2] = v[i + 2];
                gl_clip_and_render_polygon(ctx, scratch, 3);
            }
            break;
        case GL_QUADS:
            for (int i = 0; i + 3 < n; i += 4) {
                scratch[0] = v[i]; scratch[1] = v[i + 1];
                scratch[2] = v[i + 2]; scratch[3] = v[i + 3];
                gl_clip_and_render_polygon(ctx, scratch, 4);
            }
            break;
        case GL_QUAD_STRIP:
            for (int i = 0; i + 3 < n; i += 2) {
                scratch[0] = v[i]; scratch[1] = v[i + 1];
                scratch[2] = v[i + 3]; scratch[3] = v[i + 2];
                gl_clip_and_render_polygon(ctx, scratch, 4);
            }
            break;
        case GL_TRIANGLE_STRIP:
            for (int i = 0; i + 2 < n; i++) {
                if (i & 1) {
                    scratch[0] = v[i + 1]; scratch[1] = v[i]; scratch[2] = v[i + 2];
                } else {
                    scratch[0] = v[i]; scratch[1] = v[i + 1]; scratch[2] = v[i + 2];
                }
                gl_clip_and_render_polygon(ctx, scratch, 3);
            }
            break;
        case GL_TRIANGLE_FAN:
            for (int i = 1; i + 1 < n; i++) {
                scratch[0] = v[0]; scratch[1] = v[i]; scratch[2] = v[i + 1];
                gl_clip_and_render_polygon(ctx, scratch, 3);
            }
            break;
        case GL_POLYGON:
            if (n >= 3) {
                gl_clip_and_render_polygon(ctx, v, n);
            }
            break;
        case GL_LINES:
            for (int i = 0; i + 1 < n; i += 2) {
                gl_clip_and_render_line(ctx, &v[i], &v[i + 1]);
            }
            break;
        case GL_LINE_STRIP:
            for (int i = 0; i + 1 < n; i++) {
                gl_clip_and_render_line(ctx, &v[i], &v[i + 1]);
            }
            break;
        case GL_LINE_LOOP:
            for (int i = 0; i + 1 < n; i++) {
                gl_clip_and_render_line(ctx, &v[i], &v[i + 1]);
            }
            if (n >= 2) {
                gl_clip_and_render_line(ctx, &v[n - 1], &v[0]);
            }
            break;
        case GL_POINTS:
            for (int i = 0; i < n; i++) {
                gl_clip_and_render_point(ctx, &v[i]);
            }
            break;
        default:
            ctx->error_flag = GL_INVALID_ENUM;
            break;
    }
}

/* Vertex Arrays */
void glEnableClientState(GLenum array) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    switch (array) {
        case GL_VERTEX_ARRAY:        ctx->va_vertex_enabled   = GL_TRUE; break;
        case GL_COLOR_ARRAY:         ctx->va_color_enabled    = GL_TRUE; break;
        case GL_NORMAL_ARRAY:        ctx->va_normal_enabled   = GL_TRUE; break;
        case GL_TEXTURE_COORD_ARRAY: ctx->va_texcoord_enabled = GL_TRUE; break;
        default: ctx->error_flag = GL_INVALID_ENUM; break;
    }
}

void glDisableClientState(GLenum array) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    switch (array) {
        case GL_VERTEX_ARRAY:        ctx->va_vertex_enabled   = GL_FALSE; break;
        case GL_COLOR_ARRAY:         ctx->va_color_enabled    = GL_FALSE; break;
        case GL_NORMAL_ARRAY:        ctx->va_normal_enabled   = GL_FALSE; break;
        case GL_TEXTURE_COORD_ARRAY: ctx->va_texcoord_enabled = GL_FALSE; break;
        default: ctx->error_flag = GL_INVALID_ENUM; break;
    }
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->va_vertex_size   = size;
    ctx->va_vertex_type   = type;
    ctx->va_vertex_stride = stride ? stride : (GLsizei)(size * sizeof(GLfloat));
    ctx->va_vertex_ptr    = pointer;
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->va_color_size   = size;
    ctx->va_color_type   = type;
    ctx->va_color_stride = stride ? stride : (GLsizei)(size * (type == GL_UNSIGNED_BYTE ? sizeof(GLubyte) : sizeof(GLfloat)));
    ctx->va_color_ptr    = pointer;
}

void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *pointer) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->va_normal_type   = type;
    ctx->va_normal_stride = stride ? stride : (GLsizei)(3 * sizeof(GLfloat));
    ctx->va_normal_ptr    = pointer;
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->va_texcoord_size   = size;
    ctx->va_texcoord_type   = type;
    ctx->va_texcoord_stride = stride ? stride : (GLsizei)(size * sizeof(GLfloat));
    ctx->va_texcoord_ptr    = pointer;
}

static void process_array_element(GLContext *ctx, int index) {
    if (ctx->va_color_enabled && ctx->va_color_ptr) {
        const uint8_t *p = (const uint8_t *)ctx->va_color_ptr + index * ctx->va_color_stride;
        if (ctx->va_color_type == GL_FLOAT) {
            const GLfloat *f = (const GLfloat *)p;
            if (ctx->va_color_size == 4) glColor4f(f[0], f[1], f[2], f[3]);
            else glColor3f(f[0], f[1], f[2]);
        } else if (ctx->va_color_type == GL_UNSIGNED_BYTE) {
            if (ctx->va_color_size == 4) glColor4ub(p[0], p[1], p[2], p[3]);
            else glColor3ub(p[0], p[1], p[2]);
        }
    }

    if (ctx->va_normal_enabled && ctx->va_normal_ptr) {
        const GLfloat *n = (const GLfloat *)((const uint8_t *)ctx->va_normal_ptr + index * ctx->va_normal_stride);
        glNormal3f(n[0], n[1], n[2]);
    }

    if (ctx->va_texcoord_enabled && ctx->va_texcoord_ptr) {
        const GLfloat *t = (const GLfloat *)((const uint8_t *)ctx->va_texcoord_ptr + index * ctx->va_texcoord_stride);
        if (ctx->va_texcoord_size >= 2) glTexCoord2f(t[0], t[1]);
        else glTexCoord1f(t[0]);
    }

    if (ctx->va_vertex_enabled && ctx->va_vertex_ptr) {
        const GLfloat *v = (const GLfloat *)((const uint8_t *)ctx->va_vertex_ptr + index * ctx->va_vertex_stride);
        if (ctx->va_vertex_size == 2) glVertex2f(v[0], v[1]);
        else if (ctx->va_vertex_size == 3) glVertex3f(v[0], v[1], v[2]);
        else if (ctx->va_vertex_size == 4) glVertex4f(v[0], v[1], v[2], v[3]);
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || count <= 0) return;

    glBegin(mode);
    for (GLint i = 0; i < count; i++) {
        process_array_element(ctx, first + i);
    }
    glEnd();
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || count <= 0 || !indices) return;

    glBegin(mode);
    for (GLsizei i = 0; i < count; i++) {
        int idx = 0;
        if (type == GL_UNSIGNED_SHORT) {
            idx = ((const GLushort *)indices)[i];
        } else if (type == GL_UNSIGNED_INT) {
            idx = (int)((const GLuint *)indices)[i];
        } else if (type == GL_UNSIGNED_BYTE) {
            idx = ((const GLubyte *)indices)[i];
        }
        process_array_element(ctx, idx);
    }
    glEnd();
}
