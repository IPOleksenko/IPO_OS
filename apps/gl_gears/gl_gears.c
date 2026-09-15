#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/ipo_gl.h>
#include <GL/gl_math.h>
#include <wm.h>
#include <syscall.h>
#include <system/state.h>
#include <system/timer.h>

#define sinf gl_sinf
#define cosf gl_cosf

#define WIN_W 140
#define WIN_H 100

static void build_gear(float inner_radius, float outer_radius, float width, int teeth, float tooth_depth) {
    float r0 = inner_radius;
    float r1 = outer_radius - tooth_depth / 2.0f;
    float r2 = outer_radius + tooth_depth / 2.0f;
    float da = 2.0f * 3.14159265f / (float)teeth / 4.0f;

    glShadeModel(GL_FLAT);
    glNormal3f(0.0f, 0.0f, 1.0f);

    /* Draw front face */
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= teeth; i++) {
        float angle = i * 2.0f * 3.14159265f / (float)teeth;
        glVertex3f(r0 * cosf(angle), r0 * sinf(angle), width * 0.5f);
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), width * 0.5f);
        if (i < teeth) {
            glVertex3f(r0 * cosf(angle), r0 * sinf(angle), width * 0.5f);
            glVertex3f(r1 * cosf(angle + 3.0f * da), r1 * sinf(angle + 3.0f * da), width * 0.5f);
        }
    }
    glEnd();

    /* Draw front face of teeth */
    glBegin(GL_QUADS);
    for (int i = 0; i < teeth; i++) {
        float angle = i * 2.0f * 3.14159265f / (float)teeth;
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), width * 0.5f);
        glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da), width * 0.5f);
        glVertex3f(r2 * cosf(angle + 2.0f * da), r2 * sinf(angle + 2.0f * da), width * 0.5f);
        glVertex3f(r1 * cosf(angle + 3.0f * da), r1 * sinf(angle + 3.0f * da), width * 0.5f);
    }
    glEnd();

    /* Draw back face */
    glNormal3f(0.0f, 0.0f, -1.0f);
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= teeth; i++) {
        float angle = i * 2.0f * 3.14159265f / (float)teeth;
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), -width * 0.5f);
        glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -width * 0.5f);
        if (i < teeth) {
            glVertex3f(r1 * cosf(angle + 3.0f * da), r1 * sinf(angle + 3.0f * da), -width * 0.5f);
            glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -width * 0.5f);
        }
    }
    glEnd();
}

static volatile bool app_running = true;

static void on_window_event(wm_window_t *w, uint32_t event, uint32_t data) {
    (void)w; (void)data;
    if (event == WM_EVENT_CLOSE) {
        app_running = false;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    uint32_t pid = (uint32_t)ipo_syscall(IPO_SYSCALL_GETPID, 0, NULL);
    if (pid == 0) pid = 1;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "3D GL Gears";
    opt.x = 20 + ((pid * 24) % 120);
    opt.y = 15 + ((pid * 18) % 65);
    opt.w = WIN_W;
    opt.h = WIN_H;
    opt.decor_style = WM_DECOR_DEFAULT;
    opt.event_cb = on_window_event;

    wm_window_t *win = wm_create_window(&opt);
    if (!win) return 1;

    GLContext *gl = gl_create_context(WIN_W, WIN_H, win);
    if (!gl) {
        wm_destroy_window(win);
        return 1;
    }
    gl_make_current(gl);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);

    GLfloat pos[4] = { 5.0f, 5.0f, 10.0f, 0.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);

    /* Compile Gears into Display Lists */
    GLuint gear1 = glGenLists(1);
    glNewList(gear1, GL_COMPILE);
    GLfloat red[4] = { 0.8f, 0.1f, 0.0f, 1.0f };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, red);
    build_gear(1.0f, 4.0f, 1.0f, 10, 0.7f);
    glEndList();

    GLuint gear2 = glGenLists(1);
    glNewList(gear2, GL_COMPILE);
    GLfloat green[4] = { 0.0f, 0.8f, 0.2f, 1.0f };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, green);
    build_gear(0.5f, 2.0f, 2.0f, 10, 0.4f);
    glEndList();

    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    float angle = 0.0f;

    while (app_running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        uint32_t frame_start = timer_millis();

        gl_make_current(gl);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glFrustum(-1.0, 1.0, -1.0, 1.0, 5.0, 60.0);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -25.0f);
        glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
        glRotatef(30.0f, 0.0f, 1.0f, 0.0f);

        /* Gear 1 */
        glPushMatrix();
        glTranslatef(-3.0f, -2.0f, 0.0f);
        glRotatef(angle, 0.0f, 0.0f, 1.0f);
        build_gear(1.0f, 4.0f, 1.0f, 10, 0.7f);
        glPopMatrix();

        /* Gear 2 */
        glPushMatrix();
        glTranslatef(3.1f, -2.0f, 0.0f);
        glRotatef(-2.0f * angle - 9.0f, 0.0f, 0.0f, 1.0f);
        build_gear(0.5f, 2.0f, 2.0f, 10, 0.4f);
        glPopMatrix();

        gl_swap_buffers(gl, win);
        angle += 2.0f;

        /* Limit to 30 FPS */
        do {
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        } while (timer_elapsed_ms(frame_start) < 33);
    }

    gl_destroy_context(gl);
    if (wm_is_window_valid(win)) {
        wm_destroy_window(win);
    }
    return 0;
}
