#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <driver/vbe_bga.h>
#include <wm.h>
#include <vga_gfx.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>
#include <syscall.h>

static GLContext *g_current_context = NULL;

GLContext *gl_create_context(int width, int height, void *win) {
    if (width <= 0)  width  = 100;
    if (height <= 0) height = 60;

    GLContext *ctx = kmalloc(sizeof(GLContext));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(GLContext));

    ctx->width  = width;
    ctx->height = height;
    ctx->win    = win;

    /* Allocate 32-bit ARGB color buffer, 16-bit depth buffer, and 8-bit stencil buffer */
    size_t num_pixels = (size_t)width * (size_t)height;
    ctx->color_buffer   = kmalloc(num_pixels * sizeof(uint32_t));
    ctx->z_buffer       = kmalloc(num_pixels * sizeof(uint16_t));
    ctx->stencil_buffer = kmalloc(num_pixels * sizeof(uint8_t));

    if (!ctx->color_buffer || !ctx->z_buffer || !ctx->stencil_buffer) {
        gl_destroy_context(ctx);
        return NULL;
    }

    /* Initialize matrix stacks without hardcoded limits */
    gl_matrix_init_stack(&ctx->modelview_stack);
    gl_matrix_init_stack(&ctx->projection_stack);
    gl_matrix_init_stack(&ctx->texture_stack);
    ctx->matrix_mode = GL_MODELVIEW;

    /* Viewport */
    ctx->vp_x = 0;
    ctx->vp_y = 0;
    ctx->vp_w = width;
    ctx->vp_h = height;
    ctx->depth_near = 0.0;
    ctx->depth_far  = 1.0;

    /* Scissor */
    ctx->scissor_x = 0;
    ctx->scissor_y = 0;
    ctx->scissor_w = width;
    ctx->scissor_h = height;
    ctx->scissor_enabled = GL_FALSE;

    /* Clear values */
    ctx->clear_color[0] = 0.0f;
    ctx->clear_color[1] = 0.0f;
    ctx->clear_color[2] = 0.0f;
    ctx->clear_color[3] = 1.0f;
    ctx->clear_depth    = 1.0;
    ctx->clear_stencil  = 0;

    /* Depth Test */
    ctx->depth_test_enabled = GL_FALSE;
    ctx->depth_func         = GL_LESS;
    ctx->depth_mask         = GL_TRUE;

    /* Alpha Test */
    ctx->alpha_test_enabled = GL_FALSE;
    ctx->alpha_func         = GL_ALWAYS;
    ctx->alpha_ref          = 0.0f;

    /* Stencil Test */
    ctx->stencil_test_enabled = GL_FALSE;
    ctx->stencil_func         = GL_ALWAYS;
    ctx->stencil_ref          = 0;
    ctx->stencil_mask         = 0xFFFFFFFFu;
    ctx->stencil_fail         = GL_KEEP;
    ctx->stencil_zfail        = GL_KEEP;
    ctx->stencil_zpass        = GL_KEEP;
    ctx->stencil_writemask    = 0xFFFFFFFFu;

    /* Blending & LogicOp */
    ctx->blend_enabled   = GL_FALSE;
    ctx->blend_sfactor   = GL_ONE;
    ctx->blend_dfactor   = GL_ZERO;
    ctx->blend_equation  = GL_FUNC_ADD;
    ctx->logic_op_enabled = GL_FALSE;
    ctx->logic_opcode    = GL_COPY;

    /* Fog */
    ctx->fog_enabled = GL_FALSE;
    ctx->fog_mode    = GL_EXP;
    ctx->fog_density = 1.0f;
    ctx->fog_start   = 0.0f;
    ctx->fog_end     = 1.0f;
    ctx->fog_color[0] = 0.0f; ctx->fog_color[1] = 0.0f;
    ctx->fog_color[2] = 0.0f; ctx->fog_color[3] = 0.0f;

    /* Lighting & Materials */
    ctx->lighting_enabled = GL_FALSE;
    ctx->shade_model      = GL_SMOOTH;
    ctx->front_material.ambient[0] = 0.2f; ctx->front_material.ambient[1] = 0.2f;
    ctx->front_material.ambient[2] = 0.2f; ctx->front_material.ambient[3] = 1.0f;
    ctx->front_material.diffuse[0] = 0.8f; ctx->front_material.diffuse[1] = 0.8f;
    ctx->front_material.diffuse[2] = 0.8f; ctx->front_material.diffuse[3] = 1.0f;
    ctx->front_material.specular[0] = 0.0f; ctx->front_material.specular[1] = 0.0f;
    ctx->front_material.specular[2] = 0.0f; ctx->front_material.specular[3] = 1.0f;
    ctx->front_material.shininess = 0.0f;

    /* Light 0 default position */
    ctx->lights[0].diffuse[0] = 1.0f; ctx->lights[0].diffuse[1] = 1.0f;
    ctx->lights[0].diffuse[2] = 1.0f; ctx->lights[0].diffuse[3] = 1.0f;
    ctx->lights[0].position[2] = 1.0f;

    /* Polygon mode */
    ctx->polygon_mode      = GL_FILL;
    ctx->cull_face_enabled = GL_FALSE;
    ctx->cull_face_mode    = GL_BACK;
    ctx->front_face        = GL_CCW;
    ctx->point_size        = 1.0f;
    ctx->line_width        = 1.0f;

    /* Current attributes */
    ctx->cur_color[0] = 1.0f; ctx->cur_color[1] = 1.0f;
    ctx->cur_color[2] = 1.0f; ctx->cur_color[3] = 1.0f;
    ctx->cur_normal[2] = 1.0f;
    ctx->cur_texcoord[0] = 0.0f; ctx->cur_texcoord[1] = 0.0f;
    ctx->cur_texcoord[2] = 0.0f; ctx->cur_texcoord[3] = 1.0f;

    /* Textures */
    ctx->tex_env_mode = GL_MODULATE;

    /* Display lists */
    ctx->list_capacity = 16;
    ctx->display_lists = kmalloc((size_t)ctx->list_capacity * sizeof(gl_display_list_t *));

    /* Vertex buffer for immediate mode */
    ctx->vert_buf.capacity = 64;
    ctx->vert_buf.count = 0;
    ctx->vert_buf.data = kmalloc((size_t)ctx->vert_buf.capacity * sizeof(gl_vertex_t));

    ctx->error_flag = GL_NO_ERROR;

    if (!g_current_context) {
        g_current_context = ctx;
    }

    return ctx;
}

