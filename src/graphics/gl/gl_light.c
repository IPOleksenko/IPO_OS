#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <GL/gl_math.h>
#include <memory/kmalloc.h>
#include <string.h>

static void init_light_defaults(gl_light_t *l, size_t index) {
    memset(l, 0, sizeof(gl_light_t));
    l->ambient[3] = 1.0f;
    l->position[2] = 1.0f;
    l->spot_direction[2] = -1.0f;
    l->spot_cutoff = 180.0f;
    l->constant_attenuation = 1.0f;

    if (index == 0) {
        l->diffuse[0] = 1.0f;
        l->diffuse[1] = 1.0f;
        l->diffuse[2] = 1.0f;
        l->diffuse[3] = 1.0f;
        l->specular[0] = 1.0f;
        l->specular[1] = 1.0f;
        l->specular[2] = 1.0f;
        l->specular[3] = 1.0f;
    } else {
        l->diffuse[3] = 1.0f;
        l->specular[3] = 1.0f;
    }
}

static float gl_pow_unit(float base, float exp) {
    if (exp <= 0.0f) return 1.0f;
    if (base <= 0.0f) return 0.0f;
    if (base >= 1.0f) return 1.0f;

    /* Reduce base into [0.5, 1.0] so the series converges rapidly. */
    float x = base;
    int shifts = 0;
    while (x < 0.5f && shifts < 32) {
        x *= 2.0f;
        shifts++;
    }
    if (shifts >= 32 && exp >= 1.0f) return 0.0f;

    float u = (x - 1.0f) / (x + 1.0f);
    float u2 = u * u;
    float lnx = 2.0f * u * (1.0f + u2 * (0.3333333f + u2 * (0.2f + u2 * 0.1428571f)));
    lnx -= (float)shifts * 0.69314718f;

    return gl_exp_neg(exp * lnx);
}

gl_light_t *gl_get_or_create_light(GLContext *ctx, size_t index) {
    if (!ctx) return NULL;
    if (index >= ctx->lights_capacity) {
        size_t new_cap = (ctx->lights_capacity == 0) ? 8 : (ctx->lights_capacity * 2);
        while (new_cap <= index) new_cap *= 2;
        gl_light_t *new_lights = (gl_light_t *)krealloc(ctx->lights, new_cap * sizeof(gl_light_t));
        if (!new_lights) return NULL;
        for (size_t i = ctx->lights_capacity; i < new_cap; i++) {
            init_light_defaults(&new_lights[i], i);
        }
        ctx->lights = new_lights;
        ctx->lights_capacity = new_cap;
    }
    if (index >= ctx->lights_count) {
        ctx->lights_count = index + 1;
    }
    return &ctx->lights[index];
}

void gl_evaluate_vertex_lighting(GLContext *ctx, gl_vertex_t *v) {
    if (!ctx || !v || !ctx->lighting_enabled) return;

    const float *mat_amb = ctx->color_material_enabled ? v->color : ctx->front_material.ambient;
    const float *mat_diff = ctx->color_material_enabled ? v->color : ctx->front_material.diffuse;
    const float *mat_spec = ctx->front_material.specular;
    const float *mat_emis = ctx->front_material.emission;

    /* Default GL_LIGHT_MODEL_AMBIENT is (0.2, 0.2, 0.2, 1.0). */
    float r = mat_emis[0] + 0.2f * mat_amb[0];
    float g = mat_emis[1] + 0.2f * mat_amb[1];
    float b = mat_emis[2] + 0.2f * mat_amb[2];
    float a = mat_diff[3];

    float nx = v->norm[0], ny = v->norm[1], nz = v->norm[2];
    if (ctx->normalize_normals) {
        float len = gl_sqrtf(nx * nx + ny * ny + nz * nz);
        if (len > 0.0f) {
            nx /= len;
            ny /= len;
            nz /= len;
        }
    }

    for (size_t i = 0; i < ctx->lights_count; i++) {
        gl_light_t *l = &ctx->lights[i];
        if (!l->enabled) continue;

        float lx = l->position[0];
        float ly = l->position[1];
        float lz = l->position[2];
        float lw = l->position[3];
        float dist = 0.0f;
        float att = 1.0f;

        if (lw != 0.0f) {
            float px = lx / lw;
            float py = ly / lw;
            float pz = lz / lw;
            lx = px - v->eye[0];
            ly = py - v->eye[1];
            lz = pz - v->eye[2];
            dist = gl_sqrtf(lx * lx + ly * ly + lz * lz);
            if (dist > 0.0f) {
                lx /= dist;
                ly /= dist;
                lz /= dist;
            }
            float denom = l->constant_attenuation +
                          l->linear_attenuation * dist +
                          l->quadratic_attenuation * dist * dist;
            if (denom > 0.0f) {
                att = 1.0f / denom;
            }
        } else {
            float len = gl_sqrtf(lx * lx + ly * ly + lz * lz);
            if (len > 0.0f) {
                lx /= len;
                ly /= len;
                lz /= len;
            }
        }

        float spot = 1.0f;
        if (lw != 0.0f && l->spot_cutoff != 180.0f) {
            float sx = l->spot_direction[0];
            float sy = l->spot_direction[1];
            float sz = l->spot_direction[2];
            float slen = gl_sqrtf(sx * sx + sy * sy + sz * sz);
            if (slen > 0.0f) {
                sx /= slen;
                sy /= slen;
                sz /= slen;
            }
            float spot_cos = -(lx * sx + ly * sy + lz * sz);
            float cutoff_cos = gl_cosf(l->spot_cutoff * 3.14159265358979323846f / 180.0f);
            if (spot_cos < cutoff_cos) {
                spot = 0.0f;
            } else if (spot_cos > 0.0f) {
                spot = gl_pow_unit(spot_cos, l->spot_exponent);
            } else {
                spot = (l->spot_exponent == 0.0f) ? 1.0f : 0.0f;
            }
        }

        float n_dot_l = nx * lx + ny * ly + nz * lz;
        if (n_dot_l < 0.0f) n_dot_l = 0.0f;

        float lr = l->ambient[0] * mat_amb[0] + l->diffuse[0] * mat_diff[0] * n_dot_l;
        float lg = l->ambient[1] * mat_amb[1] + l->diffuse[1] * mat_diff[1] * n_dot_l;
        float lb = l->ambient[2] * mat_amb[2] + l->diffuse[2] * mat_diff[2] * n_dot_l;

        if (n_dot_l > 0.0f) {
            /* Blinn-Phong half vector for local/infinite viewer along +Z in eye space. */
            float hx = lx;
            float hy = ly;
            float hz = lz + 1.0f;
            float hlen = gl_sqrtf(hx * hx + hy * hy + hz * hz);
            if (hlen > 0.0f) {
                hx /= hlen;
                hy /= hlen;
                hz /= hlen;
                float n_dot_h = nx * hx + ny * hy + nz * hz;
                if (n_dot_h > 0.0f) {
                    float spec = gl_pow_unit(n_dot_h, ctx->front_material.shininess);
                    lr += l->specular[0] * mat_spec[0] * spec;
                    lg += l->specular[1] * mat_spec[1] * spec;
                    lb += l->specular[2] * mat_spec[2] * spec;
                }
            }
        }

        float factor = att * spot;
        r += lr * factor;
        g += lg * factor;
        b += lb * factor;
    }

    if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
    if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;

    v->color[0] = r;
    v->color[1] = g;
    v->color[2] = b;
    v->color[3] = a;
}

