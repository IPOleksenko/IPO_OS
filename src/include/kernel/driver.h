#ifndef IPO_DRIVER_H
#define IPO_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <types.h>

typedef struct driver {
    char *name;
    char *description;
    uint32_t flags;

    int (*init)(void);
    int (*cleanup)(void);

    void (*on_tick)(void);
    bool (*on_key)(uint8_t scancode, bool is_break);            /* return true to consume */
    int  (*on_command)(const char *cmd, int argc, char **argv); /* return 0 if handled */
    char (*on_char_output)(char c);                             /* transform stdout char */

    int (*read)(void *buffer, size_t size, ipo_uoff_t offset);
    int (*write)(const void *buffer, size_t size, ipo_uoff_t offset);
    int (*ioctl)(uint32_t cmd, void *arg);

    void *owner;                                                /* process_t* for user-space drivers */

    struct driver *next;
} driver_t;

#define DRIVER_FLAG_KERNEL          (1 << 0)
#define DRIVER_FLAG_USER            (1 << 1)
#define DRIVER_FLAG_ACTIVE          (1 << 2)
#define DRIVER_FLAG_CONTINUOUS_TICK (1 << 3)  /* invoke on_tick even with no keys held */

int driver_register(driver_t *drv);
int driver_unregister(const char *name);
int driver_unregister_force(const char *name);
void driver_unregister_by_owner(void *owner_proc);
driver_t* driver_find(const char *name);
driver_t* driver_get_list(void);
uint32_t driver_count_kernel(void);
uint32_t driver_count_user(void);
void driver_print_list(void);
int driver_set_description(const char *name, const char *new_desc);

void driver_clear_key_state(void);
void driver_dispatch_tick(void);
bool driver_dispatch_key(uint8_t scancode, bool is_break);
int  driver_dispatch_command(const char *cmd, int argc, char **argv);
char driver_dispatch_char_output(char c);

#endif /* IPO_DRIVER_H */
