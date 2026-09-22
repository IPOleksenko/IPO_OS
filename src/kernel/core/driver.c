#include <kernel/driver.h>
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

    if (node->init != NULL) {
        int init_res = node->init();
        if (init_res < 0) {
            if (node->name) kfree(node->name);
            if (node->description) kfree(node->description);
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
                curr->cleanup();
            }

            serial_printf("[driver] unregistered '%s'\n", curr->name);
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

void driver_dispatch_tick(void) {
    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (curr->on_tick != NULL) {
            curr->on_tick();
        }
        curr = curr->next;
    }
}

bool driver_dispatch_key(uint8_t scancode, bool is_break) {
    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (curr->on_key != NULL) {
            if (curr->on_key(scancode, is_break)) {
                return true; // Consumed by driver
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
            int res = curr->on_command(cmd, argc, argv);
            if (res == 0) {
                return 0; // Handled by driver
            }
        }
        curr = curr->next;
    }
    return -1; // Not handled
}

char driver_dispatch_char_output(char c) {
    driver_t *curr = driver_head;
    char res = c;
    while (curr != NULL) {
        if (curr->on_char_output != NULL) {
            res = curr->on_char_output(res);
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