void glShadeModel(GLenum mode) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (mode == GL_FLAT || mode == GL_SMOOTH) {
        ctx->shade_model = mode;
    } else {
        ctx->error_flag = GL_INVALID_ENUM;
    }
}

void glLightf(GLenum light, GLenum pname, GLfloat param) {
    glLightfv(light, pname, &param);
}

void glLighti(GLenum light, GLenum pname, GLint param) {
    GLfloat f = (GLfloat)param;
    glLightfv(light, pname, &f);
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !params) return;
    if (light < GL_LIGHT0) {
        ctx->error_flag = GL_INVALID_ENUM;
        return;
    }

    gl_light_t *l = gl_get_or_create_light(ctx, (size_t)(light - GL_LIGHT0));
    if (!l) {
        ctx->error_flag = GL_OUT_OF_MEMORY;
        return;
    }
    switch (pname) {
        case GL_AMBIENT:               memcpy(l->ambient, params, 4 * sizeof(GLfloat)); break;
        case GL_DIFFUSE:               memcpy(l->diffuse, params, 4 * sizeof(GLfloat)); break;
        case GL_SPECULAR:              memcpy(l->specular, params, 4 * sizeof(GLfloat)); break;
        case GL_POSITION:              memcpy(l->position, params, 4 * sizeof(GLfloat)); break;
        case GL_SPOT_DIRECTION:        memcpy(l->spot_direction, params, 3 * sizeof(GLfloat)); break;
        case GL_SPOT_EXPONENT:         l->spot_exponent = params[0]; break;
        case GL_SPOT_CUTOFF:           l->spot_cutoff = params[0]; break;
        case GL_CONSTANT_ATTENUATION:  l->constant_attenuation = params[0]; break;
        case GL_LINEAR_ATTENUATION:    l->linear_attenuation = params[0]; break;
        case GL_QUADRATIC_ATTENUATION: l->quadratic_attenuation = params[0]; break;
        default:                       ctx->error_flag = GL_INVALID_ENUM; break;
    }
}

void glMaterialf(GLenum face, GLenum pname, GLfloat param) {
    glMaterialfv(face, pname, &param);
}

static void apply_material_param(GLContext *ctx, gl_material_t *m, GLenum pname, const GLfloat *params) {
    switch (pname) {
        case GL_AMBIENT:
            memcpy(m->ambient, params, 4 * sizeof(GLfloat));
            break;
        case GL_DIFFUSE:
            memcpy(m->diffuse, params, 4 * sizeof(GLfloat));
            break;
        case GL_SPECULAR:
            memcpy(m->specular, params, 4 * sizeof(GLfloat));
            break;
        case GL_EMISSION:
            memcpy(m->emission, params, 4 * sizeof(GLfloat));
            break;
        case GL_SHININESS:
            m->shininess = params[0];
            break;
        case GL_AMBIENT_AND_DIFFUSE:
            memcpy(m->ambient, params, 4 * sizeof(GLfloat));
            memcpy(m->diffuse, params, 4 * sizeof(GLfloat));
            break;
        default:
            ctx->error_flag = GL_INVALID_ENUM;
            break;
    }
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || !params) return;

    if (face == GL_FRONT) {
        apply_material_param(ctx, &ctx->front_material, pname, params);
    } else if (face == GL_BACK) {
        apply_material_param(ctx, &ctx->back_material, pname, params);
    } else if (face == GL_FRONT_AND_BACK) {
        apply_material_param(ctx, &ctx->front_material, pname, params);
        apply_material_param(ctx, &ctx->back_material, pname, params);
    } else {
        ctx->error_flag = GL_INVALID_ENUM;
    }
}
