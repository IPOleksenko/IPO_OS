#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <GL/gl_math.h>

void glPointSize(GLfloat size) {
    GLContext *ctx = gl_get_current_context();
    if (ctx && size > 0.0f) ctx->point_size = size;
}

void glLineWidth(GLfloat width) {
    GLContext *ctx = gl_get_current_context();
    if (ctx && width > 0.0f) ctx->line_width = width;
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
    ctx->alpha_ref  = ref;
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
    if (!ctx) return;
    ctx->logic_opcode = opcode;
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

/* Sample 2D texture with bilinear filtering and wrapping */
static uint32_t sample_tex2d(const gl_texture_t *tex, float s, float t) {
    if (!tex || !tex->pixels || tex->width <= 0 || tex->height <= 0) return 0xFFFFFFFFu;

    int w = tex->width;
    int h = tex->height;

    /* S wrapping */
    if (tex->wrap_s == GL_CLAMP_TO_EDGE) {
        if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
    } else if (tex->wrap_s == GL_REPEAT) {
        s = s - (float)((int)s);
        if (s < 0.0f) s += 1.0f;
    }

    /* T wrapping */
    if (tex->wrap_t == GL_CLAMP_TO_EDGE) {
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    } else if (tex->wrap_t == GL_REPEAT) {
        t = t - (float)((int)t);
        if (t < 0.0f) t += 1.0f;
    }

    float u = s * (float)w - 0.5f;
    float v = t * (float)h - 0.5f;

    if (tex->mag_filter == GL_NEAREST) {
        int ix = (int)(u + 0.5f);
        int iy = (int)(v + 0.5f);
        if (ix < 0) ix = 0; else if (ix >= w) ix = w - 1;
        if (iy < 0) iy = 0; else if (iy >= h) iy = h - 1;
        return tex->pixels[(size_t)iy * w + ix];
    }

    /* Bilinear filtering (4-sample interpolation) */
    int x0 = gl_floor_to_int(u);
    int y0 = gl_floor_to_int(v);
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    float fx = u - (float)x0;
    float fy = v - (float)y0;

    if (x0 < 0) x0 = 0; else if (x0 >= w) x0 = w - 1;
    if (x1 < 0) x1 = 0; else if (x1 >= w) x1 = w - 1;
    if (y0 < 0) y0 = 0; else if (y0 >= h) y0 = h - 1;
    if (y1 < 0) y1 = 0; else if (y1 >= h) y1 = h - 1;

    uint32_t c00 = tex->pixels[(size_t)y0 * w + x0];
    uint32_t c10 = tex->pixels[(size_t)y0 * w + x1];
    uint32_t c01 = tex->pixels[(size_t)y1 * w + x0];
    uint32_t c11 = tex->pixels[(size_t)y1 * w + x1];

    float w00 = (1.0f - fx) * (1.0f - fy);
    float w10 = fx * (1.0f - fy);
    float w01 = (1.0f - fx) * fy;
    float w11 = fx * fy;

    float a = ((c00 >> 24) & 0xFF) * w00 + ((c10 >> 24) & 0xFF) * w10 + ((c01 >> 24) & 0xFF) * w01 + ((c11 >> 24) & 0xFF) * w11;
    float r = ((c00 >> 16) & 0xFF) * w00 + ((c10 >> 16) & 0xFF) * w10 + ((c01 >> 16) & 0xFF) * w01 + ((c11 >> 16) & 0xFF) * w11;
    float g = ((c00 >> 8)  & 0xFF) * w00 + ((c10 >> 8)  & 0xFF) * w10 + ((c01 >> 8)  & 0xFF) * w01 + ((c11 >> 8)  & 0xFF) * w11;
    float b = (c00         & 0xFF) * w00 + (c10         & 0xFF) * w10 + (c01         & 0xFF) * w01 + (c11         & 0xFF) * w11;

    uint32_t ia = (uint32_t)a; uint32_t ir = (uint32_t)r;
    uint32_t ig = (uint32_t)g; uint32_t ib = (uint32_t)b;
    if (ia > 255) ia = 255; if (ir > 255) ir = 255; if (ig > 255) ig = 255; if (ib > 255) ib = 255;

    return (ia << 24) | (ir << 16) | (ig << 8) | ib;
}

static inline bool test_depth(GLenum func, uint16_t z_frag, uint16_t z_buf) {
    switch (func) {
        case GL_NEVER:    return false;
        case GL_LESS:     return z_frag < z_buf;
        case GL_EQUAL:    return z_frag == z_buf;
        case GL_LEQUAL:   return z_frag <= z_buf;
        case GL_GREATER:  return z_frag > z_buf;
        case GL_NOTEQUAL: return z_frag != z_buf;
        case GL_GEQUAL:   return z_frag >= z_buf;
        case GL_ALWAYS:   return true;
        default:          return z_frag < z_buf;
    }
}

static inline bool test_stencil(GLenum func, int ref, uint8_t buf_val, GLuint mask) {
    uint8_t r = (uint8_t)(ref & mask);
    uint8_t b = (uint8_t)(buf_val & mask);
    switch (func) {
        case GL_NEVER:    return false;
        case GL_LESS:     return r < b;
        case GL_LEQUAL:   return r <= b;
        case GL_GREATER:  return r > b;
        case GL_GEQUAL:   return r >= b;
        case GL_EQUAL:    return r == b;
        case GL_NOTEQUAL: return r != b;
        case GL_ALWAYS:   return true;
        default:          return true;
    }
}

static inline uint8_t apply_stencil_op(GLenum op, uint8_t current, int ref, GLuint writemask) {
    uint8_t res = current;
    switch (op) {
        case GL_KEEP:    res = current; break;
        case GL_ZERO:    res = 0; break;
        case GL_REPLACE: res = (uint8_t)ref; break;
        case GL_INCR:    if (current < 255) res = current + 1; break;
        case GL_DECR:    if (current > 0)   res = current - 1; break;
        case GL_INVERT:  res = ~current; break;
    }
    return (current & ~writemask) | (res & writemask);
}

/* Blend factor evaluation */
static void get_blend_factors(GLenum factor, float rs, float gs, float bs, float as,
                              float rd, float gd, float bd, float ad,
                              float *fr, float *fg, float *fb, float *fa) {
    switch (factor) {
        case GL_ZERO:                *fr = 0.0f; *fg = 0.0f; *fb = 0.0f; *fa = 0.0f; break;
        case GL_ONE:                 *fr = 1.0f; *fg = 1.0f; *fb = 1.0f; *fa = 1.0f; break;
        case GL_SRC_COLOR:           *fr = rs;   *fg = gs;   *fb = bs;   *fa = as;   break;
        case GL_ONE_MINUS_SRC_COLOR: *fr = 1.0f-rs; *fg = 1.0f-gs; *fb = 1.0f-bs; *fa = 1.0f-as; break;
        case GL_SRC_ALPHA:           *fr = as;   *fg = as;   *fb = as;   *fa = as;   break;
        case GL_ONE_MINUS_SRC_ALPHA: *fr = 1.0f-as; *fg = 1.0f-as; *fb = 1.0f-as; *fa = 1.0f-as; break;
        case GL_DST_ALPHA:           *fr = ad;   *fg = ad;   *fb = ad;   *fa = ad;   break;
        case GL_ONE_MINUS_DST_ALPHA: *fr = 1.0f-ad; *fg = 1.0f-ad; *fb = 1.0f-ad; *fa = 1.0f-ad; break;
        case GL_DST_COLOR:           *fr = rd;   *fg = gd;   *fb = bd;   *fa = ad;   break;
        case GL_ONE_MINUS_DST_COLOR: *fr = 1.0f-rd; *fg = 1.0f-gd; *fb = 1.0f-bd; *fa = 1.0f-ad; break;
        case GL_SRC_ALPHA_SATURATE: {
            float f = (as < (1.0f - ad)) ? as : (1.0f - ad);
            *fr = f; *fg = f; *fb = f; *fa = 1.0f;
            break;
        }
        default:                     *fr = 1.0f; *fg = 1.0f; *fb = 1.0f; *fa = 1.0f; break;
    }
}

/* Execute per-fragment rasterization pipeline */
static void write_fragment(GLContext *ctx, int x, int y, uint16_t z_val, float r, float g, float b, float a, float eye_z) {
    if (x < 0 || x >= ctx->width || y < 0 || y >= ctx->height) return;

    /* Scissor test */
    if (ctx->scissor_enabled) {
        if (x < ctx->scissor_x || x >= (ctx->scissor_x + ctx->scissor_w) ||
            y < ctx->scissor_y || y >= (ctx->scissor_y + ctx->scissor_h)) {
            return;
        }
    }

    /* Alpha test */
    if (ctx->alpha_test_enabled) {
        bool pass = false;
        switch (ctx->alpha_func) {
            case GL_NEVER:    pass = false; break;
            case GL_LESS:     pass = (a < ctx->alpha_ref); break;
            case GL_EQUAL:    pass = (a == ctx->alpha_ref); break;
            case GL_LEQUAL:   pass = (a <= ctx->alpha_ref); break;
            case GL_GREATER:  pass = (a > ctx->alpha_ref); break;
            case GL_NOTEQUAL: pass = (a != ctx->alpha_ref); break;
            case GL_GEQUAL:   pass = (a >= ctx->alpha_ref); break;
            case GL_ALWAYS:   pass = true; break;
        }
        if (!pass) return;
    }

    size_t pixel_idx = (size_t)y * ctx->width + x;

    /* Stencil test */
    bool stencil_pass = true;
    if (ctx->stencil_test_enabled) {
        stencil_pass = test_stencil(ctx->stencil_func, ctx->stencil_ref, ctx->stencil_buffer[pixel_idx], ctx->stencil_mask);
        if (!stencil_pass) {
            ctx->stencil_buffer[pixel_idx] = apply_stencil_op(ctx->stencil_fail, ctx->stencil_buffer[pixel_idx], ctx->stencil_ref, ctx->stencil_writemask);
            return;
        }
    }

    /* Depth test */
    bool depth_pass = true;
    if (ctx->depth_test_enabled) {
        depth_pass = test_depth(ctx->depth_func, z_val, ctx->z_buffer[pixel_idx]);
        if (!depth_pass) {
            if (ctx->stencil_test_enabled) {
                ctx->stencil_buffer[pixel_idx] = apply_stencil_op(ctx->stencil_zfail, ctx->stencil_buffer[pixel_idx], ctx->stencil_ref, ctx->stencil_writemask);
            }
            return;
        }
    }

    if (ctx->stencil_test_enabled) {
        ctx->stencil_buffer[pixel_idx] = apply_stencil_op(ctx->stencil_zpass, ctx->stencil_buffer[pixel_idx], ctx->stencil_ref, ctx->stencil_writemask);
    }

    if (ctx->depth_test_enabled && ctx->depth_mask) {
        ctx->z_buffer[pixel_idx] = z_val;
    }

    /* Fog calculation */
    if (ctx->fog_enabled) {
        float f = 1.0f;
        float d = -eye_z;
        if (ctx->fog_mode == GL_LINEAR) {
            f = (ctx->fog_end - d) / (ctx->fog_end - ctx->fog_start);
        } else if (ctx->fog_mode == GL_EXP) {
            f = gl_exp_neg(-ctx->fog_density * d);
        } else if (ctx->fog_mode == GL_EXP2) {
            f = gl_exp_neg(-(ctx->fog_density * d) * (ctx->fog_density * d));
        }
        if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;
        r = f * r + (1.0f - f) * ctx->fog_color[0];
        g = f * g + (1.0f - f) * ctx->fog_color[1];
        b = f * b + (1.0f - f) * ctx->fog_color[2];
    }

    /* Blending */
    uint32_t dst_col = ctx->color_buffer[pixel_idx];
    if (ctx->blend_enabled) {
        float rd = ((dst_col >> 16) & 0xFF) / 255.0f;
        float gd = ((dst_col >> 8)  & 0xFF) / 255.0f;
        float bd = (dst_col         & 0xFF) / 255.0f;
        float ad = ((dst_col >> 24) & 0xFF) / 255.0f;

        float fsr, fsg, fsb, fsa;
        float fdr, fdg, fdb, fda;
        get_blend_factors(ctx->blend_sfactor, r, g, b, a, rd, gd, bd, ad, &fsr, &fsg, &fsb, &fsa);
        get_blend_factors(ctx->blend_dfactor, r, g, b, a, rd, gd, bd, ad, &fdr, &fdg, &fdb, &fda);

        if (ctx->blend_equation == GL_FUNC_SUBTRACT) {
            r = r * fsr - rd * fdr; g = g * fsg - gd * fdr; b = b * fsb - bd * fdb; a = a * fsa - ad * fda;
        } else if (ctx->blend_equation == GL_FUNC_REVERSE_SUBTRACT) {
            r = rd * fdr - r * fsr; g = gd * fdr - g * fsg; b = bd * fdb - b * fsb; a = ad * fda - a * fsa;
        } else if (ctx->blend_equation == GL_MIN) {
            if (rd < r) r = rd; if (gd < g) g = gd; if (bd < b) b = bd; if (ad < a) a = ad;
        } else if (ctx->blend_equation == GL_MAX) {
            if (rd > r) r = rd; if (gd > g) g = gd; if (bd > b) b = bd; if (ad > a) a = ad;
        } else {
            /* GL_FUNC_ADD */
            r = r * fsr + rd * fdr; g = g * fsg + gd * fdr; b = b * fsb + bd * fdb; a = a * fsa + ad * fda;
        }
    }

    if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
    if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;

    uint32_t final_pixel = ((uint32_t)(a * 255.0f) << 24) |
                           ((uint32_t)(r * 255.0f) << 16) |
                           ((uint32_t)(g * 255.0f) << 8)  |
                           (uint32_t)(b * 255.0f);

    /* LogicOp */
    if (ctx->logic_op_enabled) {
        switch (ctx->logic_opcode) {
            case GL_CLEAR:         final_pixel = 0; break;
            case GL_SET:           final_pixel = 0xFFFFFFFFu; break;
            case GL_COPY:          break;
            case GL_COPY_INVERTED: final_pixel = ~final_pixel; break;
            case GL_NOOP:          final_pixel = dst_col; break;
            case GL_INVERT:        final_pixel = ~dst_col; break;
            case GL_AND:           final_pixel &= dst_col; break;
            case GL_NAND:          final_pixel = ~(final_pixel & dst_col); break;
            case GL_OR:            final_pixel |= dst_col; break;
            case GL_NOR:           final_pixel = ~(final_pixel | dst_col); break;
            case GL_XOR:           final_pixel ^= dst_col; break;
            case GL_EQUIV:         final_pixel = ~(final_pixel ^ dst_col); break;
            default: break;
        }
    }

    ctx->color_buffer[pixel_idx] = final_pixel;
}

/* Triangle Rasterization with Barycentric Coordinates and Perspective Correction */
void gl_rasterize_triangle(GLContext *ctx, gl_vertex_t *v0, gl_vertex_t *v1, gl_vertex_t *v2) {
    if (!ctx) return;

    if (ctx->polygon_mode == GL_LINE) {
        gl_rasterize_line(ctx, v0, v1);
        gl_rasterize_line(ctx, v1, v2);
        gl_rasterize_line(ctx, v2, v0);
        return;
    } else if (ctx->polygon_mode == GL_POINT) {
        gl_rasterize_point(ctx, v0);
        gl_rasterize_point(ctx, v1);
        gl_rasterize_point(ctx, v2);
        return;
    }

    float x0 = v0->win[0], y0 = v0->win[1];
    float x1 = v1->win[0], y1 = v1->win[1];
    float x2 = v2->win[0], y2 = v2->win[1];

    /* Bounding box */
    int min_x = gl_floor_to_int(gl_minf(x0, gl_minf(x1, x2)));
    int max_x = gl_ceil_to_int(gl_maxf(x0, gl_maxf(x1, x2)));
    int min_y = gl_floor_to_int(gl_minf(y0, gl_minf(y1, y2)));
    int max_y = gl_ceil_to_int(gl_maxf(y0, gl_maxf(y1, y2)));

    if (min_x < 0) min_x = 0; if (max_x >= ctx->width)  max_x = ctx->width - 1;
    if (min_y < 0) min_y = 0; if (max_y >= ctx->height) max_y = ctx->height - 1;

    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area == 0.0f) return;
    float inv_area = 1.0f / area;

    bool fast_opaque = (!ctx->texture_2d_enabled || ctx->current_texture_2d == 0) &&
                       !ctx->blend_enabled &&
                       !ctx->fog_enabled &&
                       !ctx->scissor_enabled &&
                       !ctx->alpha_test_enabled &&
                       !ctx->stencil_test_enabled &&
                       !ctx->logic_op_enabled &&
                       ctx->depth_test_enabled &&
                       ctx->depth_mask &&
                       (ctx->depth_func == GL_LESS || ctx->depth_func == GL_LEQUAL);

    if (fast_opaque) {
        float dw0_dx = (y1 - y2) * inv_area;
        float dw1_dx = (y2 - y0) * inv_area;
        float dw0_dy = (x2 - x1) * inv_area;
        float dw1_dy = (x0 - x2) * inv_area;

        float z0 = v0->win[2] * 65535.0f, z1 = v1->win[2] * 65535.0f, z2 = v2->win[2] * 65535.0f;
        float dz_dx = dw0_dx * (z0 - z2) + dw1_dx * (z1 - z2);
        float dz_dy = dw0_dy * (z0 - z2) + dw1_dy * (z1 - z2);

        float r0 = v0->color[0] * 255.0f, r1 = v1->color[0] * 255.0f, r2 = v2->color[0] * 255.0f;
        float dr_dx = dw0_dx * (r0 - r2) + dw1_dx * (r1 - r2);
        float dr_dy = dw0_dy * (r0 - r2) + dw1_dy * (r1 - r2);

        float g0 = v0->color[1] * 255.0f, g1 = v1->color[1] * 255.0f, g2 = v2->color[1] * 255.0f;
        float dg_dx = dw0_dx * (g0 - g2) + dw1_dx * (g1 - g2);
        float dg_dy = dw0_dy * (g0 - g2) + dw1_dy * (g1 - g2);

        float b0 = v0->color[2] * 255.0f, b1 = v1->color[2] * 255.0f, b2 = v2->color[2] * 255.0f;
        float db_dx = dw0_dx * (b0 - b2) + dw1_dx * (b1 - b2);
        float db_dy = dw0_dy * (b0 - b2) + dw1_dy * (b1 - b2);

        float fx_start = (float)min_x + 0.5f;
        float fy_start = (float)min_y + 0.5f;
        float w0_base = ((x1 - fx_start) * (y2 - fy_start) - (y1 - fy_start) * (x2 - fx_start)) * inv_area;
        float w1_base = ((x2 - fx_start) * (y0 - fy_start) - (y2 - fy_start) * (x0 - fx_start)) * inv_area;
        float z_base  = z2 + w0_base * (z0 - z2) + w1_base * (z1 - z2);
        float r_base  = r2 + w0_base * (r0 - r2) + w1_base * (r1 - r2);
        float g_base  = g2 + w0_base * (g0 - g2) + w1_base * (g1 - g2);
        float b_base  = b2 + w0_base * (b0 - b2) + w1_base * (b1 - b2);

        for (int py = min_y; py <= max_y; py++) {
            int y_off = py - min_y;
            float w0 = w0_base + (float)y_off * dw0_dy;
            float w1 = w1_base + (float)y_off * dw1_dy;
            float z_cur = z_base + (float)y_off * dz_dy;
            float r_cur = r_base + (float)y_off * dr_dy;
            float g_cur = g_base + (float)y_off * dg_dy;
            float b_cur = b_base + (float)y_off * db_dy;

            size_t pixel_idx = (size_t)py * ctx->width + min_x;
            bool had_inside = false;

            for (int px = min_x; px <= max_x; px++, pixel_idx++) {
                float w2 = 1.0f - w0 - w1;
                if (w0 >= -0.001f && w1 >= -0.001f && w2 >= -0.001f) {
                    had_inside = true;
                    uint16_t z_val = (z_cur > 0.0f) ? ((z_cur < 65535.0f) ? (uint16_t)z_cur : 65535) : 0;
                    if (z_val < ctx->z_buffer[pixel_idx]) {
                        ctx->z_buffer[pixel_idx] = z_val;
                        uint32_t cr = (r_cur > 0.0f) ? ((r_cur < 255.0f) ? (uint32_t)r_cur : 255) : 0;
                        uint32_t cg = (g_cur > 0.0f) ? ((g_cur < 255.0f) ? (uint32_t)g_cur : 255) : 0;
                        uint32_t cb = (b_cur > 0.0f) ? ((b_cur < 255.0f) ? (uint32_t)b_cur : 255) : 0;
                        ctx->color_buffer[pixel_idx] = 0xFF000000u | (cr << 16) | (cg << 8) | cb;
                    }
                } else if (had_inside) {
                    break;
                }
                w0 += dw0_dx;
                w1 += dw1_dx;
                z_cur += dz_dx;
                r_cur += dr_dx;
                g_cur += dg_dx;
                b_cur += db_dx;
            }
        }
        return;
    }

    gl_texture_t *tex = NULL;
    if (ctx->texture_2d_enabled && ctx->current_texture_2d != 0) {
        for (int i = 0; i < ctx->texture_count; i++) {
            if (ctx->textures[i] && ctx->textures[i]->id == ctx->current_texture_2d) {
                tex = ctx->textures[i];
                break;
            }
        }
    }

    for (int py = min_y; py <= max_y; py++) {
        float fy = (float)py + 0.5f;
        for (int px = min_x; px <= max_x; px++) {
            float fx = (float)px + 0.5f;

            /* Barycentric weights */
            float w0 = ((x1 - fx) * (y2 - fy) - (y1 - fy) * (x2 - fx)) * inv_area;
            float w1 = ((x2 - fx) * (y0 - fy) - (y2 - fy) * (x0 - fx)) * inv_area;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;

            /* Interpolate 1/w and depth */
            float inv_w = w0 * v0->inv_w + w1 * v1->inv_w + w2 * v2->inv_w;
            if (inv_w == 0.0f) continue;
            float w_frag = 1.0f / inv_w;

            float z_norm = (w0 * v0->win[2] + w1 * v1->win[2] + w2 * v2->win[2]);
            if (z_norm < 0.0f) z_norm = 0.0f; else if (z_norm > 1.0f) z_norm = 1.0f;
            uint16_t z_val = (uint16_t)(z_norm * 65535.0f);

            /* Interpolate color (Gouraud shading) */
            float r = (w0 * v0->color[0] + w1 * v1->color[0] + w2 * v2->color[0]);
            float g = (w0 * v0->color[1] + w1 * v1->color[1] + w2 * v2->color[1]);
            float b = (w0 * v0->color[2] + w1 * v1->color[2] + w2 * v2->color[2]);
            float a = (w0 * v0->color[3] + w1 * v1->color[3] + w2 * v2->color[3]);

            /* Interpolate texture coordinates with perspective correction */
            if (tex) {
                float s = (w0 * (v0->tex[0] * v0->inv_w) + w1 * (v1->tex[0] * v1->inv_w) + w2 * (v2->tex[0] * v2->inv_w)) * w_frag;
                float t = (w0 * (v0->tex[1] * v0->inv_w) + w1 * (v1->tex[1] * v1->inv_w) + w2 * (v2->tex[1] * v2->inv_w)) * w_frag;
                uint32_t tcol = sample_tex2d(tex, s, t);

                float tr = ((tcol >> 16) & 0xFF) / 255.0f;
                float tg = ((tcol >> 8)  & 0xFF) / 255.0f;
                float tb = (tcol         & 0xFF) / 255.0f;
                float ta = ((tcol >> 24) & 0xFF) / 255.0f;

                if (ctx->tex_env_mode == GL_MODULATE) {
                    r *= tr; g *= tg; b *= tb; a *= ta;
                } else if (ctx->tex_env_mode == GL_DECAL) {
                    r = tr; g = tg; b = tb;
                } else if (ctx->tex_env_mode == GL_REPLACE) {
                    r = tr; g = tg; b = tb; a = ta;
                }
            }

            float eye_z = (w0 * v0->eye[2] + w1 * v1->eye[2] + w2 * v2->eye[2]);
            write_fragment(ctx, px, py, z_val, r, g, b, a, eye_z);
        }
    }
}

