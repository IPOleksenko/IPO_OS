#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/ipo_gl.h>
#include <wm.h>
#include <syscall.h>
#include <system/state.h>
#include <system/timer.h>

#define WIN_W 140
#define WIN_H 100

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
    opt.title = "GL Stencil & Scissor";
    opt.x = 20;
    opt.y = 70;
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

    float angle = 0.0f;
    while (app_running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        uint32_t frame_start = timer_millis();

        gl_make_current(gl);

        /* Scissor test: clip center area */
        glEnable(GL_SCISSOR_TEST);
        glScissor(10, 10, WIN_W - 20, WIN_H - 20);

        glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(45.0, (double)WIN_W / (double)WIN_H, 1.0, 20.0);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -5.0f);
        glRotatef(angle, 0.0f, 1.0f, 0.0f);

        /* 1. Write Stencil Mask (rhombus) */
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

        glBegin(GL_TRIANGLE_FAN);
        glColor4f(0.3f, 0.3f, 0.3f, 1.0f);
        glVertex3f(0.0f,  1.2f, 0.0f);
        glVertex3f(-1.2f, 0.0f, 0.0f);
        glVertex3f(0.0f, -1.2f, 0.0f);
        glVertex3f( 1.2f, 0.0f, 0.0f);
        glEnd();

        /* 2. Draw rotating wireframe cube ONLY where Stencil == 1 */
        glStencilFunc(GL_EQUAL, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(2.0f);

        glPushMatrix();
        glRotatef(angle * 1.5f, 1.0f, 0.0f, 1.0f);
        glBegin(GL_QUADS);
        glColor4f(1.0f, 0.8f, 0.0f, 1.0f);
        glVertex3f(-0.8f, -0.8f,  0.8f);
        glVertex3f( 0.8f, -0.8f,  0.8f);
        glVertex3f( 0.8f,  0.8f,  0.8f);
        glVertex3f(-0.8f,  0.8f,  0.8f);

        glColor4f(0.0f, 1.0f, 0.8f, 1.0f);
        glVertex3f(-0.8f, -0.8f, -0.8f);
        glVertex3f(-0.8f,  0.8f, -0.8f);
        glVertex3f( 0.8f,  0.8f, -0.8f);
        glVertex3f( 0.8f, -0.8f, -0.8f);
        glEnd();
        glPopMatrix();

        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);

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

