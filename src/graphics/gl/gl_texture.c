#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <memory/kmalloc.h>
#include <string.h>

static gl_texture_t *get_texture(GLContext *ctx, GLuint id) {
    if (!ctx || id == 0) return NULL;
    for (int i = 0; i < ctx->texture_count; i++) {
        if (ctx->textures[i] && ctx->textures[i]->id == id && ctx->textures[i]->in_use) {
            return ctx->textures[i];
        }
    }
    return NULL;
}

static gl_texture_t *get_or_create_texture(GLContext *ctx, GLuint id) {
    gl_texture_t *t = get_texture(ctx, id);
    if (t) return t;

    /* Expand texture registry array without limits */
    if (ctx->texture_count >= ctx->texture_capacity) {
        int new_cap = ctx->texture_capacity ? ctx->texture_capacity * 2 : 16;
        gl_texture_t **new_arr = krealloc(ctx->textures, (size_t)new_cap * sizeof(gl_texture_t *));
        if (!new_arr) return NULL;
        ctx->textures = new_arr;
        ctx->texture_capacity = new_cap;
    }

    t = kmalloc(sizeof(gl_texture_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(gl_texture_t));

    t->id         = id;
    t->target     = GL_TEXTURE_2D;
    t->min_filter = GL_NEAREST;
    t->mag_filter = GL_NEAREST;
    t->wrap_s     = GL_REPEAT;
    t->wrap_t     = GL_REPEAT;
    t->wrap_r     = GL_REPEAT;
    t->in_use     = true;

    ctx->textures[ctx->texture_count++] = t;
    return t;
}

void glGenTextures(GLsizei n, GLuint *textures) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !textures || n <= 0) return;

    static GLuint next_id = 1;
    for (GLsizei i = 0; i < n; i++) {
        GLuint id = next_id++;
        get_or_create_texture(ctx, id);
        textures[i] = id;
    }
}

void glBindTexture(GLenum target, GLuint texture) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;

    if (texture != 0) {
        get_or_create_texture(ctx, texture);
    }

    if (target == GL_TEXTURE_2D) {
        ctx->current_texture_2d = texture;
    } else if (target == GL_TEXTURE_3D) {
        ctx->current_texture_3d = texture;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !textures || n <= 0) return;

    for (GLsizei i = 0; i < n; i++) {
        gl_texture_t *t = get_texture(ctx, textures[i]);
        if (t) {
            t->in_use = false;
            if (t->pixels) {
                kfree(t->pixels);
                t->pixels = NULL;
            }
        }
    }
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;

    GLuint tid = (target == GL_TEXTURE_3D) ? ctx->current_texture_3d : ctx->current_texture_2d;
    gl_texture_t *t = get_texture(ctx, tid);
    if (!t) return;

    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: t->min_filter = (GLenum)param; break;
        case GL_TEXTURE_MAG_FILTER: t->mag_filter = (GLenum)param; break;
        case GL_TEXTURE_WRAP_S:     t->wrap_s     = (GLenum)param; break;
        case GL_TEXTURE_WRAP_T:     t->wrap_t     = (GLenum)param; break;
        case GL_TEXTURE_WRAP_R:     t->wrap_r     = (GLenum)param; break;
        default: ctx->error_flag = GL_INVALID_ENUM; break;
    }
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    glTexParameteri(target, pname, (GLint)param);
}

void glTexEnvi(GLenum target, GLenum pname, GLint param) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE) {
        ctx->tex_env_mode = (GLenum)param;
    } else {
        ctx->error_flag = GL_INVALID_ENUM;
    }
}

void glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
    glTexEnvi(target, pname, (GLint)param);
}

/* Unpack incoming pixel formats to 32-bit ARGB uint32 */
static uint32_t unpack_pixel(const void *src, GLenum format, GLenum type, size_t index) {
    if (!src) return 0xFFFFFFFFu;

    if (type == GL_UNSIGNED_BYTE) {
        const uint8_t *p = (const uint8_t *)src;
        if (format == GL_RGBA) {
            size_t off = index * 4;
            return ((uint32_t)p[off + 3] << 24) | ((uint32_t)p[off] << 16) | ((uint32_t)p[off + 1] << 8) | p[off + 2];
        } else if (format == GL_BGRA) {
            size_t off = index * 4;
            return ((uint32_t)p[off + 3] << 24) | ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 1] << 8) | p[off];
        } else if (format == GL_RGB) {
            size_t off = index * 3;
            return 0xFF000000u | ((uint32_t)p[off] << 16) | ((uint32_t)p[off + 1] << 8) | p[off + 2];
        } else if (format == GL_BGR) {
            size_t off = index * 3;
            return 0xFF000000u | ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 1] << 8) | p[off];
        } else if (format == GL_LUMINANCE) {
            uint32_t l = p[index];
            return 0xFF000000u | (l << 16) | (l << 8) | l;
        } else if (format == GL_LUMINANCE_ALPHA) {
            size_t off = index * 2;
            uint32_t l = p[off];
            uint32_t a = p[off + 1];
            return (a << 24) | (l << 16) | (l << 8) | l;
        } else if (format == GL_ALPHA) {
            uint32_t a = p[index];
            return (a << 24) | 0x00FFFFFFu;
        }
    } else if (type == GL_UNSIGNED_SHORT_5_6_5) {
        uint16_t val = ((const uint16_t *)src)[index];
        uint32_t r = (uint32_t)((val >> 11) & 0x1F) * 255 / 31;
        uint32_t g = (uint32_t)((val >> 5)  & 0x3F) * 255 / 63;
        uint32_t b = (uint32_t)(val         & 0x1F) * 255 / 31;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    } else if (type == GL_UNSIGNED_INT_8_8_8_8 || type == GL_UNSIGNED_INT) {
        return ((const uint32_t *)src)[index];
    }

    return 0xFFFFFFFFu;
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels) {
    (void)level; (void)internalformat; (void)border;
    GLContext *ctx = gl_get_current_context();
    if (!ctx || width <= 0 || height <= 0) return;

    gl_texture_t *t = get_or_create_texture(ctx, ctx->current_texture_2d);
    if (!t) return;

    size_t num_pixels = (size_t)width * (size_t)height;
    if (t->pixels) {
        kfree(t->pixels);
    }
    t->pixels = kmalloc(num_pixels * sizeof(uint32_t));
    if (!t->pixels) return;

    t->target = target;
    t->width  = width;
    t->height = height;
    t->depth  = 1;
    t->format = format;
    t->type   = type;

    if (pixels) {
        for (size_t i = 0; i < num_pixels; i++) {
            t->pixels[i] = unpack_pixel(pixels, format, type, i);
        }
    } else {
        memset(t->pixels, 0xFF, num_pixels * sizeof(uint32_t));
    }
}