void gl_destroy_context(GLContext *ctx) {
    if (!ctx) return;

    if (g_current_context == ctx) {
        g_current_context = NULL;
    }

    if (ctx->color_buffer)   kfree(ctx->color_buffer);
    if (ctx->z_buffer)       kfree(ctx->z_buffer);
    if (ctx->stencil_buffer) kfree(ctx->stencil_buffer);

    gl_matrix_free_stack(&ctx->modelview_stack);
    gl_matrix_free_stack(&ctx->projection_stack);
    gl_matrix_free_stack(&ctx->texture_stack);

    if (ctx->vert_buf.data) kfree(ctx->vert_buf.data);

    if (ctx->textures) {
        for (int i = 0; i < ctx->texture_count; i++) {
            if (ctx->textures[i]) {
                if (ctx->textures[i]->pixels) kfree(ctx->textures[i]->pixels);
                kfree(ctx->textures[i]);
            }
        }
        kfree(ctx->textures);
    }

    if (ctx->display_lists) {
        for (int i = 0; i < ctx->list_count; i++) {
            if (ctx->display_lists[i]) {
                if (ctx->display_lists[i]->commands) kfree(ctx->display_lists[i]->commands);
                kfree(ctx->display_lists[i]);
            }
        }
        kfree(ctx->display_lists);
    }

    kfree(ctx);
}

