/**
 * i_net.c - Single-player stub network driver for DOOM on IPO_OS
 */

#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "d_net.h"
#include "doomstat.h"
#include "i_net.h"

void I_InitNetwork(void) {
    doomcom = (doomcom_t *)malloc(sizeof(*doomcom));
    if (doomcom) {
        memset(doomcom, 0, sizeof(*doomcom));
        doomcom->id = DOOMCOM_ID;
        doomcom->numplayers = 1;
        doomcom->numnodes = 1;
        doomcom->deathmatch = false;
        doomcom->consoleplayer = 0;
        doomcom->ticdup = 1;
        doomcom->extratics = 0;
    }
    netgame = false;
}

void I_NetCmd(void) {
}
