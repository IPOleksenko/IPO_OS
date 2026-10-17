/**
 * hello.c - Simple "Hello World" application for IPO_OS
 */

#include <stdio.h>
#include <stddef.h>

/**
 * String - Custom string type for IPO_OS applications
 *
 * Stores a pointer to character data and the string length.
 *
 * @data: Pointer to the string character data
 * @length: Number of characters in the string, excluding the null terminator
 */
typedef struct {
    char *data;
    size_t length;
} String;

/**
 * main - Application entry point
 *
 * Called by the process manager after loading the IPOB executable.
 * Runs in kernel context with access to all kernel APIs.
 *
 * @argc: Number of command-line arguments
 * @argv: Array of command-line argument strings
 *
 * Returns: A String containing the application result
 */
String main(int argc, char **argv) {
    if (argc == 1) {
        printf("Hello, World!\n");
    } else {
        for (int i = 1; i < argc; i++) {
            printf("Hello, %s\n", argv[i]);
        }
    }

    return (String) {
        .data = "hello, IPO_OS!",
        .length = sizeof("hello, IPO_OS!") - 1
    };
}