bool gl_resize_context(GLContext *ctx, int new_w, int new_h) {
    if (!ctx || new_w <= 0 || new_h <= 0) return false;
    if (ctx->width == new_w && ctx->height == new_h) return true;

    size_t npixels = (size_t)new_w * (size_t)new_h;
    uint32_t *new_color = kmalloc(npixels * sizeof(uint32_t));
    uint16_t *new_z = kmalloc(npixels * sizeof(uint16_t));
    uint8_t *new_stencil = kmalloc(npixels * sizeof(uint8_t));

    if (!new_color || !new_z || !new_stencil) {
        if (new_color) kfree(new_color);
        if (new_z) kfree(new_z);
        if (new_stencil) kfree(new_stencil);
        return false;
    }

    if (ctx->color_buffer) kfree(ctx->color_buffer);
    if (ctx->z_buffer) kfree(ctx->z_buffer);
    if (ctx->stencil_buffer) kfree(ctx->stencil_buffer);

    ctx->color_buffer = new_color;
    ctx->z_buffer = new_z;
    ctx->stencil_buffer = new_stencil;
    ctx->width = new_w;
    ctx->height = new_h;
    ctx->vp_w = new_w;
    ctx->vp_h = new_h;

    return true;
}

void gl_make_current(GLContext *ctx) {
    g_current_context = ctx;
}

GLContext *gl_get_current_context(void) {
    return g_current_context;
}

void gl_swap_buffers(GLContext *ctx, void *win_ptr) {
    if (!ctx) ctx = g_current_context;
    if (!ctx || !ctx->color_buffer) return;

    wm_window_t *win = (wm_window_t *)(win_ptr ? win_ptr : ctx->win);

    if (win != NULL && win->framebuf != NULL && wm_is_window_valid(win)) {
        /*
         * Multi-window startX mode (VGA Mode 13h or TrueColor window):
         * Strided quantization so window resizing or dimensions never shear or misalign scanlines.
         */
        int w = ctx->width  < (int)win->w ? ctx->width  : (int)win->w;
        int h = ctx->height < (int)win->h ? ctx->height : (int)win->h;
        gl_quantize_argb_to_palette_strided(ctx->color_buffer, ctx->width, win->framebuf, win->w, w, h);
        wm_invalidate(win);
    } else if (vbe_bga_is_available() && vbe_bga_get_bpp() == 32) {
        /*
         * Bare Metal / QEMU TrueColor LFB mode: direct 32-bit copy
         */
        uint32_t *lfb = (uint32_t *)vbe_bga_get_framebuffer();
        if (lfb) {
            size_t sz = (size_t)ctx->width * (size_t)ctx->height * sizeof(uint32_t);
            memcpy(lfb, ctx->color_buffer, sz);
        }
    } else {
        /*
         * Standalone Mode 13h (like figure.c): direct to VRAM at 0xA0000
         */
        uint8_t *vram = (uint8_t *)0xA0000;
        gl_quantize_argb_to_palette_strided(ctx->color_buffer, ctx->width, vram, 320, ctx->width, ctx->height);
    }
}

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->clear_color[0] = red;
    ctx->clear_color[1] = green;
    ctx->clear_color[2] = blue;
    ctx->clear_color[3] = alpha;
}

void glClearDepth(GLclampd depth) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->clear_depth = depth;
}

void glClearStencil(GLint s) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->clear_stencil = s;
}

void glClear(GLbitfield mask) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || ctx->width <= 0 || ctx->height <= 0) return;

    size_t total = (size_t)ctx->width * (size_t)ctx->height;

    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t r = (uint32_t)(ctx->clear_color[0] * 255.0f);
        uint32_t g = (uint32_t)(ctx->clear_color[1] * 255.0f);
        uint32_t b = (uint32_t)(ctx->clear_color[2] * 255.0f);
        uint32_t a = (uint32_t)(ctx->clear_color[3] * 255.0f);
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255; if (a > 255) a = 255;
        uint32_t pixel = (a << 24) | (r << 16) | (g << 8) | b;

        for (size_t i = 0; i < total; i++) {
            ctx->color_buffer[i] = pixel;
        }
    }

    if (mask & GL_DEPTH_BUFFER_BIT) {
        uint16_t zval = (uint16_t)(ctx->clear_depth * 65535.0);
        for (size_t i = 0; i < total; i++) {
            ctx->z_buffer[i] = zval;
        }
    }

    if (mask & GL_STENCIL_BUFFER_BIT) {
        uint8_t sval = (uint8_t)(ctx->clear_stencil & 0xFF);
        memset(ctx->stencil_buffer, sval, total);
    }
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->vp_x = x;
    ctx->vp_y = y;
    ctx->vp_w = width;
    ctx->vp_h = height;
}