void glTexImage1D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLint border, GLenum format, GLenum type, const GLvoid *pixels) {
    glTexImage2D(target, level, internalformat, width, 1, border, format, type, pixels);
}

void glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const GLvoid *pixels) {
    (void)level; (void)internalformat; (void)border;
    GLContext *ctx = gl_get_current_context();
    if (!ctx || width <= 0 || height <= 0 || depth <= 0) return;

    gl_texture_t *t = get_or_create_texture(ctx, ctx->current_texture_3d);
    if (!t) return;

    size_t num_pixels = (size_t)width * (size_t)height * (size_t)depth;
    if (t->pixels) {
        kfree(t->pixels);
    }
    t->pixels = kmalloc(num_pixels * sizeof(uint32_t));
    if (!t->pixels) return;

    t->target = target;
    t->width  = width;
    t->height = height;
    t->depth  = depth;
    t->format = format;
    t->type   = type;

    if (pixels) {
        for (size_t i = 0; i < num_pixels; i++) {
            t->pixels[i] = unpack_pixel(pixels, format, type, i);
        }
    } else {
        memset(t->pixels, 0xFF, num_pixels * sizeof(uint32_t));
    }
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) {
    (void)target; (void)level;
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !pixels) return;

    gl_texture_t *t = get_texture(ctx, ctx->current_texture_2d);
    if (!t || !t->pixels) return;

    for (int y = 0; y < height; y++) {
        int dy = yoffset + y;
        if (dy < 0 || dy >= t->height) continue;
        for (int x = 0; x < width; x++) {
            int dx = xoffset + x;
            if (dx < 0 || dx >= t->width) continue;
            size_t src_idx = (size_t)y * width + x;
            size_t dst_idx = (size_t)dy * t->width + dx;
            t->pixels[dst_idx] = unpack_pixel(pixels, format, type, src_idx);
        }
    }
}

void glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels) {
    glTexSubImage2D(target, level, xoffset, 0, width, 1, format, type, pixels);
}

void glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const GLvoid *pixels) {
    (void)target; (void)level;
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !pixels) return;

    gl_texture_t *t = get_texture(ctx, ctx->current_texture_3d);
    if (!t || !t->pixels) return;

    for (int z = 0; z < depth; z++) {
        int dz = zoffset + z;
        if (dz < 0 || dz >= t->depth) continue;
        for (int y = 0; y < height; y++) {
            int dy = yoffset + y;
            if (dy < 0 || dy >= t->height) continue;
            for (int x = 0; x < width; x++) {
                int dx = xoffset + x;
                if (dx < 0 || dx >= t->width) continue;
                size_t src_idx = (size_t)z * width * height + (size_t)y * width + x;
                size_t dst_idx = (size_t)dz * t->width * t->height + (size_t)dy * t->width + dx;
                t->pixels[dst_idx] = unpack_pixel(pixels, format, type, src_idx);
            }
        }
    }
}

void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
    (void)level; (void)internalformat; (void)border;
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !ctx->color_buffer) return;

    gl_texture_t *t = get_or_create_texture(ctx, ctx->current_texture_2d);
    if (!t) return;

    size_t num = (size_t)width * (size_t)height;
    if (t->pixels) kfree(t->pixels);
    t->pixels = kmalloc(num * sizeof(uint32_t));
    if (!t->pixels) return;

    t->target = target;
    t->width  = width;
    t->height = height;
    t->depth  = 1;

    for (int cy = 0; cy < height; cy++) {
        int sy = y + cy;
        for (int cx = 0; cx < width; cx++) {
            int sx = x + cx;
            uint32_t p = 0;
            if (sx >= 0 && sx < ctx->width && sy >= 0 && sy < ctx->height) {
                p = ctx->color_buffer[(size_t)sy * ctx->width + sx];
            }
            t->pixels[(size_t)cy * width + cx] = p;
        }
    }
}

void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
    (void)target; (void)level;
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !ctx->color_buffer) return;

    gl_texture_t *t = get_texture(ctx, ctx->current_texture_2d);
    if (!t || !t->pixels) return;

    for (int cy = 0; cy < height; cy++) {
        int sy = y + cy;
        int dy = yoffset + cy;
        if (dy < 0 || dy >= t->height) continue;
        for (int cx = 0; cx < width; cx++) {
            int sx = x + cx;
            int dx = xoffset + cx;
            if (dx < 0 || dx >= t->width) continue;
            uint32_t p = 0;
            if (sx >= 0 && sx < ctx->width && sy >= 0 && sy < ctx->height) {
                p = ctx->color_buffer[(size_t)sy * ctx->width + sx];
            }
            t->pixels[(size_t)dy * t->width + dx] = p;
        }
    }
}

