#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/ipo_gl.h>
#include <wm.h>
#include <syscall.h>
#include <system/timer.h>
#include <system/state.h>
#include <driver/input/keyboard.h>
#include <stdio.h>

#define WIN_W 140
#define WIN_H 100

static uint32_t tex_data[16 * 16];

static void generate_checker_texture(void) {
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            bool check = ((x ^ y) & 2) != 0;
            if (check) {
                tex_data[y * 16 + x] = 0xFF00E0FFu; /* Bright Cyan/Azure */
            } else {
                tex_data[y * 16 + x] = 0xFFFF8000u; /* Orange */
            }
        }
    }
}

static void draw_cube(void) {
    glBegin(GL_QUADS);

    /* Front Face (Z+) */
    glNormal3f(0.0f, 0.0f, 1.0f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-1.0f, -1.0f,  1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f( 1.0f, -1.0f,  1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f( 1.0f,  1.0f,  1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-1.0f,  1.0f,  1.0f);

    /* Back Face (Z-) */
    glNormal3f(0.0f, 0.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f(-1.0f, -1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f(-1.0f,  1.0f, -1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f( 1.0f,  1.0f, -1.0f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f( 1.0f, -1.0f, -1.0f);

    /* Top Face (Y+) */
    glNormal3f(0.0f, 1.0f, 0.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-1.0f,  1.0f, -1.0f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-1.0f,  1.0f,  1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f( 1.0f,  1.0f,  1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f( 1.0f,  1.0f, -1.0f);

    /* Bottom Face (Y-) */
    glNormal3f(0.0f, -1.0f, 0.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f(-1.0f, -1.0f, -1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f( 1.0f, -1.0f, -1.0f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f( 1.0f, -1.0f,  1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f(-1.0f, -1.0f,  1.0f);

    /* Right face (X+) */
    glNormal3f(1.0f, 0.0f, 0.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f( 1.0f, -1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f( 1.0f,  1.0f, -1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f( 1.0f,  1.0f,  1.0f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f( 1.0f, -1.0f,  1.0f);

    /* Left Face (X-) */
    glNormal3f(-1.0f, 0.0f, 0.0f);
    glTexCoord2f(0.0f, 0.0f); glVertex3f(-1.0f, -1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex3f(-1.0f, -1.0f,  1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex3f(-1.0f,  1.0f,  1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex3f(-1.0f,  1.0f, -1.0f);

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

    /* Auto-offset multiple instances so they don't overlap completely */
    static int inst_count = 0;
    int win_x = 10 + (inst_count % 3) * 60;
    int win_y = 15 + (inst_count % 2) * 30;
    inst_count++;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "3D GL Cube";
    opt.x = (int16_t)win_x;
    opt.y = (int16_t)win_y;
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

    generate_checker_texture();

    GLuint tex_id = 1;
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex_data);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);

    GLfloat light_pos[4] = { 2.0f, 4.0f, 3.0f, 1.0f };
    GLfloat light_diff[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diff);

    glClearColor(0.05f, 0.05f, 0.12f, 1.0f);

    float angle_x = 0.0f;
    float angle_y = 0.0f;

    /* Render loop */
    while (app_running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        uint32_t frame_start = timer_millis();

        gl_make_current(gl);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(45.0, (double)WIN_W / (double)WIN_H, 1.0, 20.0);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -4.5f);
        glRotatef(angle_x, 1.0f, 0.0f, 0.0f);
        glRotatef(angle_y, 0.0f, 1.0f, 0.0f);

        draw_cube();

        gl_swap_buffers(gl, win);

        angle_x += 2.0f;
        angle_y += 3.5f;

        /* Limit to 30 FPS to prevent CPU overload */
        do {
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        } while (timer_elapsed_ms(frame_start) < 33);
    }

    gl_destroy_context(gl);
    wm_destroy_window(win);
    return 0;
}

