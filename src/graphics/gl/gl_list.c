#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <memory/kmalloc.h>
#include <limits.h>
#include <string.h>

GLuint glGenLists(GLsizei range) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return 0;
    if (range < 0) {
        ctx->error_flag = GL_INVALID_VALUE;
        return 0;
    }
    if (range == 0) return 0;
    if (range > INT_MAX - ctx->list_count) {
        ctx->error_flag = GL_OUT_OF_MEMORY;
        return 0;
    }

    int needed = ctx->list_count + range;
    while (needed > ctx->list_capacity) {
        int old_cap = ctx->list_capacity;
        int new_cap = old_cap ? old_cap * 2 : 16;
        if (new_cap < old_cap) new_cap = needed;
        gl_display_list_t **new_arr = krealloc(
            ctx->display_lists, (size_t)new_cap * sizeof(gl_display_list_t *));
        if (!new_arr) {
            ctx->error_flag = GL_OUT_OF_MEMORY;
            return 0;
        }
        memset(new_arr + old_cap, 0,
               (size_t)(new_cap - old_cap) * sizeof(gl_display_list_t *));
        ctx->display_lists = new_arr;
        ctx->list_capacity = new_cap;
    }

    int start_idx = ctx->list_count;
    for (GLsizei i = 0; i < range; i++) {
        gl_display_list_t *dl = kmalloc(sizeof(gl_display_list_t));
        if (!dl) {
            for (int j = start_idx; j < start_idx + i; j++) {
                kfree(ctx->display_lists[j]);
                ctx->display_lists[j] = NULL;
            }
            ctx->list_count = start_idx;
            ctx->error_flag = GL_OUT_OF_MEMORY;
            return 0;
        }
        memset(dl, 0, sizeof(gl_display_list_t));
        dl->valid = true;
        ctx->display_lists[start_idx + i] = dl;
    }

    ctx->list_count = start_idx + range;
    return (GLuint)(start_idx + 1);
}

void glNewList(GLuint list, GLenum mode) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (ctx->is_compiling_list) {
        ctx->error_flag = GL_INVALID_OPERATION;
        return;
    }
    if (list == 0 || (int)list > ctx->list_count || !ctx->display_lists[list - 1]) {
        ctx->error_flag = GL_INVALID_VALUE;
        return;
    }
    if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) {
        ctx->error_flag = GL_INVALID_ENUM;
        return;
    }

    ctx->current_list = list;
    ctx->list_compile_mode = mode;
    ctx->is_compiling_list = true;
}

void glEndList(void) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (!ctx->is_compiling_list) {
        ctx->error_flag = GL_INVALID_OPERATION;
        return;
    }
    ctx->is_compiling_list = false;
    ctx->current_list = 0;
}

void glCallList(GLuint list) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || list == 0 || (int)list > ctx->list_count) return;

    gl_display_list_t *dl = ctx->display_lists[list - 1];
    if (!dl || !dl->valid) return;
}

void glCallLists(GLsizei n, GLenum type, const GLvoid *lists) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (n < 0) {
        ctx->error_flag = GL_INVALID_VALUE;
        return;
    }
    if (n == 0 || !lists) return;

    for (GLsizei i = 0; i < n; i++) {
        GLuint id = 0;
        switch (type) {
            case GL_BYTE:
                id = (GLuint)(GLint)((const GLbyte *)lists)[i];
                break;
            case GL_UNSIGNED_BYTE:
                id = (GLuint)((const GLubyte *)lists)[i];
                break;
            case GL_SHORT:
                id = (GLuint)(GLint)((const GLshort *)lists)[i];
                break;
            case GL_UNSIGNED_SHORT:
                id = (GLuint)((const GLushort *)lists)[i];
                break;
            case GL_INT:
                id = (GLuint)((const GLint *)lists)[i];
                break;
            case GL_UNSIGNED_INT:
                id = ((const GLuint *)lists)[i];
                break;
            case GL_FLOAT:
                id = (GLuint)(GLint)((const GLfloat *)lists)[i];
                break;
            default:
                ctx->error_flag = GL_INVALID_ENUM;
                return;
        }
        glCallList(id);
    }
}

void glDeleteLists(GLuint list, GLsizei range) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (range < 0) {
        ctx->error_flag = GL_INVALID_VALUE;
        return;
    }
    if (list == 0 || range == 0) return;

    for (GLsizei i = 0; i < range; i++) {
        if ((int)list - 1 > INT_MAX - i) break;
        int idx = (int)(list - 1) + i;
        if (idx >= 0 && idx < ctx->list_count && ctx->display_lists[idx]) {
            if (ctx->display_lists[idx]->commands) {
                kfree(ctx->display_lists[idx]->commands);
            }
            kfree(ctx->display_lists[idx]);
            ctx->display_lists[idx] = NULL;
        }
    }
}

GLboolean glIsList(GLuint list) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || list == 0 || (int)list > ctx->list_count) return GL_FALSE;
    gl_display_list_t *dl = ctx->display_lists[list - 1];
    return (dl && dl->valid) ? GL_TRUE : GL_FALSE;
}

