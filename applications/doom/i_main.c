/**
 * i_main.c - Application entry point for DOOM on IPO_OS
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "doomdef.h"
#include "m_argv.h"
#include "d_main.h"

int main(int argc, char **argv) {
    /* Set up environment for DOOM */
    setenv("HOME", "/", 1);

    if (access("/applications/doom1.wad", 0) == 0) {
        setenv("DOOMWADDIR", "/applications", 1);
    } else if (access("/doom1.wad", 0) == 0) {
        setenv("DOOMWADDIR", "", 1);
    } else {
        setenv("DOOMWADDIR", ".", 1);
    }

    myargc = argc;
    myargv = argv;

    D_DoomMain();

    return 0;
}

