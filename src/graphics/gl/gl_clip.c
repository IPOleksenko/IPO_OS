#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <memory/kmalloc.h>
#include <stdint.h>
#include <limits.h>

/* Interpolate between two vertices with parameter t in [0.0, 1.0] */
static void interpolate_vertex(gl_vertex_t *out, const gl_vertex_t *v0, const gl_vertex_t *v1, float t) {
    float inv_t = 1.0f - t;
    for (int i = 0; i < 4; i++) {
        out->obj[i]  = inv_t * v0->obj[i]  + t * v1->obj[i];
        out->eye[i]  = inv_t * v0->eye[i]  + t * v1->eye[i];
        out->clip[i] = inv_t * v0->clip[i] + t * v1->clip[i];
        out->color[i]= inv_t * v0->color[i]+ t * v1->color[i];
        out->tex[i]  = inv_t * v0->tex[i]  + t * v1->tex[i];
    }
    for (int i = 0; i < 3; i++) {
        out->norm[i] = inv_t * v0->norm[i] + t * v1->norm[i];
    }
}

static float plane_distance(const gl_vertex_t *v, int plane_idx) {
    switch (plane_idx) {
        case 0: return v->clip[3] + v->clip[0]; /* Left:   w + x >= 0 */
        case 1: return v->clip[3] - v->clip[0]; /* Right:  w - x >= 0 */
        case 2: return v->clip[3] + v->clip[1]; /* Bottom: w + y >= 0 */
        case 3: return v->clip[3] - v->clip[1]; /* Top:    w - y >= 0 */
        case 4: return v->clip[3] + v->clip[2]; /* Near:   w + z >= 0 */
        case 5: return v->clip[3] - v->clip[2]; /* Far:    w - z >= 0 */
        default: return 0.0f;
    }
}

static void ndc_to_screen_vertex(const GLContext *ctx, gl_vertex_t *v) {
    float w = v->clip[3];
    if (w == 0.0f) w = 0.00001f;

    float inv_w = 1.0f / w;
    float half_w = (float)ctx->vp_w * 0.5f;
    float half_h = (float)ctx->vp_h * 0.5f;
    float center_x = (float)ctx->vp_x + half_w;
    float center_y = (float)ctx->vp_y + half_h;
    float z_ndc = v->clip[2] * inv_w;

    v->inv_w  = inv_w;
    v->win[0] = center_x + (v->clip[0] * inv_w) * half_w;
    v->win[1] = center_y - (v->clip[1] * inv_w) * half_h; /* Y inverted for screen coordinates */
    v->win[2] = (float)ctx->depth_near +
                ((z_ndc + 1.0f) * 0.5f) * (float)(ctx->depth_far - ctx->depth_near);
}

static bool should_cull_triangle(const GLContext *ctx, const gl_vertex_t *v0,
                                 const gl_vertex_t *v1, const gl_vertex_t *v2) {
    if (!ctx->cull_face_enabled) return false;
    if (ctx->cull_face_mode == GL_FRONT_AND_BACK) return true;

    float ax = v1->win[0] - v0->win[0];
    float ay = v1->win[1] - v0->win[1];
    float bx = v2->win[0] - v0->win[0];
    float by = v2->win[1] - v0->win[1];
    float cross = ax * by - ay * bx;
    if (cross == 0.0f) return true;

    /* Screen Y grows downward, so a CCW triangle in NDC has negative 2D cross product. */
    bool is_front = (ctx->front_face == GL_CCW) ? (cross < 0.0f) : (cross > 0.0f);
    if (ctx->cull_face_mode == GL_BACK) return !is_front;
    if (ctx->cull_face_mode == GL_FRONT) return is_front;
    return false;
}

/* Clip polygon against one plane: dot(P, plane) >= 0 */
static int clip_against_plane(const gl_vertex_t *in, int n_in, gl_vertex_t *out, int plane_idx) {
    int n_out = 0;
    if (n_in < 3) return 0;

    for (int i = 0; i < n_in; i++) {
        const gl_vertex_t *v0 = &in[i];
        const gl_vertex_t *v1 = &in[(i + 1) % n_in];

        float d0 = plane_distance(v0, plane_idx);
        float d1 = plane_distance(v1, plane_idx);

        bool in0 = (d0 >= 0.0f);
        bool in1 = (d1 >= 0.0f);

        if (in0 && in1) {
            out[n_out++] = *v1;
        } else if (in0 && !in1) {
            float t = d0 / (d0 - d1);
            interpolate_vertex(&out[n_out++], v0, v1, t);
        } else if (!in0 && in1) {
            float t = d0 / (d0 - d1);
            interpolate_vertex(&out[n_out++], v0, v1, t);
            out[n_out++] = *v1;
        }
    }
    return n_out;
}

void gl_clip_and_render_point(GLContext *ctx, const gl_vertex_t *v0) {
    if (!ctx || !v0) return;

    float w = v0->clip[3];
    if (w <= 0.0f) return;
    if (v0->clip[0] < -w || v0->clip[0] > w ||
        v0->clip[1] < -w || v0->clip[1] > w ||
        v0->clip[2] < -w || v0->clip[2] > w) {
        return;
    }

    gl_vertex_t pt = *v0;
    ndc_to_screen_vertex(ctx, &pt);
    gl_rasterize_point(ctx, &pt);
}