void glDepthRange(GLclampd nearVal, GLclampd farVal) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->depth_near = nearVal;
    ctx->depth_far  = farVal;
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->scissor_x = x;
    ctx->scissor_y = y;
    ctx->scissor_w = width;
    ctx->scissor_h = height;
}

void glEnable(GLenum cap) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    switch (cap) {
        case GL_DEPTH_TEST:     ctx->depth_test_enabled   = GL_TRUE; break;
        case GL_ALPHA_TEST:     ctx->alpha_test_enabled   = GL_TRUE; break;
        case GL_STENCIL_TEST:   ctx->stencil_test_enabled = GL_TRUE; break;
        case GL_BLEND:          ctx->blend_enabled        = GL_TRUE; break;
        case GL_COLOR_LOGIC_OP: ctx->logic_op_enabled     = GL_TRUE; break;
        case GL_SCISSOR_TEST:   ctx->scissor_enabled      = GL_TRUE; break;
        case GL_CULL_FACE:      ctx->cull_face_enabled    = GL_TRUE; break;
        case GL_LIGHTING:       ctx->lighting_enabled     = GL_TRUE; break;
        case GL_FOG:            ctx->fog_enabled          = GL_TRUE; break;
        case GL_TEXTURE_2D:     ctx->texture_2d_enabled   = GL_TRUE; break;
        case GL_TEXTURE_3D:     ctx->texture_3d_enabled   = GL_TRUE; break;
        case GL_COLOR_MATERIAL: ctx->color_material_enabled = GL_TRUE; break;
        case GL_NORMALIZE:      ctx->normalize_normals    = GL_TRUE; break;
        default:
            if (cap >= GL_LIGHT0 && cap <= GL_LIGHT7) {
                ctx->lights[cap - GL_LIGHT0].enabled = GL_TRUE;
            } else {
                ctx->error_flag = GL_INVALID_ENUM;
            }
            break;
    }
}

void glDisable(GLenum cap) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    switch (cap) {
        case GL_DEPTH_TEST:     ctx->depth_test_enabled   = GL_FALSE; break;
        case GL_ALPHA_TEST:     ctx->alpha_test_enabled   = GL_FALSE; break;
        case GL_STENCIL_TEST:   ctx->stencil_test_enabled = GL_FALSE; break;
        case GL_BLEND:          ctx->blend_enabled        = GL_FALSE; break;
        case GL_COLOR_LOGIC_OP: ctx->logic_op_enabled     = GL_FALSE; break;
        case GL_SCISSOR_TEST:   ctx->scissor_enabled      = GL_FALSE; break;
        case GL_CULL_FACE:      ctx->cull_face_enabled    = GL_FALSE; break;
        case GL_LIGHTING:       ctx->lighting_enabled     = GL_FALSE; break;
        case GL_FOG:            ctx->fog_enabled          = GL_FALSE; break;
        case GL_TEXTURE_2D:     ctx->texture_2d_enabled   = GL_FALSE; break;
        case GL_TEXTURE_3D:     ctx->texture_3d_enabled   = GL_FALSE; break;
        case GL_COLOR_MATERIAL: ctx->color_material_enabled = GL_FALSE; break;
        case GL_NORMALIZE:      ctx->normalize_normals    = GL_FALSE; break;
        default:
            if (cap >= GL_LIGHT0 && cap <= GL_LIGHT7) {
                ctx->lights[cap - GL_LIGHT0].enabled = GL_FALSE;
            } else {
                ctx->error_flag = GL_INVALID_ENUM;
            }
            break;
    }
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
        default:
            if (cap >= GL_LIGHT0 && cap <= GL_LIGHT7) {
                return ctx->lights[cap - GL_LIGHT0].enabled;
            }
            return GL_FALSE;
    }
}

