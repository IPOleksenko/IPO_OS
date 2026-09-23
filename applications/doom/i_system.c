/**
 * i_system.c - Platform system interface for DOOM on IPO_OS
 *
 * Implements timer, memory allocation, error handling and process exit.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"

#include <system/timer.h>
#include <syscall.h>

extern void serial_printf(const char *fmt, ...);
extern boolean demorecording;

static ticcmd_t s_empty_ticcmd;

void I_Tactile(int on, int off, int total) {
    (void)on; (void)off; (void)total;
}

ticcmd_t *I_BaseTiccmd(void) {
    memset(&s_empty_ticcmd, 0, sizeof(s_empty_ticcmd));
    return &s_empty_ticcmd;
}

int I_GetHeapSize(void) {
    return 8 * 1024 * 1024;
}

byte *I_ZoneBase(int *size) {
    *size = 8 * 1024 * 1024;
    return (byte *)malloc(*size);
}

byte *I_AllocLow(int length) {
    byte *mem = (byte *)malloc(length);
    if (mem) {
        memset(mem, 0, length);
    }
    return mem;
}

int I_GetTime(void) {
    static uint32_t basetime = 0;
    uint32_t t = timer_millis();
    if (!basetime) {
        basetime = t;
    }
    return (int)(((uint64_t)(t - basetime) * 35) / 1000);
}

void I_Init(void) {
    I_InitSound();
}

void I_Quit(void) {
    D_QuitNetGame();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults();
    I_ShutdownGraphics();
    exit(0);
}

void I_Error(char *error, ...) {
    va_list argptr;

    I_ShutdownGraphics();

    va_start(argptr, error);
    printf("\n[DOOM ERROR] ");
    vprintf(error, argptr);
    printf("\n");
    va_end(argptr);

    if (demorecording) {
        G_CheckDemoStatus();
    }

    D_QuitNetGame();
    exit(-1);
}

