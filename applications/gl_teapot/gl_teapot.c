/*
 * gl_teapot.c - Realistic 3D Utah Teapot with GL
 *
 * Features:
 * - High-detail smooth Utah Teapot 3D geometry
 * - Analytical per-vertex surface normals for smooth Gouraud shading
 * - Blinn-Phong lighting model with dynamic specular highlights
 * - Multiple realistic material presets (Porcelain, Gold, Emerald Jade, Chrome, Cobalt, Obsidian)
 * - Full interactive manual rotation (mouse drag + arrow keys)
 * - Automatic spin toggle (Spacebar), material cycle ('M'), reset ('R'), zoom (+/-)
 * - Dynamic window resizing with automatic context & projection recalculation
 * - Hardware backface culling (GL_CULL_FACE) for optimal performance
 * - Multi-instance PID-isolated execution
 * - Smooth 30 FPS timer pacing with cooperative kernel yielding
 */

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/ipo_gl.h>
#include <GL/gl_math.h>
#include <wm.h>
#include <syscall.h>
#include <system/timer.h>
#include <system/state.h>
#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <stdio.h>
#include <string.h>
#include "freeglut_teapot_data.h"

#define DEFAULT_WIN_W 200
#define DEFAULT_WIN_H 150

/* Keyboard scancodes */
#define SC_ESC       0x01
#define SC_Q         0x10
#define SC_R         0x13
#define SC_M         0x32
#define SC_SPACE     0x39
#define SC_UP        0x48
#define SC_PAGE_UP   0x49
#define SC_KEYPAD_MINUS 0x4A
#define SC_LEFT      0x4B
#define SC_RIGHT     0x4D
#define SC_KEYPAD_PLUS  0x4E
#define SC_DOWN      0x50
#define SC_PAGE_DOWN 0x51
#define SC_S         0x1F
#define SC_L         0x26

typedef struct {
    const char *name;
    GLfloat ambient[4];
    GLfloat diffuse[4];
    GLfloat specular[4];
    GLfloat shininess;
} material_preset_t;

static const material_preset_t materials[] = {
    {
        .name = "Glazed Crimson Porcelain",
        .ambient = { 0.28f, 0.06f, 0.06f, 1.0f },
        .diffuse = { 0.88f, 0.18f, 0.14f, 1.0f },
        .specular = { 1.0f, 0.95f, 0.9f, 1.0f },
        .shininess = 75.0f
    },
    {
        .name = "Polished 24K Gold",
        .ambient = { 0.25f, 0.22f, 0.08f, 1.0f },
        .diffuse = { 0.80f, 0.65f, 0.24f, 1.0f },
        .specular = { 0.98f, 0.90f, 0.50f, 1.0f },
        .shininess = 68.0f
    },
    {
        .name = "Imperial Emerald Jade",
        .ambient = { 0.06f, 0.24f, 0.09f, 1.0f },
        .diffuse = { 0.14f, 0.72f, 0.30f, 1.0f },
        .specular = { 0.75f, 0.98f, 0.85f, 1.0f },
        .shininess = 55.0f
    },
    {
        .name = "Polished Chrome",
        .ambient = { 0.24f, 0.24f, 0.28f, 1.0f },
        .diffuse = { 0.60f, 0.60f, 0.68f, 1.0f },
        .specular = { 0.98f, 0.98f, 1.0f, 1.0f },
        .shininess = 96.0f
    },
    {
        .name = "Royal Cobalt Ceramic",
        .ambient = { 0.08f, 0.10f, 0.28f, 1.0f },
        .diffuse = { 0.15f, 0.25f, 0.85f, 1.0f },
        .specular = { 0.90f, 0.92f, 1.0f, 1.0f },
        .shininess = 80.0f
    },
    {
        .name = "Polished Obsidian Glass",
        .ambient = { 0.08f, 0.08f, 0.10f, 1.0f },
        .diffuse = { 0.18f, 0.18f, 0.22f, 1.0f },
        .specular = { 0.95f, 0.95f, 0.95f, 1.0f },
        .shininess = 90.0f
    }
};

#define NUM_MATERIALS (sizeof(materials) / sizeof(materials[0]))

static volatile bool app_running = true;
static float rot_x = 22.0f;
static float rot_y = 0.0f;
static float cam_dist = 7.5f;
static bool auto_spin = true;
static int current_mat = 0;
static bool shadows_enabled = true;
static bool light1_enabled = true;