void gl_clip_and_render_line(GLContext *ctx, const gl_vertex_t *v0, const gl_vertex_t *v1) {
    if (!ctx || !v0 || !v1) return;

    float t0 = 0.0f;
    float t1 = 1.0f;

    for (int p = 0; p < 6; p++) {
        float d0 = plane_distance(v0, p);
        float d1 = plane_distance(v1, p);

        if (d0 < 0.0f && d1 < 0.0f) return;

        float denom = d0 - d1;
        if (denom != 0.0f) {
            float t = d0 / denom;
            if (d0 < 0.0f) {
                if (t > t0) t0 = t;
            } else if (d1 < 0.0f) {
                if (t < t1) t1 = t;
            }
            if (t0 > t1) return;
        }
    }

    gl_vertex_t a, b;
    if (t0 == 0.0f) a = *v0;
    else interpolate_vertex(&a, v0, v1, t0);

    if (t1 == 1.0f) b = *v1;
    else interpolate_vertex(&b, v0, v1, t1);

    ndc_to_screen_vertex(ctx, &a);
    ndc_to_screen_vertex(ctx, &b);
    gl_rasterize_line(ctx, &a, &b);
}

void gl_clip_and_render_polygon(GLContext *ctx, gl_vertex_t *verts, int nverts) {
    if (!ctx || nverts < 3) return;

    /* Fast path for standard 3D triangles completely outside or inside the frustum */
    if (nverts == 3) {
        float w0 = verts[0].clip[3], w1 = verts[1].clip[3], w2 = verts[2].clip[3];

        /* Frustum Trivial Reject */
        if ((verts[0].clip[0] < -w0 && verts[1].clip[0] < -w1 && verts[2].clip[0] < -w2) ||
            (verts[0].clip[0] >  w0 && verts[1].clip[0] >  w1 && verts[2].clip[0] >  w2) ||
            (verts[0].clip[1] < -w0 && verts[1].clip[1] < -w1 && verts[2].clip[1] < -w2) ||
            (verts[0].clip[1] >  w0 && verts[1].clip[1] >  w1 && verts[2].clip[1] >  w2) ||
            (verts[0].clip[2] < -w0 && verts[1].clip[2] < -w1 && verts[2].clip[2] < -w2) ||
            (verts[0].clip[2] >  w0 && verts[1].clip[2] >  w1 && verts[2].clip[2] >  w2)) {
            return;
        }

        /* Frustum Trivial Accept: all 3 vertices completely inside view volume */
        if (w0 > 0.0f && w1 > 0.0f && w2 > 0.0f &&
            verts[0].clip[0] >= -w0 && verts[0].clip[0] <= w0 &&
            verts[0].clip[1] >= -w0 && verts[0].clip[1] <= w0 &&
            verts[0].clip[2] >= -w0 && verts[0].clip[2] <= w0 &&
            verts[1].clip[0] >= -w1 && verts[1].clip[0] <= w1 &&
            verts[1].clip[1] >= -w1 && verts[1].clip[1] <= w1 &&
            verts[1].clip[2] >= -w1 && verts[1].clip[2] <= w1 &&
            verts[2].clip[0] >= -w2 && verts[2].clip[0] <= w2 &&
            verts[2].clip[1] >= -w2 && verts[2].clip[1] <= w2 &&
            verts[2].clip[2] >= -w2 && verts[2].clip[2] <= w2) {

            for (int i = 0; i < 3; i++) {
                ndc_to_screen_vertex(ctx, &verts[i]);
            }

            if (should_cull_triangle(ctx, &verts[0], &verts[1], &verts[2])) return;

            gl_rasterize_triangle(ctx, &verts[0], &verts[1], &verts[2]);
            return;
        }
    }

    if (nverts > INT_MAX - 6) {
        if (ctx->error_flag == GL_NO_ERROR) ctx->error_flag = GL_OUT_OF_MEMORY;
        return;
    }

    int required_capacity = nverts + 6;
    if (ctx->clip_capacity < required_capacity) {
        size_t capacity = (size_t)required_capacity;
        if (capacity > SIZE_MAX / (2 * sizeof(gl_vertex_t))) {
            if (ctx->error_flag == GL_NO_ERROR) ctx->error_flag = GL_OUT_OF_MEMORY;
            return;
        }

        gl_vertex_t *new_buf = (gl_vertex_t *)krealloc(
            ctx->clip_buf, capacity * 2 * sizeof(gl_vertex_t));
        if (!new_buf) {
            if (ctx->error_flag == GL_NO_ERROR) ctx->error_flag = GL_OUT_OF_MEMORY;
            return;
        }
        ctx->clip_buf = new_buf;
        ctx->clip_capacity = required_capacity;
    }

    gl_vertex_t *buf0 = ctx->clip_buf;
    gl_vertex_t *buf1 = ctx->clip_buf + ctx->clip_capacity;
    int count = nverts;
    for (int i = 0; i < count; i++) buf0[i] = verts[i];

    gl_vertex_t *curr_in = buf0;
    gl_vertex_t *curr_out = buf1;

    for (int p = 0; p < 6; p++) {
        count = clip_against_plane(curr_in, count, curr_out, p);
        if (count < 3) return; /* Fully clipped */
        gl_vertex_t *tmp = curr_in;
        curr_in = curr_out;
        curr_out = tmp;
    }

    for (int i = 0; i < count; i++) {
        ndc_to_screen_vertex(ctx, &curr_in[i]);
    }

    if (should_cull_triangle(ctx, &curr_in[0], &curr_in[1], &curr_in[2])) return;

    /* Triangle fan rasterization */
    for (int i = 1; i < count - 1; i++) {
        gl_rasterize_triangle(ctx, &curr_in[0], &curr_in[i], &curr_in[i + 1]);
    }
}
