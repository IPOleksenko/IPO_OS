#ifndef LIB_IPO_GL_H
#define LIB_IPO_GL_H

#include <GL/gl.h>
#include <GL/glu.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dynamic 4x4 matrix stack without fixed limits */
typedef struct {
    GLfloat *data;       /* 16 floats per matrix */
    int      top;        /* index of current matrix (0 = bottom) */
    int      capacity;   /* max matrices currently allocated */
} gl_matrix_stack_t;

/* Vertex structure with transformed clip and screen coordinates */
typedef struct {
    GLfloat obj[4];      /* x, y, z, w in object coordinates */
    GLfloat eye[4];      /* eye coordinates */
    GLfloat clip[4];     /* clip coordinates */
    GLfloat norm[3];     /* transformed normal */
    GLfloat color[4];    /* RGBA color [0.0f, 1.0f] */
    GLfloat tex[4];      /* s, t, r, q texture coordinates */
    GLfloat win[3];      /* screen x, y, z */
    GLfloat inv_w;       /* 1 / w */
} gl_vertex_t;

/* Dynamic vertex buffer */
typedef struct {
    gl_vertex_t *data;
    int          count;
    int          capacity;
} gl_vertex_buf_t;

/* Texture representation */
typedef struct {
    GLuint    id;
    GLenum    target;     /* GL_TEXTURE_1D, 2D, 3D */
    GLint     width;
    GLint     height;
    GLint     depth;
    GLenum    format;
    GLenum    type;
    uint32_t *pixels;     /* 32-bit ARGB/RGBA texel storage */
    GLenum    min_filter;
    GLenum    mag_filter;
    GLenum    wrap_s;
    GLenum    wrap_t;
    GLenum    wrap_r;
    bool      in_use;
} gl_texture_t;

/* Light representation */
typedef struct {
    GLboolean enabled;
    GLfloat   ambient[4];
    GLfloat   diffuse[4];
    GLfloat   specular[4];
    GLfloat   position[4];
    GLfloat   spot_direction[3];
    GLfloat   spot_exponent;
    GLfloat   spot_cutoff;
    GLfloat   constant_attenuation;
    GLfloat   linear_attenuation;
    GLfloat   quadratic_attenuation;
} gl_light_t;

/* Material properties */
typedef struct {
    GLfloat ambient[4];
    GLfloat diffuse[4];
    GLfloat specular[4];
    GLfloat emission[4];
    GLfloat shininess;
} gl_material_t;

/* Display list dynamic storage */
typedef struct {
    uint8_t *commands;
    size_t   size;
    size_t   capacity;
    bool     valid;
} gl_display_list_t;

