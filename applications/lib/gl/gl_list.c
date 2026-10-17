#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <memory/kmalloc.h>
#include <string.h>

GLuint glGenLists(GLsizei range) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || range <= 0) return 0;

    /* Ensure display list registry has space */
    while (ctx->list_count + range >= ctx->list_capacity) {
        int new_cap = ctx->list_capacity ? ctx->list_capacity * 2 : 16;
        gl_display_list_t **new_arr = krealloc(ctx->display_lists, (size_t)new_cap * sizeof(gl_display_list_t *));
        if (!new_arr) return 0;
        ctx->display_lists = new_arr;
        ctx->list_capacity = new_cap;
    }

    GLuint base = (GLuint)(ctx->list_count + 1);
    for (GLsizei i = 0; i < range; i++) {
        gl_display_list_t *dl = kmalloc(sizeof(gl_display_list_t));
        if (dl) {
            memset(dl, 0, sizeof(gl_display_list_t));
            dl->valid = true;
            ctx->display_lists[ctx->list_count++] = dl;
        }
    }
    return base;
}

void glNewList(GLuint list, GLenum mode) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || list == 0) return;

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
    (void)type;
    if (n <= 0 || !lists) return;
    const GLuint *l = (const GLuint *)lists;
    for (GLsizei i = 0; i < n; i++) {
        glCallList(l[i]);
    }
}

void glDeleteLists(GLuint list, GLsizei range) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || list == 0) return;

    for (GLsizei i = 0; i < range; i++) {
        int idx = (int)(list + i) - 1;
        if (idx >= 0 && idx < ctx->list_count && ctx->display_lists[idx]) {
            ctx->display_lists[idx]->valid = false;
            if (ctx->display_lists[idx]->commands) {
                kfree(ctx->display_lists[idx]->commands);
                ctx->display_lists[idx]->commands = NULL;
            }
        }
    }
}

GLboolean glIsList(GLuint list) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || list == 0 || (int)list > ctx->list_count) return GL_FALSE;
    gl_display_list_t *dl = ctx->display_lists[list - 1];
    return (dl && dl->valid) ? GL_TRUE : GL_FALSE;
}

