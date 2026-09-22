#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <string.h>

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !ctx->color_buffer || !pixels || width <= 0 || height <= 0) return;

    for (int cy = 0; cy < height; cy++) {
        int sy = y + cy;
        if (sy < 0 || sy >= ctx->height) continue;

        for (int cx = 0; cx < width; cx++) {
            int sx = x + cx;
            if (sx < 0 || sx >= ctx->width) continue;

            size_t src_idx = (size_t)sy * ctx->width + sx;
            size_t dst_idx = (size_t)cy * width + cx;

            if (format == GL_DEPTH_COMPONENT) {
                uint16_t z_val = ctx->z_buffer[src_idx];
                if (type == GL_UNSIGNED_SHORT) {
                    ((uint16_t *)pixels)[dst_idx] = z_val;
                } else if (type == GL_FLOAT) {
                    ((float *)pixels)[dst_idx] = (float)z_val / 65535.0f;
                }
                continue;
            }

            uint32_t c = ctx->color_buffer[src_idx];
            uint8_t a = (uint8_t)((c >> 24) & 0xFF);
            uint8_t r = (uint8_t)((c >> 16) & 0xFF);
            uint8_t g = (uint8_t)((c >> 8)  & 0xFF);
            uint8_t b = (uint8_t)(c         & 0xFF);

            if (type == GL_UNSIGNED_BYTE) {
                uint8_t *dst = (uint8_t *)pixels;
                if (format == GL_RGBA) {
                    dst[dst_idx * 4 + 0] = r;
                    dst[dst_idx * 4 + 1] = g;
                    dst[dst_idx * 4 + 2] = b;
                    dst[dst_idx * 4 + 3] = a;
                } else if (format == GL_BGRA) {
                    dst[dst_idx * 4 + 0] = b;
                    dst[dst_idx * 4 + 1] = g;
                    dst[dst_idx * 4 + 2] = r;
                    dst[dst_idx * 4 + 3] = a;
                } else if (format == GL_RGB) {
                    dst[dst_idx * 3 + 0] = r;
                    dst[dst_idx * 3 + 1] = g;
                    dst[dst_idx * 3 + 2] = b;
                } else if (format == GL_BGR) {
                    dst[dst_idx * 3 + 0] = b;
                    dst[dst_idx * 3 + 1] = g;
                    dst[dst_idx * 3 + 2] = r;
                }
            } else if (type == GL_UNSIGNED_INT_8_8_8_8 || type == GL_UNSIGNED_INT) {
                ((uint32_t *)pixels)[dst_idx] = c;
            }
        }
    }
}

