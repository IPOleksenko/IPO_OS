#include <kernel/driver.h>
#include <kernel/process.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

static char *driver_strdup_local(const char *s) {
    size_t len = strlen(s) + 1u;
    char *p = (char *)kmalloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

static driver_t *driver_head = NULL;
static uint32_t s_continuous_tick_count = 0;

static bool driver_matches_name(const driver_t *curr, const char *name) {
    if (strcmp(curr->name, name) == 0) {
        return true;
    }
    if (curr->owner != NULL && process_is_valid((process_t *)curr->owner)) {
        process_t *p = (process_t *)curr->owner;
        if (p->name != NULL) {
            const char *base = strrchr(p->name, '/');
            base = base ? base + 1 : p->name;
            if (strcmp(base, name) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool driver_enter_owner_ctx(const driver_t *drv,
                                   process_t **out_prev_mapped,
                                   process_t **out_prev_current,
                                   bool *out_switched) {
    *out_switched = false;
    *out_prev_mapped = NULL;
    *out_prev_current = NULL;

    if (drv->owner != NULL) {
        process_t *owner = (process_t *)drv->owner;
        if (!process_is_valid(owner)) {
            return false;
        }
        *out_prev_mapped = process_get_mapped_app();
        *out_prev_current = process_get_current();
        process_map_app(owner);
        process_set_current(owner);
        *out_switched = true;
        return true;
    }

    return (drv->flags & DRIVER_FLAG_KERNEL) != 0u;
}

static void driver_leave_owner_ctx(process_t *prev_mapped,
                                   process_t *prev_current,
                                   bool switched) {
    if (!switched) return;
    if (prev_mapped != NULL && !process_is_valid(prev_mapped)) {
        prev_mapped = NULL;
    }
    if (prev_current != NULL && !process_is_valid(prev_current)) {
        prev_current = NULL;
    }
    process_map_app(prev_mapped);
    process_set_current(prev_current);
}

driver_t* driver_find(const char *name) {
    if (name == NULL) return NULL;
    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (driver_matches_name(curr, name)) {
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

    driver_t *existing = driver_find(drv->name);
    if (existing != NULL) {
        driver_unregister(drv->name);
    }

    driver_t *node = (driver_t *)kmalloc(sizeof(driver_t));
    if (node == NULL) {
        return -2;
    }
    memcpy(node, drv, sizeof(driver_t));

    node->name = driver_strdup_local(drv->name);
    if (node->name == NULL) {
        kfree(node);
        return -2;
    }

    if (drv->description != NULL) {
        node->description = driver_strdup_local(drv->description);
        if (node->description == NULL) {
            kfree(node->name);
            kfree(node);
            return -2;
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
    if (node->flags & DRIVER_FLAG_CONTINUOUS_TICK) {
        s_continuous_tick_count++;
    }

    serial_printf("[driver] registered '%s': %s\n", node->name, node->description ? node->description : "");
    return 0;
}

static void driver_destroy_node(driver_t *curr) {
    if (!curr) return;
    if ((curr->flags & DRIVER_FLAG_CONTINUOUS_TICK) && s_continuous_tick_count > 0) {
        s_continuous_tick_count--;
    }

    if (curr->cleanup != NULL) {
        if (curr->owner != NULL) {
            process_t *prev_mapped, *prev_current;
            bool switched = false;
            if (driver_enter_owner_ctx(curr, &prev_mapped, &prev_current, &switched)) {
                curr->cleanup();
                driver_leave_owner_ctx(prev_mapped, prev_current, switched);
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
}

static int driver_unregister_internal(const char *name, bool force) {
    if (name == NULL) return -1;

    driver_t *curr = driver_head;
    driver_t *prev = NULL;

    while (curr != NULL) {
        if (driver_matches_name(curr, name)) {
            if (!force && (curr->flags & DRIVER_FLAG_KERNEL)) {
                return -3;
            }
            if (prev != NULL) {
                prev->next = curr->next;
            } else {
                driver_head = curr->next;
            }
            driver_destroy_node(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    return -2;
}

int driver_unregister(const char *name) {
    return driver_unregister_internal(name, false);
}

int driver_unregister_force(const char *name) {
    return driver_unregister_internal(name, true);
}

void driver_unregister_by_owner(void *owner_proc) {
    if (!owner_proc) return;
    driver_t *curr = driver_head;
    driver_t *prev = NULL;
    while (curr != NULL) {
        driver_t *next = curr->next;
        if (curr->owner == owner_proc && !(curr->flags & DRIVER_FLAG_KERNEL)) {
            if (prev != NULL) {
                prev->next = next;
            } else {
                driver_head = next;
            }
            driver_destroy_node(curr);
        } else {
            prev = curr;
        }
        curr = next;
    }
}

driver_t* driver_get_list(void) {
    return driver_head;
}

uint32_t driver_count_kernel(void) {
    uint32_t count = 0;
    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (curr->flags & DRIVER_FLAG_KERNEL) count++;
        curr = curr->next;
    }
    return count;
}

uint32_t driver_count_user(void) {
    uint32_t count = 0;
    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (curr->flags & DRIVER_FLAG_USER) count++;
        curr = curr->next;
    }
    return count;
}

void driver_print_list(void) {
    if (driver_head == NULL) {
        printf("  No drivers currently loaded.\n");
        return;
    }

    printf("  NAME           TYPE     ORIGIN / STATUS       HOOKS   DESCRIPTION\n");
    printf("  -----------------------------------------------------------------------------------\n");

    driver_t *curr = driver_head;
    while (curr != NULL) {
        char *hooks = (char *)kmalloc(16u);
        if (hooks) {
            int hpos = 0;
            if (curr->on_tick) hooks[hpos++] = 'T';
            if (curr->on_key)  hooks[hpos++] = 'K';
            if (curr->on_command) hooks[hpos++] = 'C';
            if (curr->on_char_output) hooks[hpos++] = 'O';
            if (curr->read || curr->write) hooks[hpos++] = 'I';
            if (hpos == 0) hooks[hpos++] = '-';
            hooks[hpos] = '\0';
        }

        const char *type_str = (curr->flags & DRIVER_FLAG_KERNEL) ? "KERNEL" : "USER";
        char *origin_buf = NULL;
        if (curr->flags & DRIVER_FLAG_KERNEL) {
            origin_buf = driver_strdup_local("Built-in Core");
        } else if (curr->owner != NULL && process_is_valid((process_t *)curr->owner)) {
            process_t *p = (process_t *)curr->owner;
            const char *base = p->name ? strrchr(p->name, '/') : NULL;
            base = base ? base + 1 : (p->name ? p->name : "app");
            size_t ocap = strlen(base) + 24u;
            origin_buf = (char *)kmalloc(ocap);
            if (origin_buf) {
                snprintf(origin_buf, ocap, "PID %u (%s)", p->pid, base);
            }
        } else {
            origin_buf = driver_strdup_local("Resident Process");
        }

        printf("  ");
        print_padded(curr->name, 15);
        print_padded(type_str, 9);
        print_padded(origin_buf ? origin_buf : "", 22);
        print_padded(hooks ? hooks : "-", 8);
        printf("%s\n", curr->description ? curr->description : "");
        if (origin_buf) kfree(origin_buf);
        if (hooks) kfree(hooks);
        curr = curr->next;
    }
    printf("\n  Hooks: T=Timer, K=Key, C=Command, O=CharOut, I=IO\n");
}

static bool *s_active_keys = NULL;
static uint32_t s_active_keys_cap = 0;
static uint32_t s_keys_down_count = 0;
static bool s_driver_e0_prefix = false;
static uint32_t s_last_driver_tick_ms = 0;

void driver_clear_key_state(void) {
    if (s_active_keys && s_active_keys_cap > 0u) {
        memset(s_active_keys, 0, (size_t)s_active_keys_cap * sizeof(bool));
    }
    s_keys_down_count = 0u;
    s_driver_e0_prefix = false;
}

void driver_dispatch_tick(void) {
    if (s_keys_down_count == 0 && s_continuous_tick_count == 0) {
        return;
    }

    uint32_t now = timer_millis();
    if (now - s_last_driver_tick_ms < 50u) {
        return;
    }
    s_last_driver_tick_ms = now;

    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (curr->on_tick != NULL &&
            (s_keys_down_count > 0 || (curr->flags & DRIVER_FLAG_CONTINUOUS_TICK))) {
            process_t *prev_mapped, *prev_current;
            bool switched = false;
            if (driver_enter_owner_ctx(curr, &prev_mapped, &prev_current, &switched)) {
                curr->on_tick();
                driver_leave_owner_ctx(prev_mapped, prev_current, switched);
            }
        }
        curr = curr->next;
    }
}

bool driver_dispatch_key(uint8_t scancode, bool is_break) {
    if (scancode == 0xE0u) {
        s_driver_e0_prefix = true;
        return false;
    }
    bool is_ext = s_driver_e0_prefix;
    s_driver_e0_prefix = false;

    uint16_t key_idx = (uint16_t)(scancode & 0x7Fu) | (is_ext ? 0x80u : 0x00u);
    if (key_idx >= s_active_keys_cap) {
        uint32_t ncap = s_active_keys_cap ? s_active_keys_cap : 128u;
        while (ncap <= key_idx) ncap *= 2u;
        bool *nbuf = (bool *)krealloc(s_active_keys, (size_t)ncap * sizeof(bool));
        if (nbuf) {
            if (ncap > s_active_keys_cap) {
                memset(&nbuf[s_active_keys_cap], 0, (size_t)(ncap - s_active_keys_cap) * sizeof(bool));
            }
            s_active_keys = nbuf;
            s_active_keys_cap = ncap;
        }
    }
    if (s_active_keys && key_idx < s_active_keys_cap) {
        if (!is_break) {
            if (!s_active_keys[key_idx]) {
                s_active_keys[key_idx] = true;
                s_keys_down_count++;
            }
        } else {
            if (s_active_keys[key_idx]) {
                s_active_keys[key_idx] = false;
                if (s_keys_down_count > 0) s_keys_down_count--;
            }
        }
    }

    driver_t *curr = driver_head;
    while (curr != NULL) {
        if (curr->on_key != NULL) {
            process_t *prev_mapped, *prev_current;
            bool switched = false;
            if (driver_enter_owner_ctx(curr, &prev_mapped, &prev_current, &switched)) {
                bool consumed = curr->on_key(scancode, is_break);
                driver_leave_owner_ctx(prev_mapped, prev_current, switched);
                if (consumed) return true;
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
            process_t *prev_mapped, *prev_current;
            bool switched = false;
            if (driver_enter_owner_ctx(curr, &prev_mapped, &prev_current, &switched)) {
                int res = curr->on_command(cmd, argc, argv);
                driver_leave_owner_ctx(prev_mapped, prev_current, switched);
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
            process_t *prev_mapped, *prev_current;
            bool switched = false;
            if (driver_enter_owner_ctx(curr, &prev_mapped, &prev_current, &switched)) {
                res = curr->on_char_output(res);
                driver_leave_owner_ctx(prev_mapped, prev_current, switched);
            }
        }
        curr = curr->next;
    }
    return res;
}

int driver_set_description(const char *name, const char *new_desc) {
    if (name == NULL || new_desc == NULL) return -1;
    driver_t *drv = driver_find(name);
    if (drv == NULL) return -2;

    char *copy = driver_strdup_local(new_desc);
    if (copy == NULL) return -3;

    if (drv->description != NULL) {
        kfree(drv->description);
    }
    drv->description = copy;
    return 0;
}
