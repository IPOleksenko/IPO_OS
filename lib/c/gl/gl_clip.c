#include <GL/gl.h>
#include <GL/ipo_gl.h>

#define MAX_CLIP_VERTS 32

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

/* Clip polygon against one plane: dot(P, plane) >= 0 */
static int clip_against_plane(const gl_vertex_t *in, int n_in, gl_vertex_t *out, int plane_idx) {
    int n_out = 0;
    if (n_in < 3) return 0;

    for (int i = 0; i < n_in; i++) {
        const gl_vertex_t *v0 = &in[i];
        const gl_vertex_t *v1 = &in[(i + 1) % n_in];

        float d0 = 0.0f, d1 = 0.0f;
        switch (plane_idx) {
            case 0: d0 = v0->clip[3] + v0->clip[0]; d1 = v1->clip[3] + v1->clip[0]; break; /* Left:   w + x >= 0 */
            case 1: d0 = v0->clip[3] - v0->clip[0]; d1 = v1->clip[3] - v1->clip[0]; break; /* Right:  w - x >= 0 */
            case 2: d0 = v0->clip[3] + v0->clip[1]; d1 = v1->clip[3] + v1->clip[1]; break; /* Bottom: w + y >= 0 */
            case 3: d0 = v0->clip[3] - v0->clip[1]; d1 = v1->clip[3] - v1->clip[1]; break; /* Top:    w - y >= 0 */
            case 4: d0 = v0->clip[3] + v0->clip[2]; d1 = v1->clip[3] + v1->clip[2]; break; /* Near:   w + z >= 0 */
            case 5: d0 = v0->clip[3] - v0->clip[2]; d1 = v1->clip[3] - v1->clip[2]; break; /* Far:    w - z >= 0 */
        }

        bool in0 = (d0 >= 0.0f);
        bool in1 = (d1 >= 0.0f);

        if (in0 && in1) {
            if (n_out < MAX_CLIP_VERTS) out[n_out++] = *v1;
        } else if (in0 && !in1) {
            float t = d0 / (d0 - d1);
            if (n_out < MAX_CLIP_VERTS) {
                interpolate_vertex(&out[n_out++], v0, v1, t);
            }
        } else if (!in0 && in1) {
            float t = d0 / (d0 - d1);
            if (n_out < MAX_CLIP_VERTS) {
                interpolate_vertex(&out[n_out++], v0, v1, t);
            }
            if (n_out < MAX_CLIP_VERTS) {
                out[n_out++] = *v1;
            }
        }
    }
    return n_out;
}

void gl_clip_and_render_polygon(GLContext *ctx, gl_vertex_t *verts, int nverts) {
    if (!ctx || nverts < 3) return;

    /* High-performance fast path for standard 3D triangles */
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

            float half_w = (float)ctx->vp_w * 0.5f;
            float half_h = (float)ctx->vp_h * 0.5f;
            float center_x = (float)ctx->vp_x + half_w;
            float center_y = (float)ctx->vp_y + half_h;

            float inv_w0 = 1.0f / w0;
            verts[0].inv_w = inv_w0;
            verts[0].win[0] = center_x + (verts[0].clip[0] * inv_w0) * half_w;
            verts[0].win[1] = center_y - (verts[0].clip[1] * inv_w0) * half_h;
            verts[0].win[2] = ((verts[0].clip[2] * inv_w0) + 1.0f) * 0.5f;

            float inv_w1 = 1.0f / w1;
            verts[1].inv_w = inv_w1;
            verts[1].win[0] = center_x + (verts[1].clip[0] * inv_w1) * half_w;
            verts[1].win[1] = center_y - (verts[1].clip[1] * inv_w1) * half_h;
            verts[1].win[2] = ((verts[1].clip[2] * inv_w1) + 1.0f) * 0.5f;

            float inv_w2 = 1.0f / w2;
            verts[2].inv_w = inv_w2;
            verts[2].win[0] = center_x + (verts[2].clip[0] * inv_w2) * half_w;
            verts[2].win[1] = center_y - (verts[2].clip[1] * inv_w2) * half_h;
            verts[2].win[2] = ((verts[2].clip[2] * inv_w2) + 1.0f) * 0.5f;

            /* Early Back-face culling check */
            if (ctx->cull_face_enabled) {
                float ax = verts[1].win[0] - verts[0].win[0];
                float ay = verts[1].win[1] - verts[0].win[1];
                float bx = verts[2].win[0] - verts[0].win[0];
                float by = verts[2].win[1] - verts[0].win[1];
                float cross = ax * by - ay * bx;

                bool is_front = (ctx->front_face == GL_CCW) ? (cross > 0.0f) : (cross < 0.0f);
                if (ctx->cull_face_mode == GL_BACK && !is_front) return;
                if (ctx->cull_face_mode == GL_FRONT && is_front) return;
                if (ctx->cull_face_mode == GL_FRONT_AND_BACK) return;
            }

            gl_rasterize_triangle(ctx, &verts[0], &verts[1], &verts[2]);
            return;
        }
    }

    gl_vertex_t buf0[MAX_CLIP_VERTS];
    gl_vertex_t buf1[MAX_CLIP_VERTS];

    int count = nverts > MAX_CLIP_VERTS ? MAX_CLIP_VERTS : nverts;
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

    /* Transform remaining clipped vertices to screen coordinates */
    float half_w = (float)ctx->vp_w * 0.5f;
    float half_h = (float)ctx->vp_h * 0.5f;
    float center_x = (float)ctx->vp_x + half_w;
    float center_y = (float)ctx->vp_y + half_h;

    for (int i = 0; i < count; i++) {
        gl_vertex_t *v = &curr_in[i];
        float w = v->clip[3];
        if (w == 0.0f) w = 0.00001f;
        v->inv_w = 1.0f / w;
        v->win[0] = center_x + (v->clip[0] * v->inv_w) * half_w;
        v->win[1] = center_y - (v->clip[1] * v->inv_w) * half_h; /* Y inverted for screen coordinates */
        v->win[2] = ((v->clip[2] * v->inv_w) + 1.0f) * 0.5f;
    }

    /* Back-face culling check */
    if (ctx->cull_face_enabled) {
        float ax = curr_in[1].win[0] - curr_in[0].win[0];
        float ay = curr_in[1].win[1] - curr_in[0].win[1];
        float bx = curr_in[2].win[0] - curr_in[0].win[0];
        float by = curr_in[2].win[1] - curr_in[0].win[1];
        float cross = ax * by - ay * bx;

        bool is_front = (ctx->front_face == GL_CCW) ? (cross > 0.0f) : (cross < 0.0f);
        if (ctx->cull_face_mode == GL_BACK && !is_front) return;
        if (ctx->cull_face_mode == GL_FRONT && is_front) return;
        if (ctx->cull_face_mode == GL_FRONT_AND_BACK) return;
    }

    /* Triangle fan rasterization */
    for (int i = 1; i < count - 1; i++) {
        gl_rasterize_triangle(ctx, &curr_in[0], &curr_in[i], &curr_in[i + 1]);
    }
}

