#ifndef IPO_DRIVER_H
#define IPO_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DRIVER_NAME_MAX 64
#define DRIVER_DESC_MAX 128

/**
 * Driver structure for dynamically loading drivers and kernel extensions.
 */
typedef struct driver {
    char name[DRIVER_NAME_MAX];
    char description[DRIVER_DESC_MAX];
    uint32_t flags;

    /* Lifecycle callbacks */
    int (*init)(void);
    int (*cleanup)(void);

    /* Event hooks */
    void (*on_tick)(void);                                       /* called every timer tick */
    bool (*on_key)(uint8_t scancode, bool is_break);            /* called on key event; return true to consume */
    int  (*on_command)(const char *cmd, int argc, char **argv); /* custom shell command; return 0 if handled */
    char (*on_char_output)(char c);                             /* intercept / transform stdout chars */

    /* Custom device I/O */
    int (*read)(void *buffer, size_t size, uint32_t offset);
    int (*write)(const void *buffer, size_t size, uint32_t offset);
    int (*ioctl)(uint32_t cmd, void *arg);

    struct driver *next;
} driver_t;

#define DRIVER_FLAG_KERNEL   (1 << 0)  /* Built-in kernel hardware driver */
#define DRIVER_FLAG_USER     (1 << 1)  /* Dynamically loaded user-space driver */
#define DRIVER_FLAG_ACTIVE   (1 << 2)  /* Driver is initialized and active */

/* Driver registration functions */
int driver_register(driver_t *drv);
int driver_unregister(const char *name);
driver_t* driver_find(const char *name);
driver_t* driver_get_list(void);
uint32_t driver_count(void);
void driver_print_list(void);

/* Kernel dispatchers */
void driver_dispatch_tick(void);
bool driver_dispatch_key(uint8_t scancode, bool is_break);
int  driver_dispatch_command(const char *cmd, int argc, char **argv);
char driver_dispatch_char_output(char c);

#endif /* IPO_DRIVER_H */