static void on_window_event(wm_window_t *w, uint32_t event, uint32_t data) {
    (void)w;
    if (event == WM_EVENT_CLOSE) {
        app_running = false;
    } else if (event == WM_EVENT_KEY_DOWN) {
        uint8_t sc = (uint8_t)(data & 0xFF);
        if (sc == SC_ESC || sc == SC_Q) {
            app_running = false;
        } else if (sc == SC_LEFT) {
            rot_y -= 6.0f;
            auto_spin = false;
        } else if (sc == SC_RIGHT) {
            rot_y += 6.0f;
            auto_spin = false;
        } else if (sc == SC_UP) {
            rot_x -= 6.0f;
            auto_spin = false;
        } else if (sc == SC_DOWN) {
            rot_x += 6.0f;
            auto_spin = false;
        } else if (sc == SC_SPACE) {
            auto_spin = !auto_spin;
        } else if (sc == SC_M) {
            current_mat = (current_mat + 1) % (int)NUM_MATERIALS;
        } else if (sc == SC_S) {
            shadows_enabled = !shadows_enabled;
        } else if (sc == SC_L) {
            light1_enabled = !light1_enabled;
        } else if (sc == SC_R) {
            rot_x = 22.0f;
            rot_y = 0.0f;
            cam_dist = 7.5f;
            auto_spin = true;
            shadows_enabled = true;
            light1_enabled = true;
        } else if (sc == SC_PAGE_UP || sc == SC_KEYPAD_MINUS) {
            cam_dist -= 0.6f;
            if (cam_dist < 3.5f) cam_dist = 3.5f;
        } else if (sc == SC_PAGE_DOWN || sc == SC_KEYPAD_PLUS) {
            cam_dist += 0.6f;
            if (cam_dist > 22.0f) cam_dist = 22.0f;
        }
    }
}

#define cosf gl_cosf
#define sinf gl_sinf

static void build_shadow_matrix(float mat[16], const float light[4], const float plane[4]) {
    float dot = plane[0] * light[0] + plane[1] * light[1] + plane[2] * light[2] + plane[3] * light[3];

    mat[0]  = dot - light[0] * plane[0];
    mat[4]  = 0.0f - light[0] * plane[1];
    mat[8]  = 0.0f - light[0] * plane[2];
    mat[12] = 0.0f - light[0] * plane[3];

    mat[1]  = 0.0f - light[1] * plane[0];
    mat[5]  = dot - light[1] * plane[1];
    mat[9]  = 0.0f - light[1] * plane[2];
    mat[13] = 0.0f - light[1] * plane[3];

    mat[2]  = 0.0f - light[2] * plane[0];
    mat[6]  = 0.0f - light[2] * plane[1];
    mat[10] = dot - light[2] * plane[2];
    mat[14] = 0.0f - light[2] * plane[3];

    mat[3]  = 0.0f - light[3] * plane[0];
    mat[7]  = 0.0f - light[3] * plane[1];
    mat[11] = 0.0f - light[3] * plane[2];
    mat[15] = dot - light[3] * plane[3];
}

static const float ped_table[11][2] = {
    {  1.000000f,  0.000000f },
    {  0.809017f,  0.587785f },
    {  0.309017f,  0.951057f },
    { -0.309017f,  0.951057f },
    { -0.809017f,  0.587785f },
    { -1.000000f,  0.000000f },
    { -0.809017f, -0.587785f },
    { -0.309017f, -0.951057f },
    {  0.309017f, -0.951057f },
    {  0.809017f, -0.587785f },
    {  1.000000f,  0.000000f }
};

