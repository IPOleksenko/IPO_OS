#include <kernel/driver.h>
#include <kernel/process.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

static driver_t *driver_head = NULL;

driver_t* driver_find(const char *name) {
    if (name == NULL) return NULL;
    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (strcmp(curr->name, name) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

int driver_register(driver_t *drv) {
    if (drv == NULL || drv->name == NULL || drv->name[0] == '\0') {
        return -1;
    }

    /* If already exists, unregister old one first */
    driver_t *existing = driver_find(drv->name);
    if (existing != NULL) {
        driver_unregister(drv->name);
    }

    driver_t *node = (driver_t *)kmalloc(sizeof(driver_t));
    if (node == NULL) {
        return -2;
    }
    memcpy(node, drv, sizeof(driver_t));

    node->name = (char *)kmalloc(strlen(drv->name) + 1u);
    if (node->name == NULL) {
        kfree(node);
        return -2;
    }
    strcpy(node->name, drv->name);

    if (drv->description != NULL) {
        node->description = (char *)kmalloc(strlen(drv->description) + 1u);
        if (node->description != NULL) {
            strcpy(node->description, drv->description);
        }
    } else {
        node->description = NULL;
    }

    process_t *owner = process_get_current();
    if ((node->flags & DRIVER_FLAG_USER) && owner != NULL) {
        process_set_keep_alive(owner, 1);
        node->owner = owner;
    } else {
        node->owner = NULL;
    }

    if (node->init != NULL) {
        int init_res = node->init();
        if (init_res < 0) {
            if (node->name) kfree(node->name);
            if (node->description) kfree(node->description);
            if (node->owner) process_set_keep_alive((process_t *)node->owner, 0);
            kfree(node);
            return init_res;
        }
    }

    node->next = driver_head;
    driver_head = node;

    serial_printf("[driver] registered '%s': %s\n", node->name, node->description ? node->description : "");
    return 0;
}

int driver_unregister(const char *name) {
    if (name == NULL) return -1;

    driver_t *curr = driver_head;
    driver_t *prev = NULL;

    while (curr != NULL) {
        if (strcmp(curr->name, name) == 0) {
            if (curr->flags & DRIVER_FLAG_KERNEL) {
                return -3; /* Cannot unregister built-in kernel driver */
            }
            if (prev != NULL) {
                prev->next = curr->next;
            } else {
                driver_head = curr->next;
            }

            if (curr->cleanup != NULL) {
                if (curr->owner != NULL && process_is_valid((process_t *)curr->owner)) {
                    process_t *prev_mapped = process_get_mapped_app();
                    process_t *prev_current = process_get_current();
                    process_map_app((process_t *)curr->owner);
                    process_set_current((process_t *)curr->owner);
                    curr->cleanup();
                    if (prev_mapped != NULL && prev_mapped != (process_t *)curr->owner && process_is_valid(prev_mapped)) {
                        process_map_app(prev_mapped);
                        process_set_current(prev_current);
                    }
                } else {
                    curr->cleanup();
                }
            }

            serial_printf("[driver] unregistered '%s'\n", curr->name);
            if (curr->owner != NULL) {
                process_set_keep_alive((process_t *)curr->owner, 0);
                curr->owner = NULL;
            }
            if (curr->name != NULL) {
                kfree(curr->name);
            }
            if (curr->description != NULL) {
                kfree(curr->description);
            }
            kfree(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    return -2; // Not found
}

driver_t* driver_get_list(void) {
    return driver_head;
}

uint32_t driver_count(void) {
    uint32_t count = 0;
    driver_t *curr = driver_head;
    while (curr != NULL) {
        count++;
        curr = curr->next;
    }
    return count;
}

static void print_padded(const char *s, int width) {
    int len = 0;
    if (s) {
        while (s[len]) {
            putchar(s[len]);
            len++;
        }
    }
    while (len < width) {
        putchar(' ');
        len++;
    }
}

void driver_print_list(void) {
    if (driver_head == NULL) {
        printf("No drivers loaded.\n");
        return;
    }

    printf("  NAME           TYPE     HOOKS   DESCRIPTION\n");
    printf("  ---------------------------------------------------------------------------\n");

    driver_t *curr = driver_head;
    while (curr != NULL) {
        char hooks[32];
        int hpos = 0;
        if (curr->on_tick) hooks[hpos++] = 'T';
        if (curr->on_key)  hooks[hpos++] = 'K';
        if (curr->on_command) hooks[hpos++] = 'C';
        if (curr->on_char_output) hooks[hpos++] = 'O';
        if (curr->read || curr->write) hooks[hpos++] = 'I';
        if (hpos == 0) hooks[hpos++] = '-';
        hooks[hpos] = '\0';

        const char *type_str = (curr->flags & DRIVER_FLAG_KERNEL) ? "KERNEL" : "USER";
        printf("  ");
        print_padded(curr->name, 15);
        print_padded(type_str, 9);
        print_padded(hooks, 8);
        printf("%s\n", curr->description);
        curr = curr->next;
    }
    printf("\n  Hooks: T=Timer, K=Key, C=Command, O=CharOut, I=IO\n");
}

/* System-wide active keys tracking to completely eliminate CPU overhead during idle/normal typing */
static bool s_active_keys[128] = {false};
static uint32_t s_keys_down_count = 0;
static uint32_t s_last_driver_tick_ms = 0;

void driver_dispatch_tick(void) {
    /* If no keys are currently pressed anywhere on the system,
     * no key-hold drivers need ticks. Avoid expensive process mapping! */
    if (s_keys_down_count == 0) {
        return;
    }

    uint32_t now = timer_millis();
    /* Rate-limit tick processing to 20Hz (every 50ms) to ensure 0% CPU starvation */
    if (now - s_last_driver_tick_ms < 50u) {
        return;
    }
    s_last_driver_tick_ms = now;

    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (curr->on_tick != NULL) {
            process_t *owner = (process_t *)curr->owner;
            if (owner != NULL) {
                if (!process_is_valid(owner)) {
                    curr = curr->next;
                    continue;
                }
                process_t *prev_mapped = process_get_mapped_app();
                process_t *prev_current = process_get_current();

                process_map_app(owner);
                process_set_current(owner);

                curr->on_tick();

                if (prev_mapped != NULL && prev_mapped != owner && process_is_valid(prev_mapped)) {
                    process_map_app(prev_mapped);
                    process_set_current(prev_current);
                }
            } else if (curr->flags & DRIVER_FLAG_KERNEL) {
                curr->on_tick();
            }
        }
        curr = curr->next;
    }
}

bool driver_dispatch_key(uint8_t scancode, bool is_break) {
    uint8_t clean_sc = scancode & 0x7F;
    if (clean_sc < 128) {
        if (!is_break) {
            if (!s_active_keys[clean_sc]) {
                s_active_keys[clean_sc] = true;
                s_keys_down_count++;
            }
        } else {
            if (s_active_keys[clean_sc]) {
                s_active_keys[clean_sc] = false;
                if (s_keys_down_count > 0) s_keys_down_count--;
            }
        }
    }

    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (curr->on_key != NULL) {
            process_t *owner = (process_t *)curr->owner;
            if (owner != NULL) {
                if (!process_is_valid(owner)) {
                    curr = curr->next;
                    continue;
                }
                process_t *prev_mapped = process_get_mapped_app();
                process_t *prev_current = process_get_current();

                process_map_app(owner);
                process_set_current(owner);

                bool consumed = curr->on_key(scancode, is_break);

                if (prev_mapped != NULL && prev_mapped != owner && process_is_valid(prev_mapped)) {
                    process_map_app(prev_mapped);
                    process_set_current(prev_current);
                }

                if (consumed) return true;
            } else if (curr->flags & DRIVER_FLAG_KERNEL) {
                if (curr->on_key(scancode, is_break)) {
                    return true;
                }
            }
        }
        curr = curr->next;
    }
    return false;
}

int driver_dispatch_command(const char *cmd, int argc, char **argv) {
    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (curr->on_command != NULL) {
            process_t *owner = (process_t *)curr->owner;
            if (owner != NULL) {
                if (!process_is_valid(owner)) {
                    curr = curr->next;
                    continue;
                }
                process_t *prev_mapped = process_get_mapped_app();
                process_t *prev_current = process_get_current();

                process_map_app(owner);
                process_set_current(owner);

                int res = curr->on_command(cmd, argc, argv);

                if (prev_mapped != NULL && prev_mapped != owner && process_is_valid(prev_mapped)) {
                    process_map_app(prev_mapped);
                    process_set_current(prev_current);
                }

                if (res == 0) return 0;
            } else if (curr->flags & DRIVER_FLAG_KERNEL) {
                int res = curr->on_command(cmd, argc, argv);
                if (res == 0) return 0;
            }
        }
        curr = curr->next;
    }
    return -1;
}

char driver_dispatch_char_output(char c) {
    driver_t *curr = driver_head;
    char res = c;
    while (curr != NULL) {
        if (curr->on_char_output != NULL) {
            process_t *owner = (process_t *)curr->owner;
            if (owner != NULL) {
                if (!process_is_valid(owner)) {
                    curr = curr->next;
                    continue;
                }
                process_t *prev_mapped = process_get_mapped_app();
                process_t *prev_current = process_get_current();

                process_map_app(owner);
                process_set_current(owner);

                res = curr->on_char_output(res);

                if (prev_mapped != NULL && prev_mapped != owner && process_is_valid(prev_mapped)) {
                    process_map_app(prev_mapped);
                    process_set_current(prev_current);
                }
            } else if (curr->flags & DRIVER_FLAG_KERNEL) {
                res = curr->on_char_output(res);
            }
        }
        curr = curr->next;
    }
    return res;
}

void driver_init_system_drivers(void) {
    static const struct {
        const char *name;
        const char *desc;
    } sys_drvs[] = {
        {"pit",      "8254 Programmable Interval Timer"},
        {"keyboard", "PS/2 Keyboard Controller"},
        {"mouse",    "PS/2 Mouse Device Driver"},
        {"vga",      "VGA Text/Graphics & VBE Display Driver"},
        {"ata",      "IDE/ATA Hard Disk PIO Driver"},
        {"sound",    "PC Speaker & Audio Core Subsystem"},
        {"pci",      "PCI Bus Controller & Device Enumerator"},
        {"rtl8139",  "Realtek RTL8139 Fast Ethernet NIC"},
    };

    for (size_t i = 0; i < sizeof(sys_drvs) / sizeof(sys_drvs[0]); i++) {
        driver_t drv;
        memset(&drv, 0, sizeof(drv));
        drv.name = (char *)sys_drvs[i].name;
        drv.description = (char *)sys_drvs[i].desc;
        drv.flags = DRIVER_FLAG_KERNEL | DRIVER_FLAG_ACTIVE;
        driver_register(&drv);
    }
}

int driver_set_description(const char *name, const char *new_desc) {
    if (name == NULL || new_desc == NULL) return -1;
    driver_t *drv = driver_find(name);
    if (drv == NULL) return -2;

    char *copy = (char *)kmalloc(strlen(new_desc) + 1u);
    if (copy == NULL) return -3;
    strcpy(copy, new_desc);

    if (drv->description != NULL) {
        kfree(drv->description);
    }
    drv->description = copy;
    return 0;
}
