#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/ipo_gl.h>
#include <GL/gl_math.h>
#include <memory/kmalloc.h>
#include <string.h>

#define MAT_IDENTITY { \
    1.0f, 0.0f, 0.0f, 0.0f, \
    0.0f, 1.0f, 0.0f, 0.0f, \
    0.0f, 0.0f, 1.0f, 0.0f, \
    0.0f, 0.0f, 0.0f, 1.0f  \
}

void gl_matrix_init_stack(gl_matrix_stack_t *stack) {
    stack->capacity = 16;
    stack->data = kmalloc((size_t)stack->capacity * 16 * sizeof(GLfloat));
    stack->top = 0;
    if (stack->data) {
        GLfloat id[16] = MAT_IDENTITY;
        memcpy(stack->data, id, 16 * sizeof(GLfloat));
    }
}

void gl_matrix_free_stack(gl_matrix_stack_t *stack) {
    if (stack->data) {
        kfree(stack->data);
        stack->data = NULL;
    }
    stack->top = 0;
    stack->capacity = 0;
}

void gl_matrix_push(gl_matrix_stack_t *stack) {
    if (stack->top + 1 >= stack->capacity) {
        int new_cap = stack->capacity * 2;
        GLfloat *new_data = krealloc(stack->data, (size_t)new_cap * 16 * sizeof(GLfloat));
        if (!new_data) return;
        stack->data = new_data;
        stack->capacity = new_cap;
    }
    GLfloat *curr = stack->data + stack->top * 16;
    GLfloat *next = stack->data + (stack->top + 1) * 16;
    memcpy(next, curr, 16 * sizeof(GLfloat));
    stack->top++;
}

void gl_matrix_pop(gl_matrix_stack_t *stack) {
    if (stack->top > 0) {
        stack->top--;
    }
}

GLfloat *gl_matrix_current(gl_matrix_stack_t *stack) {
    if (!stack->data) return NULL;
    return stack->data + stack->top * 16;
}

void gl_matrix_mult(GLfloat *dst, const GLfloat *a, const GLfloat *b) {
    GLfloat res[16];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            res[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] +
                             a[1 * 4 + r] * b[c * 4 + 1] +
                             a[2 * 4 + r] * b[c * 4 + 2] +
                             a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    memcpy(dst, res, 16 * sizeof(GLfloat));
}

static gl_matrix_stack_t *get_current_stack(GLContext *ctx) {
    if (!ctx) return NULL;
    switch (ctx->matrix_mode) {
        case GL_MODELVIEW:  return &ctx->modelview_stack;
        case GL_PROJECTION: return &ctx->projection_stack;
        case GL_TEXTURE:    return &ctx->texture_stack;
        default:            return &ctx->modelview_stack;
    }
}

void glMatrixMode(GLenum mode) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (mode == GL_MODELVIEW || mode == GL_PROJECTION || mode == GL_TEXTURE) {
        ctx->matrix_mode = mode;
    } else {
        ctx->error_flag = GL_INVALID_ENUM;
    }
}

void glLoadIdentity(void) {
    GLContext *ctx = gl_get_current_context();
    gl_matrix_stack_t *st = get_current_stack(ctx);
    if (!st) return;
    GLfloat id[16] = MAT_IDENTITY;
    memcpy(gl_matrix_current(st), id, 16 * sizeof(GLfloat));
}

void glLoadMatrixf(const GLfloat *m) {
    GLContext *ctx = gl_get_current_context();
    gl_matrix_stack_t *st = get_current_stack(ctx);
    if (!st || !m) return;
    memcpy(gl_matrix_current(st), m, 16 * sizeof(GLfloat));
}

void glMultMatrixf(const GLfloat *m) {
    GLContext *ctx = gl_get_current_context();
    gl_matrix_stack_t *st = get_current_stack(ctx);
    if (!st || !m) return;
    GLfloat *curr = gl_matrix_current(st);
    gl_matrix_mult(curr, curr, m);
}

void glPushMatrix(void) {
    GLContext *ctx = gl_get_current_context();
    gl_matrix_stack_t *st = get_current_stack(ctx);
    if (st) gl_matrix_push(st);
}