static void rasterize_fast_gouraud(GLContext *ctx,
                                   const gl_vertex_t *v0,
                                   const gl_vertex_t *v1,
                                   const gl_vertex_t *v2,
                                   const float c0[4],
                                   const float c1[4],
                                   const float c2[4]) {
    if (!ctx || !ctx->color_buffer || !ctx->z_buffer) return;

    const gl_vertex_t *a = v0;
    const gl_vertex_t *b = v1;
    const gl_vertex_t *c = v2;
    const float *col_a = c0 ? c0 : v0->color;
    const float *col_b = c1 ? c1 : v1->color;
    const float *col_c = c2 ? c2 : v2->color;

    if (a->win[1] > b->win[1]) {
        const gl_vertex_t *tmp = a; a = b; b = tmp;
        const float *ctmp = col_a; col_a = col_b; col_b = ctmp;
    }
    if (b->win[1] > c->win[1]) {
        const gl_vertex_t *tmp = b; b = c; c = tmp;
        const float *ctmp = col_b; col_b = col_c; col_c = ctmp;
    }
    if (a->win[1] > b->win[1]) {
        const gl_vertex_t *tmp = a; a = b; b = tmp;
        const float *ctmp = col_a; col_a = col_b; col_b = ctmp;
    }

    float y_a = a->win[1];
    float y_b = b->win[1];
    float y_c = c->win[1];
    float total_h = y_c - y_a;
    if (total_h < 0.5f) return;

    int min_y = (int)(y_a + 0.5f);
    int max_y = (int)(y_c - 0.5f);
    if (min_y < 0) min_y = 0;
    if (max_y >= ctx->height) max_y = ctx->height - 1;
    if (min_y > max_y) return;

    float inv_total_h = 1.0f / total_h;
    float h_ab = y_b - y_a;
    float inv_h_ab = (h_ab > 1e-4f) ? (1.0f / h_ab) : 0.0f;
    float h_bc = y_c - y_b;
    float inv_h_bc = (h_bc > 1e-4f) ? (1.0f / h_bc) : 0.0f;

    for (int y = min_y; y <= max_y; y++) {
        float fy = (float)y + 0.5f;

        /* Long edge A -> C */
        float t_ac = (fy - y_a) * inv_total_h;
        if (t_ac < 0.0f) t_ac = 0.0f; else if (t_ac > 1.0f) t_ac = 1.0f;
        float x1 = a->win[0] + t_ac * (c->win[0] - a->win[0]);
        float z1 = a->win[2] + t_ac * (c->win[2] - a->win[2]);
        float r1 = col_a[0] + t_ac * (col_c[0] - col_a[0]);
        float g1 = col_a[1] + t_ac * (col_c[1] - col_a[1]);
        float b1 = col_a[2] + t_ac * (col_c[2] - col_a[2]);

        /* Short edge */
        float x2, z2, r2, g2, b2;
        if (fy < y_b) {
            float t_ab = (fy - y_a) * inv_h_ab;
            if (t_ab < 0.0f) t_ab = 0.0f; else if (t_ab > 1.0f) t_ab = 1.0f;
            x2 = a->win[0] + t_ab * (b->win[0] - a->win[0]);
            z2 = a->win[2] + t_ab * (b->win[2] - a->win[2]);
            r2 = col_a[0] + t_ab * (col_b[0] - col_a[0]);
            g2 = col_a[1] + t_ab * (col_b[1] - col_a[1]);
            b2 = col_a[2] + t_ab * (col_b[2] - col_a[2]);
        } else {
            float t_bc = (fy - y_b) * inv_h_bc;
            if (t_bc < 0.0f) t_bc = 0.0f; else if (t_bc > 1.0f) t_bc = 1.0f;
            x2 = b->win[0] + t_bc * (c->win[0] - b->win[0]);
            z2 = b->win[2] + t_bc * (c->win[2] - b->win[2]);
            r2 = col_b[0] + t_bc * (col_c[0] - col_b[0]);
            g2 = col_b[1] + t_bc * (col_c[1] - col_b[1]);
            b2 = col_b[2] + t_bc * (col_c[2] - col_b[2]);
        }

        if (x1 > x2) {
            float tmp;
            tmp = x1; x1 = x2; x2 = tmp;
            tmp = z1; z1 = z2; z2 = tmp;
            tmp = r1; r1 = r2; r2 = tmp;
            tmp = g1; g1 = g2; g2 = tmp;
            tmp = b1; b1 = b2; b2 = tmp;
        }

        int min_x = (int)(x1 + 0.5f);
        int max_x = (int)(x2 - 0.5f);
        if (min_x < 0) min_x = 0;
        if (max_x >= ctx->width) max_x = ctx->width - 1;
        if (min_x > max_x) continue;

        float span_w = x2 - x1;
        float inv_span = (span_w > 1e-4f) ? (1.0f / span_w) : 0.0f;
        float dz_dx = (z2 - z1) * inv_span;
        float dr_dx = (r2 - r1) * inv_span;
        float dg_dx = (g2 - g1) * inv_span;
        float db_dx = (b2 - b1) * inv_span;

        float pre_x = ((float)min_x + 0.5f) - x1;
        float cur_z = z1 + pre_x * dz_dx;
        float cur_r = r1 + pre_x * dr_dx;
        float cur_g = g1 + pre_x * dg_dx;
        float cur_b = b1 + pre_x * db_dx;

        int32_t z_fx  = (int32_t)(cur_z * 65535.0f * 256.0f);
        int32_t dz_fx = (int32_t)(dz_dx * 65535.0f * 256.0f);
        int32_t r_fx  = (int32_t)(cur_r * 255.0f * 65536.0f);
        int32_t dr_fx = (int32_t)(dr_dx * 255.0f * 65536.0f);
        int32_t g_fx  = (int32_t)(cur_g * 255.0f * 65536.0f);
        int32_t dg_fx = (int32_t)(dg_dx * 255.0f * 65536.0f);
        int32_t b_fx  = (int32_t)(cur_b * 255.0f * 65536.0f);
        int32_t db_fx = (int32_t)(db_dx * 255.0f * 65536.0f);

        size_t row_offset = (size_t)y * (size_t)ctx->width + (size_t)min_x;
        uint16_t *z_buf = &ctx->z_buffer[row_offset];
        uint32_t *c_buf = &ctx->color_buffer[row_offset];
        int span_len = max_x - min_x + 1;

        for (int px = 0; px < span_len; px++) {
            uint16_t z_val = (z_fx <= 0) ? 0 : ((z_fx >= 16776960) ? 65535 : (uint16_t)((uint32_t)z_fx >> 8));
            if (z_val < z_buf[px]) {
                z_buf[px] = z_val;
                uint32_t cr = (uint32_t)((r_fx >> 16) & 0xFF);
                uint32_t cg = (uint32_t)((g_fx >> 16) & 0xFF);
                uint32_t cb = (uint32_t)((b_fx >> 16) & 0xFF);
                c_buf[px] = 0xFF000000u | (cr << 16) | (cg << 8) | cb;
            }
            z_fx += dz_fx;
            r_fx += dr_fx;
            g_fx += dg_fx;
            b_fx += db_fx;
        }
    }
}