/* Complete GL Context */
typedef struct GLContext {
    /* Framebuffer and dimensions */
    int          width;
    int          height;
    uint32_t    *color_buffer;    /* 32-bit ARGB/RGBA (w * h) */
    uint16_t    *z_buffer;        /* 16-bit depth (w * h) */
    uint8_t     *stencil_buffer;  /* 8-bit stencil (w * h) */
    void        *win;             /* Pointer to wm_window_t */

    /* Clear values */
    GLclampf     clear_color[4];
    GLclampd     clear_depth;
    GLint        clear_stencil;

    /* Viewport & Scissor */
    GLint        vp_x, vp_y;
    GLsizei      vp_w, vp_h;
    GLdouble     depth_near, depth_far;
    GLboolean    scissor_enabled;
    GLint        scissor_x, scissor_y;
    GLsizei      scissor_w, scissor_h;

    /* Matrix stacks */
    GLenum             matrix_mode;
    gl_matrix_stack_t  modelview_stack;
    gl_matrix_stack_t  projection_stack;
    gl_matrix_stack_t  texture_stack;

    /* Current attributes */
    GLfloat      cur_color[4];
    GLfloat      cur_normal[3];
    GLfloat      cur_texcoord[4];

    /* Immediate geometry buffer */
    GLenum           prim_mode;
    bool             in_begin;
    gl_vertex_buf_t  vert_buf;

    /* Vertex Arrays state */
    GLboolean    va_vertex_enabled;
    GLint        va_vertex_size;
    GLenum       va_vertex_type;
    GLsizei      va_vertex_stride;
    const void  *va_vertex_ptr;

    GLboolean    va_color_enabled;
    GLint        va_color_size;
    GLenum       va_color_type;
    GLsizei      va_color_stride;
    const void  *va_color_ptr;

    GLboolean    va_normal_enabled;
    GLenum       va_normal_type;
    GLsizei      va_normal_stride;
    const void  *va_normal_ptr;

    GLboolean    va_texcoord_enabled;
    GLint        va_texcoord_size;
    GLenum       va_texcoord_type;
    GLsizei      va_texcoord_stride;
    const void  *va_texcoord_ptr;

    /* Lighting & Shading */
    GLboolean    lighting_enabled;
    GLenum       shade_model;
    gl_light_t   lights[8];
    gl_material_t front_material;
    gl_material_t back_material;
    GLboolean    color_material_enabled;
    GLboolean    normalize_normals;

    /* Rasterization & Polygon */
    GLenum       polygon_mode;
    GLboolean    cull_face_enabled;
    GLenum       cull_face_mode;
    GLenum       front_face;
    GLfloat      point_size;
    GLfloat      line_width;

    /* Depth, Alpha & Stencil Testing */
    GLboolean    depth_test_enabled;
    GLenum       depth_func;
    GLboolean    depth_mask;

    GLboolean    alpha_test_enabled;
    GLenum       alpha_func;
    GLclampf     alpha_ref;

    GLboolean    stencil_test_enabled;
    GLenum       stencil_func;
    GLint        stencil_ref;
    GLuint       stencil_mask;
    GLenum       stencil_fail;
    GLenum       stencil_zfail;
    GLenum       stencil_zpass;
    GLuint       stencil_writemask;

    /* Blending & LogicOp */
    GLboolean    blend_enabled;
    GLenum       blend_sfactor;
    GLenum       blend_dfactor;
    GLenum       blend_equation;

    GLboolean    logic_op_enabled;
    GLenum       logic_opcode;

    /* Fog */
    GLboolean    fog_enabled;
    GLenum       fog_mode;
    GLfloat      fog_density;
    GLfloat      fog_start;
    GLfloat      fog_end;
    GLfloat      fog_color[4];

    /* Texturing */
    GLboolean     texture_2d_enabled;
    GLboolean     texture_3d_enabled;
    GLuint        current_texture_2d;
    GLuint        current_texture_3d;
    GLenum        tex_env_mode;
    gl_texture_t **textures;
    int           texture_count;
    int           texture_capacity;

    /* Display Lists */
    gl_display_list_t **display_lists;
    int           list_count;
    int           list_capacity;
    GLuint        current_list;
    GLenum        list_compile_mode;
    bool          is_compiling_list;

    /* Error handling */
    GLenum       error_flag;
} GLContext;

/* Context Management API */
GLContext *gl_create_context(int width, int height, void *win);
void       gl_destroy_context(GLContext *ctx);
bool       gl_resize_context(GLContext *ctx, int new_w, int new_h);
void       gl_make_current(GLContext *ctx);
GLContext *gl_get_current_context(void);
void       gl_swap_buffers(GLContext *ctx, void *win);

/* Internal Rasterizer and Pipeline Hooks */
void gl_matrix_init_stack(gl_matrix_stack_t *stack);
void gl_matrix_free_stack(gl_matrix_stack_t *stack);
void gl_matrix_push(gl_matrix_stack_t *stack);
void gl_matrix_pop(gl_matrix_stack_t *stack);
GLfloat *gl_matrix_current(gl_matrix_stack_t *stack);
void gl_matrix_mult(GLfloat *dst, const GLfloat *a, const GLfloat *b);

void gl_rasterize_triangle(GLContext *ctx, gl_vertex_t *v0, gl_vertex_t *v1, gl_vertex_t *v2);
void gl_rasterize_line(GLContext *ctx, gl_vertex_t *v0, gl_vertex_t *v1);
void gl_rasterize_point(GLContext *ctx, gl_vertex_t *v0);

void gl_clip_and_render_polygon(GLContext *ctx, gl_vertex_t *verts, int nverts);

/* Palette Dithering conversion for Mode 13h */
void gl_palette_init(void);
void gl_quantize_argb_to_palette(const uint32_t *src, uint8_t *dst, int w, int h);
void gl_quantize_argb_to_palette_strided(const uint32_t *src, int src_stride, uint8_t *dst, int dst_stride, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* LIB_IPO_GL_H */

