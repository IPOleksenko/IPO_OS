#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "py/mpconfig.h"
#include "py/misc.h"
#include "py/mphal.h"

#define CHAR_CTRL_C (3)
#define CHAR_CTRL_D (4)

int readline(vstr_t *line, const char *prompt) {
    if (prompt) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    vstr_reset(line);
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) {
        return CHAR_CTRL_D;
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    vstr_add_strn(line, buf, len);
    return 0;
}

void mp_hal_stdio_mode_raw(void) {}
void mp_hal_stdio_mode_orig(void) {}