static void rasterize_fast_shadow(GLContext *ctx,
                                  const gl_vertex_t *v0,
                                  const gl_vertex_t *v1,
                                  const gl_vertex_t *v2) {
    if (!ctx || !ctx->color_buffer || !ctx->z_buffer) return;

    const gl_vertex_t *a = v0;
    const gl_vertex_t *b = v1;
    const gl_vertex_t *c = v2;

    if (a->win[1] > b->win[1]) { const gl_vertex_t *tmp = a; a = b; b = tmp; }
    if (b->win[1] > c->win[1]) { const gl_vertex_t *tmp = b; b = c; c = tmp; }
    if (a->win[1] > b->win[1]) { const gl_vertex_t *tmp = a; a = b; b = tmp; }

    float y_a = a->win[1];
    float y_b = b->win[1];
    float y_c = c->win[1];
    float total_h = y_c - y_a;
    if (total_h < 0.5f) return;

    int min_y = (int)(y_a + 0.5f);
    int max_y = (int)(y_c - 0.5f);
    if (min_y < 0) min_y = 0;
    if (max_y >= ctx->height) max_y = ctx->height - 1;
    if (min_y > max_y) return;

    float inv_total_h = 1.0f / total_h;
    float h_ab = y_b - y_a;
    float inv_h_ab = (h_ab > 1e-4f) ? (1.0f / h_ab) : 0.0f;
    float h_bc = y_c - y_b;
    float inv_h_bc = (h_bc > 1e-4f) ? (1.0f / h_bc) : 0.0f;

    for (int y = min_y; y <= max_y; y++) {
        float fy = (float)y + 0.5f;

        /* Long edge A -> C */
        float t_ac = (fy - y_a) * inv_total_h;
        if (t_ac < 0.0f) t_ac = 0.0f; else if (t_ac > 1.0f) t_ac = 1.0f;
        float x1 = a->win[0] + t_ac * (c->win[0] - a->win[0]);
        float z1 = a->win[2] + t_ac * (c->win[2] - a->win[2]);

        /* Short edge */
        float x2, z2;
        if (fy < y_b) {
            float t_ab = (fy - y_a) * inv_h_ab;
            if (t_ab < 0.0f) t_ab = 0.0f; else if (t_ab > 1.0f) t_ab = 1.0f;
            x2 = a->win[0] + t_ab * (b->win[0] - a->win[0]);
            z2 = a->win[2] + t_ab * (b->win[2] - a->win[2]);
        } else {
            float t_bc = (fy - y_b) * inv_h_bc;
            if (t_bc < 0.0f) t_bc = 0.0f; else if (t_bc > 1.0f) t_bc = 1.0f;
            x2 = b->win[0] + t_bc * (c->win[0] - b->win[0]);
            z2 = b->win[2] + t_bc * (c->win[2] - b->win[2]);
        }

        if (x1 > x2) {
            float tmp;
            tmp = x1; x1 = x2; x2 = tmp;
            tmp = z1; z1 = z2; z2 = tmp;
        }

        int min_x = (int)(x1 + 0.5f);
        int max_x = (int)(x2 - 0.5f);
        if (min_x < 0) min_x = 0;
        if (max_x >= ctx->width) max_x = ctx->width - 1;
        if (min_x > max_x) continue;

        float span_w = x2 - x1;
        float inv_span = (span_w > 1e-4f) ? (1.0f / span_w) : 0.0f;
        float dz_dx = (z2 - z1) * inv_span;

        float pre_x = ((float)min_x + 0.5f) - x1;
        float cur_z = z1 + pre_x * dz_dx;

        int32_t z_fx  = (int32_t)(cur_z * 65535.0f * 256.0f);
        int32_t dz_fx = (int32_t)(dz_dx * 65535.0f * 256.0f);

        size_t row_offset = (size_t)y * (size_t)ctx->width + (size_t)min_x;
        uint16_t *z_buf = &ctx->z_buffer[row_offset];
        uint32_t *c_buf = &ctx->color_buffer[row_offset];
        int span_len = max_x - min_x + 1;

        for (int px = 0; px < span_len; px++) {
            uint16_t z_val = (z_fx <= 0) ? 0 : ((z_fx >= 16776960) ? 65535 : (uint16_t)((uint32_t)z_fx >> 8));
            if (z_val <= z_buf[px] + 8) {
                /* Realistic soft dark shadow blending */
                uint32_t c = c_buf[px];
                uint32_t cr = (((c >> 16) & 0xFF) * 45) >> 7;
                uint32_t cg = (((c >> 8)  & 0xFF) * 45) >> 7;
                uint32_t cb = ((c         & 0xFF) * 45) >> 7;
                c_buf[px] = 0xFF000000u | (cr << 16) | (cg << 8) | cb;
            }
            z_fx += dz_fx;
        }
    }
}