void glPopMatrix(void) {
    GLContext *ctx = gl_get_current_context();
    gl_matrix_stack_t *st = get_current_stack(ctx);
    if (st) {
        if (st->top == 0) {
            if (ctx) ctx->error_flag = GL_STACK_UNDERFLOW;
            return;
        }
        gl_matrix_pop(st);
    }
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    GLfloat m[16] = MAT_IDENTITY;
    m[12] = x;
    m[13] = y;
    m[14] = z;
    glMultMatrixf(m);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    GLfloat m[16] = MAT_IDENTITY;
    m[0]  = x;
    m[5]  = y;
    m[10] = z;
    glMultMatrixf(m);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    GLfloat len = gl_sqrtf(x * x + y * y + z * z);
    if (len == 0.0f) return;
    x /= len; y /= len; z /= len;

    GLfloat rad = angle * 3.14159265358979323846f / 180.0f;
    GLfloat c = gl_cosf(rad);
    GLfloat s = gl_sinf(rad);
    GLfloat t = 1.0f - c;

    GLfloat m[16];
    m[0] = t * x * x + c;
    m[1] = t * x * y + s * z;
    m[2] = t * x * z - s * y;
    m[3] = 0.0f;

    m[4] = t * x * y - s * z;
    m[5] = t * y * y + c;
    m[6] = t * y * z + s * x;
    m[7] = 0.0f;

    m[8] = t * x * z + s * y;
    m[9] = t * y * z - s * x;
    m[10] = t * z * z + c;
    m[11] = 0.0f;

    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;

    glMultMatrixf(m);
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble nearVal, GLdouble farVal) {
    if (nearVal <= 0.0 || farVal <= 0.0 || left == right || bottom == top || nearVal == farVal) return;

    GLfloat m[16] = {0};
    m[0] = (GLfloat)((2.0 * nearVal) / (right - left));
    m[5] = (GLfloat)((2.0 * nearVal) / (top - bottom));
    m[8] = (GLfloat)((right + left) / (right - left));
    m[9] = (GLfloat)((top + bottom) / (top - bottom));
    m[10] = (GLfloat)(-(farVal + nearVal) / (farVal - nearVal));
    m[11] = -1.0f;
    m[14] = (GLfloat)(-(2.0 * farVal * nearVal) / (farVal - nearVal));

    glMultMatrixf(m);
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble nearVal, GLdouble farVal) {
    if (left == right || bottom == top || nearVal == farVal) return;

    GLfloat m[16] = {0};
    m[0]  = (GLfloat)(2.0 / (right - left));
    m[5]  = (GLfloat)(2.0 / (top - bottom));
    m[10] = (GLfloat)(-2.0 / (farVal - nearVal));
    m[12] = (GLfloat)(-(right + left) / (right - left));
    m[13] = (GLfloat)(-(top + bottom) / (top - bottom));
    m[14] = (GLfloat)(-(farVal + nearVal) / (farVal - nearVal));
    m[15] = 1.0f;

    glMultMatrixf(m);
}

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar) {
    GLdouble ymax = zNear * gl_tand(fovy * 3.14159265358979323846 / 360.0);
    GLdouble ymin = -ymax;
    GLdouble xmin = ymin * aspect;
    GLdouble xmax = ymax * aspect;
    glFrustum(xmin, xmax, ymin, ymax, zNear, zFar);
}

void gluLookAt(GLdouble eyex, GLdouble eyey, GLdouble eyez,
               GLdouble centerx, GLdouble centery, GLdouble centerz,
               GLdouble upx, GLdouble upy, GLdouble upz) {
    GLfloat forward[3], side[3], up[3];
    forward[0] = (GLfloat)(centerx - eyex);
    forward[1] = (GLfloat)(centery - eyey);
    forward[2] = (GLfloat)(centerz - eyez);

    GLfloat fnorm = gl_sqrtf(forward[0]*forward[0] + forward[1]*forward[1] + forward[2]*forward[2]);
    if (fnorm > 0.0f) { forward[0] /= fnorm; forward[1] /= fnorm; forward[2] /= fnorm; }

    up[0] = (GLfloat)upx; up[1] = (GLfloat)upy; up[2] = (GLfloat)upz;

    /* side = forward x up */
    side[0] = forward[1] * up[2] - forward[2] * up[1];
    side[1] = forward[2] * up[0] - forward[0] * up[2];
    side[2] = forward[0] * up[1] - forward[1] * up[0];
    GLfloat snorm = gl_sqrtf(side[0]*side[0] + side[1]*side[1] + side[2]*side[2]);
    if (snorm > 0.0f) { side[0] /= snorm; side[1] /= snorm; side[2] /= snorm; }

    /* up = side x forward */
    up[0] = side[1] * forward[2] - side[2] * forward[1];
    up[1] = side[2] * forward[0] - side[0] * forward[2];
    up[2] = side[0] * forward[1] - side[1] * forward[0];

    GLfloat m[16] = MAT_IDENTITY;
    m[0] = side[0];    m[4] = side[1];    m[8]  = side[2];
    m[1] = up[0];      m[5] = up[1];      m[9]  = up[2];
    m[2] = -forward[0];m[6] = -forward[1];m[10] = -forward[2];

    glMultMatrixf(m);
    glTranslatef((GLfloat)-eyex, (GLfloat)-eyey, (GLfloat)-eyez);
}

void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top) {
    glOrtho(left, right, bottom, top, -1.0, 1.0);
}
