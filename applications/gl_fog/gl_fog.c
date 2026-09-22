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

static void draw_scene(void) {
    /* Draw a floor grid extending into the distance */
    glBegin(GL_LINES);
    glColor3f(0.3f, 0.7f, 0.4f);
    for (float x = -6.0f; x <= 6.0f; x += 1.0f) {
        glVertex3f(x, -1.0f, -1.0f);
        glVertex3f(x, -1.0f, -15.0f);
    }
    for (float z = -1.0f; z >= -15.0f; z -= 1.0f) {
        glVertex3f(-6.0f, -1.0f, z);
        glVertex3f( 6.0f, -1.0f, z);
    }
    glEnd();

    /* Draw columns/pillars placed at varying depths */
    static const float depths[] = { -3.0f, -5.0f, -7.0f, -9.0f, -12.0f };
    for (int i = 0; i < 5; i++) {
        float z = depths[i];
        float x = (i % 2 == 0) ? -1.5f : 1.5f;

        glBegin(GL_QUADS);
        glColor3f(0.9f, 0.3f, 0.2f);
        glVertex3f(x - 0.4f, -1.0f, z);
        glVertex3f(x + 0.4f, -1.0f, z);
        glVertex3f(x + 0.4f,  1.2f, z);
        glVertex3f(x - 0.4f,  1.2f, z);
        glEnd();
    }
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
    opt.title = "GL Fog & Pixels";
    opt.x = 20;
    opt.y = 20;
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
    glEnable(GL_FOG);

    GLfloat fog_col[4] = { 0.2f, 0.25f, 0.35f, 1.0f };
    glFogfv(GL_FOG_COLOR, fog_col);
    glFogi(GL_FOG_MODE, GL_EXP2);
    glFogf(GL_FOG_DENSITY, 0.18f);
    glFogf(GL_FOG_START, 2.0f);
    glFogf(GL_FOG_END, 14.0f);

    glClearColor(0.2f, 0.25f, 0.35f, 1.0f);

    float cam_offset = 0.0f;
    uint32_t verified_pixel = 0;
    int frame = 0;

    while (app_running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        uint32_t frame_start = timer_millis();
        frame++;

        gl_make_current(gl);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(50.0, (double)WIN_W / (double)WIN_H, 0.5, 25.0);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, -0.2f, cam_offset);

        draw_scene();

        /* Verify glReadPixels */
        if (frame == 10) {
            glReadPixels(WIN_W / 2, WIN_H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &verified_pixel);
        }

        gl_swap_buffers(gl, win);

        cam_offset -= 0.015f;
        if (cam_offset < -6.0f) cam_offset = 0.0f;

        /* Limit to 30 FPS */
        do {
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        } while (timer_elapsed_ms(frame_start) < 33);
    }

    gl_destroy_context(gl);
    wm_destroy_window(win);
    return 0;
}