static inline void project_point(float ox, float oy, float oz,
                                 const GLfloat mv[16], const GLfloat pj[16],
                                 float half_w, float half_h, gl_vertex_t *out) {
    float ex = mv[0] * ox + mv[4] * oy + mv[8]  * oz + mv[12];
    float ey = mv[1] * ox + mv[5] * oy + mv[9]  * oz + mv[13];
    float ez = mv[2] * ox + mv[6] * oy + mv[10] * oz + mv[14];

    float cx = pj[0] * ex + pj[8]  * ez;
    float cy = pj[5] * ey + pj[9]  * ez;
    float cz = pj[10] * ez + pj[14];
    float cw = -ez;

    float inv_w = (cw > 0.01f || cw < -0.01f) ? (1.0f / cw) : 1.0f;
    out->inv_w = inv_w;
    out->win[0] = half_w + (cx * inv_w) * half_w;
    out->win[1] = half_h - (cy * inv_w) * half_h;
    out->win[2] = (cz * inv_w) * 0.5f + 0.5f;
}

static void draw_ground_pedestal(int vp_w, int vp_h) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;

    GLfloat mv[16], pj[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    glGetFloatv(GL_PROJECTION_MATRIX, pj);
    float half_w = (float)vp_w * 0.5f;
    float half_h = (float)vp_h * 0.5f;

    gl_vertex_t v_center;
    project_point(0.0f, -1.35f, 0.0f, mv, pj, half_w, half_h, &v_center);
    v_center.color[0] = 0.50f; v_center.color[1] = 0.52f; v_center.color[2] = 0.56f; v_center.color[3] = 1.0f;

    gl_vertex_t v_top[11];
    gl_vertex_t v_bot[11];
    for (int i = 0; i <= 10; i++) {
        float x = ped_table[i][0] * 2.8f;
        float z = ped_table[i][1] * 2.8f;
        project_point(x, -1.35f, z, mv, pj, half_w, half_h, &v_top[i]);
        v_top[i].color[0] = 0.38f; v_top[i].color[1] = 0.40f; v_top[i].color[2] = 0.44f; v_top[i].color[3] = 1.0f;
        project_point(x, -1.50f, z, mv, pj, half_w, half_h, &v_bot[i]);
        v_bot[i].color[0] = 0.20f; v_bot[i].color[1] = 0.22f; v_bot[i].color[2] = 0.25f; v_bot[i].color[3] = 1.0f;
    }

    /* Top fan - solid circular pedestal surface */
    for (int i = 0; i < 10; i++) {
        rasterize_fast_gouraud(ctx, &v_center, &v_top[i], &v_top[i+1], NULL, NULL, NULL);
    }
    /* Side rim */
    for (int i = 0; i < 10; i++) {
        rasterize_fast_gouraud(ctx, &v_top[i], &v_bot[i], &v_top[i+1], NULL, NULL, NULL);
        rasterize_fast_gouraud(ctx, &v_top[i+1], &v_bot[i], &v_bot[i+1], NULL, NULL, NULL);
    }
}

static float s_teapot_verts[530][3];
static bool s_teapot_inited = false;

static void init_teapot_mesh(void) {
    if (s_teapot_inited) return;
    for (int i = 0; i < numVertices; i++) {
        s_teapot_verts[i][0] = vertices[i][0] * 0.4334f - 0.139f;
        s_teapot_verts[i][1] = vertices[i][1] * 0.4334f - 0.91f;
        s_teapot_verts[i][2] = vertices[i][2] * 0.4334f;
    }
    s_teapot_inited = true;
}

static gl_vertex_t s_cached_shadow_verts[530];

