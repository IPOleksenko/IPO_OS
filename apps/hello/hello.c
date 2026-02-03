/*
 * hello.c - Simple "Hello World" application for IPO OS
 * 
 * This application demonstrates the IPOB executable format and
 * can be run from the terminal or autorun.
 */

#include <stdio.h>

/**
 * app_entry - Application entry point
 * 
 * Called by the process manager after loading the IPOB executable.
 * Runs in kernel context with access to all kernel APIs.
 */
void app_entry(void) {
    printf("Hello World!\n");
}