void gl_rasterize_point(GLContext *ctx, gl_vertex_t *v0) {
    if (!ctx) return;
    int cx = (int)v0->win[0];
    int cy = (int)v0->win[1];
    int radius = (int)(ctx->point_size * 0.5f);
    if (radius < 1) radius = 1;

    uint16_t z_val = (uint16_t)(v0->win[2] * 65535.0f);
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            write_fragment(ctx, cx + dx, cy + dy, z_val, v0->color[0], v0->color[1], v0->color[2], v0->color[3], v0->eye[2]);
        }
    }
}

void gl_rasterize_line(GLContext *ctx, gl_vertex_t *v0, gl_vertex_t *v1) {
    if (!ctx) return;
    int x0 = (int)v0->win[0], y0 = (int)v0->win[1];
    int x1 = (int)v1->win[0], y1 = (int)v1->win[1];

    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int total_steps = dx > dy ? dx : dy;
    if (total_steps <= 0) total_steps = 1;
    int step = 0;

    while (1) {
        float t = (float)step / (float)total_steps;
        float r = (1.0f - t) * v0->color[0] + t * v1->color[0];
        float g = (1.0f - t) * v0->color[1] + t * v1->color[1];
        float b = (1.0f - t) * v0->color[2] + t * v1->color[2];
        float a = (1.0f - t) * v0->color[3] + t * v1->color[3];
        float z_norm = (1.0f - t) * v0->win[2] + t * v1->win[2];
        uint16_t z_val = (uint16_t)(z_norm * 65535.0f);
        float eye_z = (1.0f - t) * v0->eye[2] + t * v1->eye[2];

        write_fragment(ctx, x0, y0, z_val, r, g, b, a, eye_z);

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
        step++;
    }
}