static void draw_shadow_mesh(int vp_w, int vp_h, float rot_y, const float shadow_mat[16]) {
    init_teapot_mesh();
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;

    GLfloat mv[16], pj[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    glGetFloatv(GL_PROJECTION_MATRIX, pj);
    float half_w = (float)vp_w * 0.5f;
    float half_h = (float)vp_h * 0.5f;

    float rad = rot_y * (3.14159265f / 180.0f);
    float cos_r = gl_cosf(rad);
    float sin_r = gl_sinf(rad);

    /* 1. Project shadow vertices */
    for (int i = 0; i < numVertices; i++) {
        float ox = s_teapot_verts[i][0];
        float oy = s_teapot_verts[i][1];
        float oz = s_teapot_verts[i][2];

        float rx = ox * cos_r + oz * sin_r;
        float ry = oy;
        float rz = -ox * sin_r + oz * cos_r;

        float sx = shadow_mat[0] * rx + shadow_mat[4] * ry + shadow_mat[8]  * rz + shadow_mat[12];
        float sy = shadow_mat[1] * rx + shadow_mat[5] * ry + shadow_mat[9]  * rz + shadow_mat[13];
        float sz = shadow_mat[2] * rx + shadow_mat[6] * ry + shadow_mat[10] * rz + shadow_mat[14];
        float sw = shadow_mat[3] * rx + shadow_mat[7] * ry + shadow_mat[11] * rz + shadow_mat[15];
        if (sw != 1.0f && sw != 0.0f) {
            float inv_sw = 1.0f / sw;
            sx *= inv_sw; sy *= inv_sw; sz *= inv_sw;
        }

        project_point(sx, sy, sz, mv, pj, half_w, half_h, &s_cached_shadow_verts[i]);
    }

    /* 2. Rasterize shadow faces */
    for (int t = 0; t < numFaces; t += 4) {
        const int *f = faces[t];
        const gl_vertex_t *v0 = &s_cached_shadow_verts[f[1]];
        const gl_vertex_t *v1 = &s_cached_shadow_verts[f[2]];
        const gl_vertex_t *v2 = &s_cached_shadow_verts[f[3]];

        float area = (v1->win[0] - v0->win[0]) * (v2->win[1] - v0->win[1]) -
                     (v1->win[1] - v0->win[1]) * (v2->win[0] - v0->win[0]);
        if (area >= 0.0f) continue;

        rasterize_fast_shadow(ctx, v0, v1, v2);
    }
}

static gl_vertex_t s_cached_verts[530];
static float s_normal_colors[530][4];

static void draw_teapot_mesh(int vp_w, int vp_h) {
    init_teapot_mesh();
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;

    GLfloat mv[16];
    GLfloat pj[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    glGetFloatv(GL_PROJECTION_MATRIX, pj);

    float half_w = (float)vp_w * 0.5f;
    float half_h = (float)vp_h * 0.5f;

    /* Fixed light directions in eye/camera space for dynamic surface lighting */
    static const float l0_x = 0.52f, l0_y = 0.72f, l0_z = 0.46f;
    static const float l1_x = -0.65f, l1_y = 0.55f, l1_z = -0.52f;
    /* Blinn-Phong normalized half-vector between L0 and eye vector (0,0,1) */
    static const float h_x = 0.304f, h_y = 0.421f, h_z = 0.854f;

    const material_preset_t *mat = &materials[current_mat];

    /* 1. Precompute lighting colors for all 530 unique normals */
    for (int j = 0; j < numNormals; j++) {
        float onx = normals[j][0];
        float ony = normals[j][1];
        float onz = normals[j][2];

        /* Rotate normal into eye space */
        float enx = mv[0] * onx + mv[4] * ony + mv[8]  * onz;
        float eny = mv[1] * onx + mv[5] * ony + mv[9]  * onz;
        float enz = mv[2] * onx + mv[6] * ony + mv[10] * onz;

        float dot0 = enx * l0_x + eny * l0_y + enz * l0_z;
        if (dot0 < 0.0f) dot0 = 0.0f;

        float dot1 = 0.0f;
        if (light1_enabled) {
            dot1 = enx * l1_x + eny * l1_y + enz * l1_z;
            if (dot1 < 0.0f) dot1 = 0.0f;
        }

        float r = mat->ambient[0] + mat->diffuse[0] * dot0 + (light1_enabled ? 0.30f * dot1 : 0.0f);
        float g = mat->ambient[1] + mat->diffuse[1] * dot0 + (light1_enabled ? 0.38f * dot1 : 0.0f);
        float b = mat->ambient[2] + mat->diffuse[2] * dot0 + (light1_enabled ? 0.50f * dot1 : 0.0f);

        /* Dynamic moving Blinn-Phong specular highlight as teapot turns */
        float n_dot_h = enx * h_x + eny * h_y + enz * h_z;
        if (n_dot_h > 0.65f) {
            float spec = (n_dot_h - 0.65f) * 2.857f;
            spec = spec * spec * spec * spec;
            r += mat->specular[0] * spec;
            g += mat->specular[1] * spec;
            b += mat->specular[2] * spec;
        }

        if (r > 1.0f) r = 1.0f;
        if (g > 1.0f) g = 1.0f;
        if (b > 1.0f) b = 1.0f;

        s_normal_colors[j][0] = r;
        s_normal_colors[j][1] = g;
        s_normal_colors[j][2] = b;
        s_normal_colors[j][3] = 1.0f;
    }

    /* 2. Precompute screen coordinates for all 530 unique vertices */
    for (int i = 0; i < numVertices; i++) {
        gl_vertex_t *v = &s_cached_verts[i];
        float ox = s_teapot_verts[i][0];
        float oy = s_teapot_verts[i][1];
        float oz = s_teapot_verts[i][2];

        float ex = mv[0] * ox + mv[4] * oy + mv[8]  * oz + mv[12];
        float ey = mv[1] * ox + mv[5] * oy + mv[9]  * oz + mv[13];
        float ez = mv[2] * ox + mv[6] * oy + mv[10] * oz + mv[14];

        float cx = pj[0] * ex + pj[8]  * ez;
        float cy = pj[5] * ey + pj[9]  * ez;
        float cz = pj[10] * ez + pj[14];
        float cw = -ez;

        float inv_w = (cw != 0.0f) ? (1.0f / cw) : 1.0f;
        v->inv_w = inv_w;
        v->win[0] = half_w + (cx * inv_w) * half_w;
        v->win[1] = half_h - (cy * inv_w) * half_h;
        v->win[2] = (cz * inv_w) * 0.5f + 0.5f;
    }

    /* 3. Render 1,024 faces with instant 2D backface culling */
    for (int t = 0; t < numFaces; t++) {
        const int *f = faces[t];
        const gl_vertex_t *v0 = &s_cached_verts[f[1]];
        const gl_vertex_t *v1 = &s_cached_verts[f[2]];
        const gl_vertex_t *v2 = &s_cached_verts[f[3]];

        /* Fast 2D screen backface culling (CCW) */
        float area = (v1->win[0] - v0->win[0]) * (v2->win[1] - v0->win[1]) -
                     (v1->win[1] - v0->win[1]) * (v2->win[0] - v0->win[0]);
        if (area >= 0.0f) continue;

        rasterize_fast_gouraud(ctx, v0, v1, v2,
                               s_normal_colors[f[4]],
                               s_normal_colors[f[5]],
                               s_normal_colors[f[6]]);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[gl_teapot] Starting Optimized Realistic Utah Teapot...\n");

    uint32_t pid = (uint32_t)ipo_syscall(IPO_SYSCALL_GETPID, 0, NULL);
    if (pid == 0) pid = 1;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "Realistic 3D Utah Teapot";
    /* Cascade multi-instance windows based on PID */
    opt.x = 25 + ((pid * 26) % 120);
    opt.y = 15 + ((pid * 18) % 65);
    opt.w = DEFAULT_WIN_W;
    opt.h = DEFAULT_WIN_H;
    opt.decor_style = WM_DECOR_DEFAULT;
    opt.event_cb = on_window_event;

    wm_window_t *win = wm_create_window(&opt);
    if (!win) return 1;

    int cur_w = win->w;
    int cur_h = win->h;
    int gl_h = (cur_h > 14) ? (cur_h - 12) : cur_h;

    GLContext *gl = gl_create_context(cur_w, gl_h, win);
    if (!gl) {
        wm_destroy_window(win);
        return 1;
    }
    gl_make_current(gl);

    /* Configure GL rendering parameters */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    /* Normals are pre-normalized unit vectors; omit GL_NORMALIZE for 0 sqrtf calls */
    glShadeModel(GL_SMOOTH);

    /* Enable Backface Culling for 2x performance and clean geometry rendering */
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    /* Directional Light 0: Main Key Light (Warm, Top-Right-Front - casts dynamic side shadow) */
    GLfloat light0_pos[4]  = { 0.55f, 0.75f, 0.46f, 0.0f };
    GLfloat light0_amb[4]  = { 0.22f, 0.22f, 0.24f, 1.0f };
    GLfloat light0_diff[4] = { 1.0f, 0.96f, 0.90f, 1.0f };
    GLfloat light0_spec[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    glLightfv(GL_LIGHT0, GL_AMBIENT, light0_amb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light0_diff);
    glLightfv(GL_LIGHT0, GL_SPECULAR, light0_spec);

    /* Directional Light 1: Cool Azure Fill Light on Opposite Side (Top-Left-Rear) */
    GLfloat light1_pos[4]  = { -0.70f, 0.55f, -0.44f, 0.0f };
    GLfloat light1_diff[4] = { 0.40f, 0.55f, 0.75f, 1.0f };
    GLfloat light1_spec[4] = { 0.50f, 0.65f, 0.85f, 1.0f };

    glLightfv(GL_LIGHT1, GL_DIFFUSE, light1_diff);
    glLightfv(GL_LIGHT1, GL_SPECULAR, light1_spec);

    /* Ground plane equation at Y = -1.348f (slightly above Y = -1.35f floor to avoid Z-fighting) */
    const float ground_plane[4] = { 0.0f, 1.0f, 0.0f, 1.348f };
    float shadow_mat[16];
    build_shadow_matrix(shadow_mat, light0_pos, ground_plane);

    glClearColor(0.06f, 0.06f, 0.10f, 1.0f);

    uint32_t last_mat_switch = timer_millis();
    bool mouse_dragging = false;
    int last_mx = 0, last_my = 0;

    uint32_t fps_timer = timer_millis();
    uint32_t fps_frame_count = 0;
    float current_fps = 0.0f;

    /* Uncapped render loop */
    while (app_running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        uint32_t frame_start = timer_millis();

        /* Dynamic Window Resize Handler */
        if (win->w != cur_w || win->h != cur_h) {
            cur_w = win->w;
            cur_h = win->h;
            int gl_h = (cur_h > 14) ? (cur_h - 12) : cur_h;
            gl_resize_context(gl, cur_w, gl_h);
            glViewport(0, 0, cur_w, gl_h);
        }

        /* Mouse Drag Rotation Detection */
        mouse_state_t ms;
        mouse_get_state(&ms);
        if (ms.left_button) {
            if (!mouse_dragging) {
                /* Check if click is within window bounds */
                if (ms.x >= (int)win->x && ms.x < (int)win->x + (int)win->w &&
                    ms.y >= (int)win->y && ms.y < (int)win->y + (int)win->h) {
                    mouse_dragging = true;
                    last_mx = ms.x;
                    last_my = ms.y;
                }
            } else {
                int dx = ms.x - last_mx;
                int dy = ms.y - last_my;
                if (dx != 0 || dy != 0) {
                    rot_y += (float)dx * 1.5f;
                    rot_x += (float)dy * 1.5f;
                    auto_spin = false;
                    last_mx = ms.x;
                    last_my = ms.y;
                }
            }
        } else {
            mouse_dragging = false;
        }

        /* Auto-spin rotation */
        if (auto_spin) {
            rot_y += 1.8f;
            if (rot_y >= 360.0f) rot_y -= 360.0f;
        }

        gl_make_current(gl);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        /* Set projection with current aspect ratio */
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        int gl_h = (cur_h > 14) ? (cur_h - 12) : cur_h;
        double aspect = (gl_h > 0) ? ((double)cur_w / (double)gl_h) : 1.333;
        gluPerspective(45.0, aspect, 1.0, 50.0);

        /* Set modelview and camera position */
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, -0.4f, -cam_dist);
        glRotatef(rot_x, 1.0f, 0.0f, 0.0f);

        /* Set world-space light positions */
        glLightfv(GL_LIGHT0, GL_POSITION, light0_pos);
        if (light1_enabled) {
            glEnable(GL_LIGHT1);
            glLightfv(GL_LIGHT1, GL_POSITION, light1_pos);
        } else {
            glDisable(GL_LIGHT1);
        }

        /* 1. Draw studio ground pedestal */
        uint32_t t0 = timer_millis();
        draw_ground_pedestal(cur_w, gl_h);

        /* 2. Draw dynamic side shadow on ground plane */
        uint32_t t1 = timer_millis();
        if (shadows_enabled) {
            draw_shadow_mesh(cur_w, gl_h, rot_y, shadow_mat);
        }

        /* 3. Draw lit 3D Utah Teapot */
        uint32_t t2 = timer_millis();
        glPushMatrix();
        glRotatef(rot_y, 0.0f, 1.0f, 0.0f);

        const material_preset_t *m = &materials[current_mat];
        glMaterialfv(GL_FRONT, GL_AMBIENT, m->ambient);
        glMaterialfv(GL_FRONT, GL_DIFFUSE, m->diffuse);
        glMaterialfv(GL_FRONT, GL_SPECULAR, m->specular);
        glMaterialf(GL_FRONT, GL_SHININESS, m->shininess);

        draw_teapot_mesh(cur_w, gl_h);
        glPopMatrix();

        /* Swap GL backbuffer to window surface with strided quantization */
        uint32_t t3 = timer_millis();
        gl_swap_buffers(gl, win);
        uint32_t t4 = timer_millis();

        /* Real-time FPS calculation */
        fps_frame_count++;
        uint32_t now = timer_millis();
        if (now - fps_timer >= 1000) {
            current_fps = (float)fps_frame_count * 1000.0f / (float)(now - fps_timer);
            serial_printf("[gl_teapot] %d FPS (ped=%u shd=%u tea=%u swap=%u total=%u)\n",
                   (int)(current_fps + 0.5f), t1-t0, t2-t1, t3-t2, t4-t3, t4-t0);
            fps_frame_count = 0;
            fps_timer = now;
            char title_buf[64];
            snprintf(title_buf, sizeof(title_buf), "Utah Teapot [%d FPS]", (int)(current_fps + 0.5f));
            wm_set_title(win, title_buf);
        }

        /* Overlay Status Bar / Controls Help on bottom of window */
        if (win->framebuf && cur_h >= 40) {
            int bar_h = 12;
            int bar_y = cur_h - bar_h;
            wm_buf_fill_rect(win->framebuf, cur_w, cur_h, 0, bar_y, cur_w, bar_h, 16); /* Dark gray */
            char status_buf[96];
            snprintf(status_buf, sizeof(status_buf), "%d FPS|Shd:%s|Lgt:%s",
                     (int)(current_fps + 0.5f),
                     shadows_enabled ? "ON" : "OFF",
                     light1_enabled ? "DUAL" : "SINGLE");
            wm_buf_draw_string(win->framebuf, cur_w, cur_h, 4, bar_y + 2, status_buf, 15); /* White */
        }
        wm_invalidate(win);

        /* 60+ FPS pacing: cooperative single yield per frame to keep OS and mouse responsive */
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
    }

    gl_destroy_context(gl);
    if (wm_is_window_valid(win)) {
        wm_destroy_window(win);
    }

    return 0;
}
