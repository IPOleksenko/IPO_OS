#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/ipo_gl.h>
#include <GL/gl_math.h>
#include <wm.h>
#include <syscall.h>
#include <system/state.h>
#include <system/timer.h>
#include <stdio.h>

#define WIN_W 140
#define WIN_H 100

static void draw_blended_quads(float angle) {
    /* Background opaque white reference post */
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBegin(GL_QUADS);
    glColor3f(0.8f, 0.8f, 0.8f);
    glVertex3f(-0.1f, -1.5f, -0.5f);
    glVertex3f( 0.1f, -1.5f, -0.5f);
    glVertex3f( 0.1f,  1.5f, -0.5f);
    glVertex3f(-0.1f,  1.5f, -0.5f);
    glEnd();

    /* Translucent rotating planes */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE); /* Disable Z-write for transparent primitives */

    /* Red translucent plane */
    glPushMatrix();
    glRotatef(angle, 0.0f, 1.0f, 0.0f);
    glBegin(GL_QUADS);
    glColor4f(1.0f, 0.2f, 0.2f, 0.6f);
    glVertex3f(-1.2f, -1.0f, 0.0f);
    glVertex3f( 1.2f, -1.0f, 0.0f);
    glVertex3f( 1.2f,  1.0f, 0.0f);
    glVertex3f(-1.2f,  1.0f, 0.0f);
    glEnd();
    glPopMatrix();

    /* Blue translucent plane intersecting at 90 deg */
    glPushMatrix();
    glRotatef(angle + 90.0f, 0.0f, 1.0f, 0.0f);
    glBegin(GL_QUADS);
    glColor4f(0.2f, 0.4f, 1.0f, 0.6f);
    glVertex3f(-1.2f, -1.0f, 0.0f);
    glVertex3f( 1.2f, -1.0f, 0.0f);
    glVertex3f( 1.2f,  1.0f, 0.0f);
    glVertex3f(-1.2f,  1.0f, 0.0f);
    glEnd();
    glPopMatrix();

    /* Green translucent plane intersecting at 45 deg */
    glPushMatrix();
    glRotatef(angle + 45.0f, 1.0f, 0.0f, 0.0f);
    glBegin(GL_QUADS);
    glColor4f(0.2f, 1.0f, 0.3f, 0.5f);
    glVertex3f(-1.0f, 0.0f, -1.2f);
    glVertex3f( 1.0f, 0.0f, -1.2f);
    glVertex3f( 1.0f, 0.0f,  1.2f);
    glVertex3f(-1.0f, 0.0f,  1.2f);
    glEnd();
    glPopMatrix();

    glDepthMask(GL_TRUE);
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

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "GL Blending";
    opt.x = 40;
    opt.y = 30;
    opt.w = WIN_W;
    opt.h = WIN_H;
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
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.05f);
    glBlendEquation(GL_FUNC_ADD);

    glClearColor(0.08f, 0.08f, 0.12f, 1.0f);

    float angle = 0.0f;

    while (app_running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        uint32_t frame_start = timer_millis();

        gl_make_current(gl);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(45.0, (double)WIN_W / (double)WIN_H, 0.5, 20.0);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -4.0f);
        glRotatef(20.0f, 1.0f, 0.0f, 0.0f);

        draw_blended_quads(angle);

        gl_swap_buffers(gl, win);

        angle += 2.0f;

        /* Limit to 30 FPS */
        do {
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        } while (timer_elapsed_ms(frame_start) < 33);
    }

    gl_destroy_context(gl);
    wm_destroy_window(win);
    return 0;
}

