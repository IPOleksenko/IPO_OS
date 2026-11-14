#include <kernel/input_ext.h>
#include <kernel/driver.h>
#include <kernel/device.h>
#include <kernel/irq.h>
#include <kernel/process.h>
#include <kernel/smp.h>
#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <driver/input/keymap/keymap.h>
#include <driver/net/pci.h>
#include <memory/address_space.h>
#include <memory/memory_map.h>
#include <memory/kmalloc.h>
#include <system/timer.h>
#include <wm.h>
#include <ioport.h>
#include <string.h>
#include <stddef.h>

#define KBD_CMD_PORT     0x64u
#define KBD_STATUS_OBF   0x01u
#define KBD_STATUS_IBF   0x02u

#define HW_MMIO_VBASE_START 0xF0000000u
#define HW_MMIO_VBASE_END   0xFC000000u
#define HW_PAGE_SIZE        4096u

enum {
    DEC_IDLE = 0,
    DEC_E0 = 1,
    DEC_E1_B1 = 2,
    DEC_E1_B2 = 3
};

static uint32_t *s_pressed_keys = NULL;
static uint32_t s_pressed_count = 0u;
static uint32_t s_pressed_cap = 0u;
static uint32_t s_ext_modifiers = 0u;

/* Per-PID Extended Key Queue (for processes that subscribe/poll) */
typedef struct {
    bool active;
    uint32_t pid;
    ipo_key_event_t *ring;
    uint32_t cap;
    uint32_t head;
    uint32_t tail;
} key_sub_queue_t;

static key_sub_queue_t *s_key_subs = NULL;
static uint32_t s_key_sub_cap = 0u;

typedef struct {
    bool active;
    bool armed;
    bool triggered;
    uint32_t id;
    uint32_t owner_pid;
    uint32_t window_handle;
    uint32_t mod_mask;
    uint32_t key_count;
    uint32_t *keys;
    uint32_t flags;
    uint32_t user_tag;
} shortcut_entry_t;

typedef struct {
    bool active;
    uint32_t pid;
    ipo_shortcut_event_t *ring;
    uint32_t cap;
    uint32_t head;
    uint32_t tail;
} shortcut_queue_t;

static shortcut_entry_t *s_shortcuts = NULL;
static uint32_t s_shortcut_cap = 0u;
static uint32_t s_next_shortcut_id = 1u;

static shortcut_queue_t *s_shortcut_queues = NULL;
static uint32_t s_shortcut_queue_cap = 0u;

typedef struct {
    uint16_t type;
    uint32_t code;
    int32_t value;
} input_dev_axis_t;

typedef struct {
    bool active;
    uint32_t id;
    uint32_t owner_pid;
    uint32_t device_node_id;
    ipo_input_dev_desc_t desc;
    uint32_t event_count;
    input_dev_axis_t *axes;
    uint32_t axis_count;
    uint32_t axis_cap;
} input_dev_slot_t;

typedef struct {
    bool active;
    uint32_t pid;
    ipo_input_event_t *ring;
    uint32_t cap;
    uint32_t head;
    uint32_t tail;
} input_ev_queue_t;

static input_dev_slot_t *s_input_devs = NULL;
static uint32_t s_input_dev_cap = 0u;
static uint32_t s_next_input_dev_id = 1u;

static input_ev_queue_t *s_input_ev_queues = NULL;
static uint32_t s_input_ev_queue_cap = 0u;

typedef struct {
    bool active;
    uint32_t handle;
    uint32_t owner_pid;
    uint32_t chan_mask;
    uint32_t flags;
    ipo_ps2_byte_event_t *ring;
    uint32_t cap;
    uint32_t head;
    uint32_t tail;
} ps2_hook_slot_t;

static ps2_hook_slot_t *s_ps2_hooks = NULL;
static uint32_t s_ps2_hook_cap = 0u;
static uint32_t s_next_ps2_handle = 1u;

typedef struct {
    bool active;
    bool is_dma;
    uint32_t owner_pid;
    uint32_t virt_base;
    uint32_t phys_base;
    uint32_t aligned_size;
} hw_map_entry_t;

typedef struct {
    bool active;
    uint32_t dev_id;
    uint32_t owner_pid;
} user_dev_entry_t;

static hw_map_entry_t *s_hw_maps = NULL;
static uint32_t s_hw_map_cap = 0u;

static user_dev_entry_t *s_user_devs = NULL;
static uint32_t s_user_dev_cap = 0u;

static void shortcut_reconcile_chord_states(void);

void ipo_key_decoder_reset(ipo_key_decoder_t *dec) {
    if (!dec) return;
    memset(dec, 0, sizeof(*dec));
}

bool ipo_key_decoder_feed(ipo_key_decoder_t *dec, uint8_t raw_byte, ipo_key_event_t *out_ev) {
    if (!dec || !out_ev || raw_byte == 0x00u) return false;

    if (dec->len >= 4u) {
        dec->state = DEC_IDLE;
        dec->len = 0u;
    }

    if (dec->state == DEC_IDLE) {
        if (raw_byte == 0xE0u) {
            dec->raw_buf[0] = 0xE0u;
            dec->len = 1u;
            dec->state = DEC_E0;
            return false;
        }
        if (raw_byte == 0xE1u) {
            dec->raw_buf[0] = 0xE1u;
            dec->len = 1u;
            dec->state = DEC_E1_B1;
            return false;
        }
        memset(out_ev, 0, sizeof(*out_ev));
        out_ev->keycode = (uint32_t)(raw_byte & 0x7Fu);
        out_ev->flags = (raw_byte & 0x80u) ? IPO_KEY_FLAG_BREAK : 0u;
        out_ev->raw_len = 1u;
        out_ev->raw_bytes[0] = raw_byte;
        out_ev->timestamp_ms = timer_millis();
        return true;
    }

    if (dec->state == DEC_E0) {
        if (raw_byte == 0xE0u) {
            return false;
        }
        memset(out_ev, 0, sizeof(*out_ev));
        out_ev->keycode = IPO_KEY_MAKE_EXT_E0(raw_byte);
        out_ev->flags = IPO_KEY_FLAG_EXT_E0 | ((raw_byte & 0x80u) ? IPO_KEY_FLAG_BREAK : 0u);
        out_ev->raw_len = 2u;
        out_ev->raw_bytes[0] = 0xE0u;
        out_ev->raw_bytes[1] = raw_byte;
        out_ev->timestamp_ms = timer_millis();
        dec->state = DEC_IDLE;
        dec->len = 0u;
        return true;
    }

    if (dec->state == DEC_E1_B1) {
        dec->e1_b1 = raw_byte;
        dec->raw_buf[1] = raw_byte;
        dec->len = 2u;
        dec->state = DEC_E1_B2;
        return false;
    }

    /* DEC_E1_B2 */
    memset(out_ev, 0, sizeof(*out_ev));
    out_ev->keycode = IPO_KEY_MAKE_EXT_E1(dec->e1_b1, raw_byte);
    out_ev->flags = IPO_KEY_FLAG_EXT_E1 | ((dec->e1_b1 & 0x80u) ? IPO_KEY_FLAG_BREAK : 0u);
    out_ev->raw_len = 3u;
    out_ev->raw_bytes[0] = 0xE1u;
    out_ev->raw_bytes[1] = dec->e1_b1;
    out_ev->raw_bytes[2] = raw_byte;
    out_ev->timestamp_ms = timer_millis();
    dec->state = DEC_IDLE;
    dec->len = 0u;
    return true;
}

static bool pressed_contains(uint32_t kc) {
    for (uint32_t i = 0; i < s_pressed_count; i++) {
        if (s_pressed_keys[i] == kc) return true;
    }
    return false;
}

static bool pressed_add(uint32_t kc, bool *out_repeat) {
    if (pressed_contains(kc)) {
        if (out_repeat) *out_repeat = true;
        return true;
    }
    if (out_repeat) *out_repeat = false;
    if (s_pressed_count >= s_pressed_cap) {
        uint32_t new_cap = s_pressed_cap ? (s_pressed_cap * 2u) : 16u;
        uint32_t *nb = (uint32_t *)krealloc(s_pressed_keys, new_cap * sizeof(uint32_t));
        if (!nb) return false;
        s_pressed_keys = nb;
        s_pressed_cap = new_cap;
    }
    s_pressed_keys[s_pressed_count++] = kc;
    return true;
}

static void pressed_remove(uint32_t kc) {
    for (uint32_t i = 0; i < s_pressed_count; i++) {
        if (s_pressed_keys[i] == kc) {
            s_pressed_keys[i] = s_pressed_keys[s_pressed_count - 1u];
            s_pressed_count--;
            return;
        }
    }
}

static void update_ext_modifiers_for_key(uint32_t kc, bool is_break) {
    uint32_t bit = 0u;
    if (kc == 0x2Au || kc == 0x36u) {
        bit = IPO_MOD_SHIFT;
    } else if (kc == 0x1Du || kc == IPO_KEY_MAKE_EXT_E0(0x1Du)) {
        bit = IPO_MOD_CTRL;
    } else if (kc == 0x38u || kc == IPO_KEY_MAKE_EXT_E0(0x38u)) {
        bit = IPO_MOD_ALT;
    } else if (kc == IPO_KEY_MAKE_EXT_E0(0x5Bu) || kc == IPO_KEY_MAKE_EXT_E0(0x5Cu)) {
        bit = IPO_MOD_SUPER;
    } else if (kc == 0x3Au && !is_break) {
        s_ext_modifiers ^= IPO_MOD_CAPS;
        return;
    } else if (kc == 0x45u && !is_break) {
        s_ext_modifiers ^= IPO_MOD_NUM;
        return;
    }

    if (bit == 0u) return;

    if (!is_break) {
        s_ext_modifiers |= bit;
    } else {
        /* Recompute modifier group across both left/right keys */
        if (bit == IPO_MOD_SHIFT) {
            if (!pressed_contains(0x2Au) && !pressed_contains(0x36u)) {
                s_ext_modifiers &= ~IPO_MOD_SHIFT;
            }
        } else if (bit == IPO_MOD_CTRL) {
            if (!pressed_contains(0x1Du) && !pressed_contains(IPO_KEY_MAKE_EXT_E0(0x1Du))) {
                s_ext_modifiers &= ~IPO_MOD_CTRL;
            }
        } else if (bit == IPO_MOD_ALT) {
            if (!pressed_contains(0x38u) && !pressed_contains(IPO_KEY_MAKE_EXT_E0(0x38u))) {
                s_ext_modifiers &= ~IPO_MOD_ALT;
            }
        } else if (bit == IPO_MOD_SUPER) {
            if (!pressed_contains(IPO_KEY_MAKE_EXT_E0(0x5Bu)) &&
                !pressed_contains(IPO_KEY_MAKE_EXT_E0(0x5Cu))) {
                s_ext_modifiers &= ~IPO_MOD_SUPER;
            }
        }
    }
}

uint32_t ipo_input_get_modifiers(void) {
    uint32_t mods = s_ext_modifiers;
    if (keyboard_is_shift_pressed()) mods |= IPO_MOD_SHIFT;
    if (keyboard_is_ctrl_pressed()) mods |= IPO_MOD_CTRL;
    if (keyboard_is_alt_pressed()) mods |= IPO_MOD_ALT;
    return mods;
}

bool ipo_input_is_key_down(uint32_t keycode) {
    return pressed_contains(keycode);
}

void input_ext_clear_key_state(void) {
    s_pressed_count = 0u;
    s_ext_modifiers &= (IPO_MOD_CAPS | IPO_MOD_NUM);
    for (uint32_t i = 0; i < s_shortcut_cap; i++) {
        if (s_shortcuts[i].active) {
            s_shortcuts[i].armed = false;
            s_shortcuts[i].triggered = false;
        }
    }
}

#define DEFINE_RING_PUSH(FN_NAME, SLOT_T, ELEM_T, INITIAL_CAP)                  \
static bool FN_NAME(SLOT_T *q, const ELEM_T *ev) {                              \
    if (!q->ring) {                                                             \
        q->ring = (ELEM_T *)kmalloc((INITIAL_CAP) * sizeof(ELEM_T));            \
        if (!q->ring) return false;                                             \
        q->cap = (INITIAL_CAP);                                                 \
        q->head = 0u;                                                           \
        q->tail = 0u;                                                           \
    }                                                                           \
    if (((q->head + 1u) % q->cap) == q->tail) {                                 \
        uint32_t old_cap = q->cap;                                              \
        uint32_t new_cap = old_cap * 2u;                                        \
        ELEM_T *nb = (ELEM_T *)kmalloc(new_cap * sizeof(ELEM_T));               \
        if (!nb) {                                                              \
            q->tail = (q->tail + 1u) % old_cap;                                 \
        } else {                                                                \
            uint32_t cnt = (q->head - q->tail + old_cap) % old_cap;             \
            for (uint32_t i = 0; i < cnt; i++) {                                \
                nb[i] = q->ring[(q->tail + i) % old_cap];                       \
            }                                                                   \
            kfree(q->ring);                                                     \
            q->ring = nb;                                                       \
            q->tail = 0u;                                                       \
            q->head = cnt;                                                      \
            q->cap = new_cap;                                                   \
        }                                                                       \
    }                                                                           \
    q->ring[q->head] = *ev;                                                     \
    q->head = (q->head + 1u) % q->cap;                                          \
    return true;                                                                \
}

#define DEFINE_RING_POP(FN_NAME, SLOT_T, ELEM_T)                                \
static bool FN_NAME(SLOT_T *q, ELEM_T *out_ev) {                                \
    if (!q || !q->ring || q->cap == 0u || q->head == q->tail) return false;     \
    *out_ev = q->ring[q->tail];                                                 \
    q->tail = (q->tail + 1u) % q->cap;                                          \
    return true;                                                                \
}

DEFINE_RING_PUSH(key_ring_push, key_sub_queue_t, ipo_key_event_t, 32u)
DEFINE_RING_POP(key_ring_pop, key_sub_queue_t, ipo_key_event_t)

DEFINE_RING_PUSH(shortcut_ring_push, shortcut_queue_t, ipo_shortcut_event_t, 32u)
DEFINE_RING_POP(shortcut_ring_pop, shortcut_queue_t, ipo_shortcut_event_t)

DEFINE_RING_PUSH(input_dev_ring_push, input_ev_queue_t, ipo_input_event_t, 64u)
DEFINE_RING_POP(input_dev_ring_pop, input_ev_queue_t, ipo_input_event_t)

DEFINE_RING_PUSH(ps2_ring_push, ps2_hook_slot_t, ipo_ps2_byte_event_t, 128u)
DEFINE_RING_POP(ps2_ring_pop, ps2_hook_slot_t, ipo_ps2_byte_event_t)

static void *ensure_pid_queue_slot(void **arr_ptr, uint32_t *cap_ptr,
                                   size_t elem_size, size_t pid_off, uint32_t pid) {
    uint8_t *base = (uint8_t *)*arr_ptr;
    uint32_t cap = *cap_ptr;
    uint32_t free_slot = cap;

    for (uint32_t i = 0; i < cap; i++) {
        uint8_t *elem = base + (size_t)i * elem_size;
        bool active;
        uint32_t elem_pid;
        memcpy(&active, elem, sizeof(active));
        memcpy(&elem_pid, elem + pid_off, sizeof(elem_pid));
        if (active && elem_pid == pid) {
            return elem;
        }
        if (!active && free_slot == cap) {
            free_slot = i;
        }
    }
    if (free_slot == cap) {
        uint32_t nc = cap ? (cap * 2u) : 8u;
        void *nb = krealloc(*arr_ptr, (size_t)nc * elem_size);
        if (!nb) return NULL;
        base = (uint8_t *)nb;
        memset(base + (size_t)cap * elem_size, 0, (size_t)(nc - cap) * elem_size);
        *arr_ptr = nb;
        *cap_ptr = nc;
    }
    uint8_t *slot = base + (size_t)free_slot * elem_size;
    bool active = true;
    memcpy(slot, &active, sizeof(active));
    memcpy(slot + pid_off, &pid, sizeof(pid));
    return slot;
}

static key_sub_queue_t *ensure_key_sub_for_pid(uint32_t pid) {
    void *arr = s_key_subs;
    key_sub_queue_t *res = (key_sub_queue_t *)ensure_pid_queue_slot(
        &arr, &s_key_sub_cap, sizeof(*s_key_subs),
        offsetof(key_sub_queue_t, pid), pid);
    s_key_subs = (key_sub_queue_t *)arr;
    return res;
}

static shortcut_queue_t *ensure_shortcut_queue_for_pid(uint32_t pid) {
    void *arr = s_shortcut_queues;
    shortcut_queue_t *res = (shortcut_queue_t *)ensure_pid_queue_slot(
        &arr, &s_shortcut_queue_cap, sizeof(*s_shortcut_queues),
        offsetof(shortcut_queue_t, pid), pid);
    s_shortcut_queues = (shortcut_queue_t *)arr;
    return res;
}

static input_ev_queue_t *ensure_input_ev_queue_for_pid(uint32_t pid) {
    void *arr = s_input_ev_queues;
    input_ev_queue_t *res = (input_ev_queue_t *)ensure_pid_queue_slot(
        &arr, &s_input_ev_queue_cap, sizeof(*s_input_ev_queues),
        offsetof(input_ev_queue_t, pid), pid);
    s_input_ev_queues = (input_ev_queue_t *)arr;
    return res;
}

static bool shortcut_key_in_chord(const shortcut_entry_t *sc, uint32_t kc) {
    for (uint32_t i = 0; i < sc->key_count; i++) {
        if (sc->keys[i] == kc) return true;
    }
    return false;
}

static bool shortcut_all_keys_down(const shortcut_entry_t *sc) {
    if (sc->key_count == 0u) return false;
    for (uint32_t i = 0; i < sc->key_count; i++) {
        if (!pressed_contains(sc->keys[i])) return false;
    }
    return true;
}

static bool shortcut_mods_match(const shortcut_entry_t *sc, uint32_t mods) {
    uint32_t req = sc->mod_mask & 0x3Fu;
    uint32_t cur = mods & 0x3Fu;
    if (sc->flags & IPO_SHORTCUT_FLAG_EXACT_MODS) {
        return (cur & 0x0Fu) == (req & 0x0Fu);
    }
    return (cur & req) == req;
}

static void shortcut_reconcile_chord_states(void) {
    uint32_t mods = ipo_input_get_modifiers();
    for (uint32_t i = 0; i < s_shortcut_cap; i++) {
        shortcut_entry_t *sc = &s_shortcuts[i];
        if (!sc->active) continue;
        if (!shortcut_all_keys_down(sc) || !shortcut_mods_match(sc, mods)) {
            if (!(sc->flags & IPO_SHORTCUT_FLAG_ON_RELEASE)) {
                sc->triggered = false;
            }
            if (!shortcut_all_keys_down(sc)) {
                sc->armed = false;
                sc->triggered = false;
            }
        }
    }
}

uint32_t ipo_shortcut_register(const ipo_shortcut_spec_t *spec, uint32_t owner_pid) {
    if (!spec || spec->key_count == 0u || !spec->keys) return 0u;

    uint32_t slot = s_shortcut_cap;
    for (uint32_t i = 0; i < s_shortcut_cap; i++) {
        if (!s_shortcuts[i].active) { slot = i; break; }
    }
    if (slot == s_shortcut_cap) {
        uint32_t nc = s_shortcut_cap ? (s_shortcut_cap * 2u) : 16u;
        shortcut_entry_t *nb = (shortcut_entry_t *)krealloc(s_shortcuts, nc * sizeof(shortcut_entry_t));
        if (!nb) return 0u;
        memset(&nb[s_shortcut_cap], 0, (nc - s_shortcut_cap) * sizeof(shortcut_entry_t));
        s_shortcuts = nb;
        s_shortcut_cap = nc;
    }

    uint32_t *keys_copy = (uint32_t *)kmalloc(spec->key_count * sizeof(uint32_t));
    if (!keys_copy) return 0u;
    memcpy(keys_copy, spec->keys, spec->key_count * sizeof(uint32_t));

    uint32_t id = s_next_shortcut_id++;
    if (s_next_shortcut_id == 0u) s_next_shortcut_id = 1u;

    s_shortcuts[slot].active = true;
    s_shortcuts[slot].armed = false;
    s_shortcuts[slot].triggered = false;
    s_shortcuts[slot].id = id;
    s_shortcuts[slot].owner_pid = owner_pid;
    s_shortcuts[slot].window_handle = spec->window_handle;
    s_shortcuts[slot].mod_mask = spec->mod_mask;
    s_shortcuts[slot].key_count = spec->key_count;
    s_shortcuts[slot].keys = keys_copy;
    s_shortcuts[slot].flags = spec->flags;
    s_shortcuts[slot].user_tag = spec->user_tag;
    return id;
}

int ipo_shortcut_unregister(uint32_t shortcut_id, uint32_t caller_pid) {
    if (shortcut_id == 0u) return -1;
    for (uint32_t i = 0; i < s_shortcut_cap; i++) {
        if (s_shortcuts[i].active && s_shortcuts[i].id == shortcut_id) {
            if (caller_pid != 0u && s_shortcuts[i].owner_pid != caller_pid) {
                return -2;
            }
            s_shortcuts[i].active = false;
            if (s_shortcuts[i].keys) {
                kfree(s_shortcuts[i].keys);
                s_shortcuts[i].keys = NULL;
            }
            return 0;
        }
    }
    return -1;
}

bool ipo_shortcut_poll(uint32_t caller_pid, ipo_shortcut_event_t *out_ev) {
    if (!out_ev) return false;
    for (uint32_t i = 0; i < s_shortcut_queue_cap; i++) {
        if (s_shortcut_queues[i].active && s_shortcut_queues[i].pid == caller_pid) {
            return shortcut_ring_pop(&s_shortcut_queues[i], out_ev);
        }
    }
    return false;
}

bool ipo_input_poll_key_event(uint32_t caller_pid, ipo_key_event_t *out_ev) {
    if (!out_ev) return false;
    key_sub_queue_t *q = ensure_key_sub_for_pid(caller_pid);
    if (!q) return false;
    return key_ring_pop(q, out_ev);
}

static bool shortcut_matches_scope(const shortcut_entry_t *sc,
                                   uint32_t wm_window_handle,
                                   uint32_t target_pid,
                                   bool in_wm) {
    if (sc->flags & IPO_SHORTCUT_FLAG_GLOBAL) return true;
    if (in_wm) {
        if (sc->window_handle != 0u) {
            return wm_window_handle != 0u && sc->window_handle == wm_window_handle;
        }
        return target_pid != 0u && sc->owner_pid == target_pid;
    }
    if (target_pid != 0u && sc->owner_pid == target_pid) return true;
    return false;
}

static void dispatch_matched_shortcut(const shortcut_entry_t *sc,
                                      uint32_t trigger_kc,
                                      uint32_t mods,
                                      uint64_t ts_ms,
                                      uint32_t wm_window_handle,
                                      bool in_wm) {
    ipo_shortcut_event_t sev;
    sev.shortcut_id = sc->id;
    sev.user_tag = sc->user_tag;
    sev.trigger_keycode = trigger_kc;
    sev.modifiers = mods;
    sev.window_handle = sc->window_handle ? sc->window_handle : wm_window_handle;
    sev.reserved = 0u;
    sev.timestamp_ms = ts_ms;

    shortcut_queue_t *q = ensure_shortcut_queue_for_pid(sc->owner_pid);
    if (q) {
        shortcut_ring_push(q, &sev);
    }

    if (in_wm) {
        wm_window_t *dst_win = NULL;
        if (sc->window_handle != 0u) {
            dst_win = (wm_window_t *)(uintptr_t)sc->window_handle;
        } else if (wm_window_handle != 0u) {
            wm_window_t *fw = (wm_window_t *)(uintptr_t)wm_window_handle;
            if (wm_is_window_valid(fw) && fw->owner_pid == sc->owner_pid) {
                dst_win = fw;
            }
        }
        if (!dst_win && sc->owner_pid != 0u) {
            for (wm_window_t *w = wm_get_window_list(); w; w = w->next) {
                if (w->owner_pid == sc->owner_pid) {
                    dst_win = w;
                    break;
                }
            }
        }
        if (dst_win && wm_is_window_valid(dst_win)) {
            uint32_t payload = sc->user_tag ? sc->user_tag : sc->id;
            wm_notify_window_event(dst_win, WM_EVENT_SHORTCUT, payload);
        }
    }
}

bool ipo_input_process_key_event(const ipo_key_event_t *in_ev,
                                 uint32_t wm_window_handle,
                                 uint32_t target_pid,
                                 bool in_wm,
                                 bool *out_consumed) {
    if (out_consumed) *out_consumed = false;
    if (!in_ev || in_ev->keycode == 0u) return false;

    bool is_break = (in_ev->flags & IPO_KEY_FLAG_BREAK) != 0u;
    bool is_repeat = false;

    if (is_break) {
        /* For ON_RELEASE shortcuts: check before removing from pressed set */
        uint32_t mods = ipo_input_get_modifiers();
        bool consumed = false;
        for (uint32_t i = 0; i < s_shortcut_cap; i++) {
            shortcut_entry_t *sc = &s_shortcuts[i];
            if (!sc->active) continue;
            if ((sc->flags & IPO_SHORTCUT_FLAG_ON_RELEASE) &&
                sc->armed &&
                shortcut_key_in_chord(sc, in_ev->keycode) &&
                shortcut_matches_scope(sc, wm_window_handle, target_pid, in_wm) &&
                shortcut_mods_match(sc, mods)) {
                sc->armed = false;
                dispatch_matched_shortcut(sc, in_ev->keycode, mods,
                                          in_ev->timestamp_ms, wm_window_handle, in_wm);
                if ((sc->flags & IPO_SHORTCUT_FLAG_CONSUME) &&
                    !(sc->flags & IPO_SHORTCUT_FLAG_PASSIVE)) {
                    consumed = true;
                }
            }
        }
        pressed_remove(in_ev->keycode);
        update_ext_modifiers_for_key(in_ev->keycode, true);
        shortcut_reconcile_chord_states();

        ipo_key_event_t ev = *in_ev;
        ev.modifiers = ipo_input_get_modifiers();
        for (uint32_t i = 0; i < s_key_sub_cap; i++) {
            if (s_key_subs[i].active &&
                (s_key_subs[i].pid == target_pid || s_key_subs[i].pid == 0u)) {
                key_ring_push(&s_key_subs[i], &ev);
            }
        }
        if (out_consumed) *out_consumed = consumed;
        return consumed;
    }

    pressed_add(in_ev->keycode, &is_repeat);
    update_ext_modifiers_for_key(in_ev->keycode, false);
    uint32_t mods = ipo_input_get_modifiers();

    ipo_key_event_t ev = *in_ev;
    ev.modifiers = mods;
    if (is_repeat) ev.flags |= IPO_KEY_FLAG_REPEAT;

    /* Evaluate shortcuts: pass 0 = focused app/window, pass 1 = global */
    bool consumed = false;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < s_shortcut_cap; i++) {
            shortcut_entry_t *sc = &s_shortcuts[i];
            if (!sc->active) continue;
            bool is_global = (sc->flags & IPO_SHORTCUT_FLAG_GLOBAL) != 0u;
            if ((pass == 0 && is_global) || (pass == 1 && !is_global)) continue;

            if (!shortcut_key_in_chord(sc, ev.keycode)) continue;
            if (!shortcut_all_keys_down(sc)) continue;
            if (!shortcut_mods_match(sc, mods)) continue;
            if (!shortcut_matches_scope(sc, wm_window_handle, target_pid, in_wm)) continue;

            if (sc->flags & IPO_SHORTCUT_FLAG_ON_RELEASE) {
                sc->armed = true;
                continue;
            }

            bool allow_repeat = (sc->flags & IPO_SHORTCUT_FLAG_ALLOW_REPEAT) != 0u;
            if (sc->triggered && (!allow_repeat || !is_repeat)) {
                continue;
            }

            sc->triggered = true;
            dispatch_matched_shortcut(sc, ev.keycode, mods,
                                      ev.timestamp_ms, wm_window_handle, in_wm);
            if ((sc->flags & IPO_SHORTCUT_FLAG_CONSUME) &&
                !(sc->flags & IPO_SHORTCUT_FLAG_PASSIVE)) {
                consumed = true;
            }
        }
    }

    if (!consumed) {
        for (uint32_t i = 0; i < s_key_sub_cap; i++) {
            if (s_key_subs[i].active &&
                (s_key_subs[i].pid == target_pid || s_key_subs[i].pid == 0u)) {
                key_ring_push(&s_key_subs[i], &ev);
            }
        }
    }

    if (out_consumed) *out_consumed = consumed;
    return consumed;
}

uint32_t ipo_input_dev_register(const ipo_input_dev_desc_t *desc, uint32_t owner_pid) {
    if (!desc || desc->name[0] == '\0') return 0u;

    uint32_t slot = s_input_dev_cap;
    for (uint32_t i = 0; i < s_input_dev_cap; i++) {
        if (!s_input_devs[i].active) { slot = i; break; }
    }
    if (slot == s_input_dev_cap) {
        uint32_t nc = s_input_dev_cap ? (s_input_dev_cap * 2u) : 8u;
        input_dev_slot_t *nb = (input_dev_slot_t *)krealloc(s_input_devs, nc * sizeof(input_dev_slot_t));
        if (!nb) return 0u;
        memset(&nb[s_input_dev_cap], 0, (nc - s_input_dev_cap) * sizeof(input_dev_slot_t));
        s_input_devs = nb;
        s_input_dev_cap = nc;
    }

    uint32_t id = s_next_input_dev_id++;
    if (s_next_input_dev_id == 0u) s_next_input_dev_id = 1u;

    input_dev_slot_t *dev = &s_input_devs[slot];
    memset(dev, 0, sizeof(*dev));
    dev->active = true;
    dev->id = id;
    dev->owner_pid = owner_pid;
    dev->desc = *desc;
    dev->desc.name[sizeof(dev->desc.name) - 1u] = '\0';

    device_node_t node;
    device_node_init(&node);
    node.type = DEVICE_TYPE_INPUT;
    node.state = DEVICE_STATE_ACTIVE;
    node.res.bus_type = (desc->bus_type <= BUS_TYPE_VIRTUAL)
                        ? (bus_type_t)desc->bus_type
                        : BUS_TYPE_VIRTUAL;
    node.res.vendor_id = desc->vendor_id;
    node.res.device_id = desc->product_id;
    device_node_set_name(&node, dev->desc.name);
    device_node_set_model(&node, dev->desc.name);
    device_node_set_description(&node, "External Registered Input Device");
    device_node_t *reg = device_register(&node);
    dev->device_node_id = reg ? reg->id : 0u;
    device_node_free(&node);

    (void)ensure_input_ev_queue_for_pid(owner_pid);

    return id;
}

static void free_input_dev_slot(input_dev_slot_t *dev) {
    if (!dev || !dev->active) return;
    if (dev->device_node_id != 0u) {
        device_unregister(dev->device_node_id);
        dev->device_node_id = 0u;
    }
    if (dev->axes) {
        kfree(dev->axes);
        dev->axes = NULL;
    }
    dev->axis_count = 0u;
    dev->axis_cap = 0u;
    dev->active = false;
}

int ipo_input_dev_unregister(uint32_t dev_id, uint32_t caller_pid) {
    if (dev_id == 0u) return -1;
    for (uint32_t i = 0; i < s_input_dev_cap; i++) {
        if (s_input_devs[i].active && s_input_devs[i].id == dev_id) {
            if (caller_pid != 0u && s_input_devs[i].owner_pid != caller_pid) {
                return -2;
            }
            free_input_dev_slot(&s_input_devs[i]);
            return 0;
        }
    }
    return -1;
}

static void input_dev_store_axis(input_dev_slot_t *dev, uint16_t type, uint32_t code, int32_t value) {
    for (uint32_t i = 0; i < dev->axis_count; i++) {
        if (dev->axes[i].type == type && dev->axes[i].code == code) {
            dev->axes[i].value = value;
            return;
        }
    }
    if (dev->axis_count >= dev->axis_cap) {
        uint32_t nc = dev->axis_cap ? (dev->axis_cap * 2u) : 16u;
        input_dev_axis_t *nb = (input_dev_axis_t *)krealloc(dev->axes, nc * sizeof(input_dev_axis_t));
        if (!nb) return;
        dev->axes = nb;
        dev->axis_cap = nc;
    }
    dev->axes[dev->axis_count].type = type;
    dev->axes[dev->axis_count].code = code;
    dev->axes[dev->axis_count].value = value;
    dev->axis_count++;
}

int ipo_input_dev_emit(uint32_t dev_id, const ipo_input_event_t *in_ev, uint32_t caller_pid) {
    if (!in_ev) return -1;

    input_dev_slot_t *dev = NULL;
    for (uint32_t i = 0; i < s_input_dev_cap; i++) {
        if (s_input_devs[i].active && s_input_devs[i].id == dev_id) {
            dev = &s_input_devs[i];
            break;
        }
    }
    if (!dev) return -1;
    if (caller_pid != 0u && dev->owner_pid != caller_pid) return -2;

    ipo_input_event_t ev = *in_ev;
    ev.dev_id = dev_id;
    if (ev.timestamp_ms == 0u) ev.timestamp_ms = timer_millis();
    dev->event_count++;

    if (ev.type == IPO_EV_KEY || ev.type == IPO_EV_REL || ev.type == IPO_EV_ABS) {
        input_dev_store_axis(dev, ev.type, ev.code, ev.value);
    }

    for (uint32_t i = 0; i < s_input_ev_queue_cap; i++) {
        if (s_input_ev_queues[i].active) {
            input_dev_ring_push(&s_input_ev_queues[i], &ev);
        }
    }

    if (ev.type == IPO_EV_KEY && ev.code != 0u) {
        bool is_break = (ev.value == 0);
        bool in_wm = wm_session_active();
        wm_window_t *fw = in_wm ? wm_get_focused() : NULL;
        process_t *fg = process_get_displayed_foreground();
        if (!fg) fg = process_get_foreground();
        uint32_t target_pid = in_wm
            ? (fw ? fw->owner_pid : (fg ? fg->pid : 0u))
            : (fg ? fg->pid : caller_pid);

        ipo_key_event_t kev;
        memset(&kev, 0, sizeof(kev));
        kev.keycode = ev.code & 0x7FFFFFFFu;
        kev.flags = IPO_KEY_FLAG_INJECTED | (is_break ? IPO_KEY_FLAG_BREAK : 0u);
        if (kev.keycode >= 0xE000u && kev.keycode <= 0xE0FFu) kev.flags |= IPO_KEY_FLAG_EXT_E0;
        kev.source_dev_id = dev_id;
        kev.timestamp_ms = ev.timestamp_ms;

        bool consumed = false;
        ipo_input_process_key_event(&kev, (uint32_t)(uintptr_t)fw, target_pid, in_wm, &consumed);

        if (!consumed && in_wm && fw && wm_is_window_valid(fw)) {
            uint32_t ext_payload = (kev.keycode & 0x7FFFFFFFu) | (is_break ? 0x80000000u : 0u);
            wm_notify_window_event(fw, WM_EVENT_KEY_EXT, ext_payload);
            if ((dev->desc.caps & IPO_INPUT_CAP_KEYBOARD_ROUTE) && kev.keycode <= 0xE0FFu) {
                if (!is_break) {
                    if (kev.keycode >= 0xE000u) wm_notify_window_event(fw, WM_EVENT_KEY_DOWN, 0xE0u);
                    wm_notify_window_event(fw, WM_EVENT_KEY_DOWN, kev.keycode & 0x7Fu);
                } else {
                    wm_notify_window_event(fw, WM_EVENT_KEY_UP, (kev.keycode & 0x7Fu) | 0x80u);
                }
            }
        } else if (!consumed && !in_wm && (dev->desc.caps & IPO_INPUT_CAP_KEYBOARD_ROUTE)) {
            if (kev.keycode <= 0x7Fu) {
                keyboard_enqueue_scancode((uint8_t)((kev.keycode & 0x7Fu) | (is_break ? 0x80u : 0u)));
            } else if (kev.keycode >= 0xE000u && kev.keycode <= 0xE07Fu) {
                keyboard_enqueue_scancode(0xE0u);
                keyboard_enqueue_scancode((uint8_t)((kev.keycode & 0x7Fu) | (is_break ? 0x80u : 0u)));
            }
        }
    }

    return 0;
}

uint32_t ipo_input_dev_list(ipo_input_dev_info_t *out_arr, uint32_t max_count) {
    uint32_t total = 0u;
    for (uint32_t i = 0; i < s_input_dev_cap; i++) {
        if (!s_input_devs[i].active) continue;
        if (out_arr && total < max_count) {
            input_dev_slot_t *d = &s_input_devs[i];
            ipo_input_dev_info_t *info = &out_arr[total];
            memset(info, 0, sizeof(*info));
            info->id = d->id;
            info->owner_pid = d->owner_pid;
            info->device_node_id = d->device_node_id;
            info->caps = d->desc.caps;
            info->bus_type = d->desc.bus_type;
            info->vendor_id = d->desc.vendor_id;
            info->product_id = d->desc.product_id;
            info->version = d->desc.version;
            info->event_count = d->event_count;
            strncpy(info->name, d->desc.name, sizeof(info->name) - 1u);
        }
        total++;
    }
    return total;
}

bool ipo_input_dev_poll_event(uint32_t caller_pid, uint32_t dev_filter_id, ipo_input_event_t *out_ev) {
    if (!out_ev) return false;
    input_ev_queue_t *q = ensure_input_ev_queue_for_pid(caller_pid);
    if (!q) return false;

    ipo_input_event_t tmp;
    while (input_dev_ring_pop(q, &tmp)) {
        if (dev_filter_id == 0u || tmp.dev_id == dev_filter_id) {
            *out_ev = tmp;
            return true;
        }
    }
    return false;
}

int ipo_input_dev_get_state(uint32_t dev_id, uint16_t type, uint32_t code, int32_t *out_val) {
    if (!out_val) return -1;
    for (uint32_t i = 0; i < s_input_dev_cap; i++) {
        if (s_input_devs[i].active && s_input_devs[i].id == dev_id) {
            for (uint32_t a = 0; a < s_input_devs[i].axis_count; a++) {
                if (s_input_devs[i].axes[a].type == type &&
                    s_input_devs[i].axes[a].code == code) {
                    *out_val = s_input_devs[i].axes[a].value;
                    return 0;
                }
            }
            *out_val = 0;
            return 0;
        }
    }
    return -1;
}

uint32_t ipo_ps2_hook_register(uint32_t chan_mask, uint32_t flags, uint32_t owner_pid) {
    if ((chan_mask & (IPO_PS2_CHAN_KBD | IPO_PS2_CHAN_AUX)) == 0u) return 0u;

    uint32_t slot = s_ps2_hook_cap;
    for (uint32_t i = 0; i < s_ps2_hook_cap; i++) {
        if (!s_ps2_hooks[i].active) { slot = i; break; }
    }
    if (slot == s_ps2_hook_cap) {
        uint32_t nc = s_ps2_hook_cap ? (s_ps2_hook_cap * 2u) : 8u;
        ps2_hook_slot_t *nb = (ps2_hook_slot_t *)krealloc(s_ps2_hooks, nc * sizeof(ps2_hook_slot_t));
        if (!nb) return 0u;
        memset(&nb[s_ps2_hook_cap], 0, (nc - s_ps2_hook_cap) * sizeof(ps2_hook_slot_t));
        s_ps2_hooks = nb;
        s_ps2_hook_cap = nc;
    }

    uint32_t h = s_next_ps2_handle++;
    if (s_next_ps2_handle == 0u) s_next_ps2_handle = 1u;

    s_ps2_hooks[slot].active = true;
    s_ps2_hooks[slot].handle = h;
    s_ps2_hooks[slot].owner_pid = owner_pid;
    s_ps2_hooks[slot].chan_mask = chan_mask;
    s_ps2_hooks[slot].flags = flags;
    return h;
}

int ipo_ps2_hook_unregister(uint32_t handle, uint32_t caller_pid) {
    if (handle == 0u) return -1;
    for (uint32_t i = 0; i < s_ps2_hook_cap; i++) {
        if (s_ps2_hooks[i].active && s_ps2_hooks[i].handle == handle) {
            if (caller_pid != 0u && s_ps2_hooks[i].owner_pid != caller_pid) {
                return -2;
            }
            s_ps2_hooks[i].active = false;
            if (s_ps2_hooks[i].ring) {
                kfree(s_ps2_hooks[i].ring);
                s_ps2_hooks[i].ring = NULL;
            }
            s_ps2_hooks[i].cap = 0u;
            s_ps2_hooks[i].head = 0u;
            s_ps2_hooks[i].tail = 0u;
            return 0;
        }
    }
    return -1;
}

uint32_t ipo_ps2_hook_poll(uint32_t handle, uint32_t caller_pid,
                           ipo_ps2_byte_event_t *out_events, uint32_t max_count) {
    if (handle == 0u || !out_events || max_count == 0u) return 0u;
    keyboard_poll();
    for (uint32_t i = 0; i < s_ps2_hook_cap; i++) {
        if (s_ps2_hooks[i].active && s_ps2_hooks[i].handle == handle) {
            if (caller_pid != 0u && s_ps2_hooks[i].owner_pid != caller_pid) {
                return 0u;
            }
            uint32_t cnt = 0u;
            while (cnt < max_count && ps2_ring_pop(&s_ps2_hooks[i], &out_events[cnt])) {
                cnt++;
            }
            return cnt;
        }
    }
    return 0u;
}

bool ipo_ps2_dispatch_byte(bool is_aux, uint8_t data) {
    uint32_t chan = is_aux ? IPO_PS2_CHAN_AUX : IPO_PS2_CHAN_KBD;
    bool consumed = false;
    ipo_ps2_byte_event_t ev;
    ev.chan_mask = chan;
    ev.byte = data;
    ev.chan = (uint8_t)chan;
    ev.reserved[0] = 0u;
    ev.reserved[1] = 0u;
    ev.timestamp_ms = timer_millis();

    for (uint32_t i = 0; i < s_ps2_hook_cap; i++) {
        if (s_ps2_hooks[i].active && (s_ps2_hooks[i].chan_mask & chan) != 0u) {
            ps2_ring_push(&s_ps2_hooks[i], &ev);
            if (s_ps2_hooks[i].flags & IPO_PS2_HOOK_EXCLUSIVE) {
                consumed = true;
            }
        }
    }
    return consumed;
}

static bool ps2_wait_ibf_clear(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(KBD_STATUS_PORT) & KBD_STATUS_IBF) == 0u) return true;
        io_wait();
    }
    return false;
}

static bool ps2_wait_obf_full(uint8_t *out_byte) {
    for (int i = 0; i < 100000; i++) {
        if (inb(KBD_STATUS_PORT) & KBD_STATUS_OBF) {
            *out_byte = inb(KBD_DATA_PORT);
            return true;
        }
        io_wait();
    }
    return false;
}

static bool ps2_send_port_byte(uint8_t port_id, uint8_t byte) {
    if (port_id == IPO_PS2_PORT_AUX) {
        if (!ps2_wait_ibf_clear()) return false;
        outb(KBD_CMD_PORT, 0xD4u);
    }
    if (!ps2_wait_ibf_clear()) return false;
    outb(KBD_DATA_PORT, byte);
    return true;
}

int ipo_ps2_transact(const ipo_ps2_transact_req_t *req) {
    if (!req) return -1;

    if (req->port_id == IPO_PS2_PORT_CTRL) {
        if (!ps2_wait_ibf_clear()) return -2;
        outb(KBD_CMD_PORT, req->cmd);
        for (uint32_t i = 0; i < req->param_len; i++) {
            if (!ps2_wait_ibf_clear()) return -2;
            outb(KBD_DATA_PORT, req->params[i]);
        }
        for (uint32_t i = 0; i < req->reply_len; i++) {
            uint8_t b = 0u;
            if (!ps2_wait_obf_full(&b)) return -3;
            if (req->reply) req->reply[i] = b;
        }
        return 0;
    }

    if (req->port_id != IPO_PS2_PORT_KBD && req->port_id != IPO_PS2_PORT_AUX) {
        return -1;
    }

    /* Drain stale bytes first */
    for (int i = 0; i < 16; i++) {
        if (!(inb(KBD_STATUS_PORT) & KBD_STATUS_OBF)) break;
        (void)inb(KBD_DATA_PORT);
        io_wait();
    }

    if (!ps2_send_port_byte(req->port_id, req->cmd)) return -2;
    uint8_t ack = 0u;
    if (!ps2_wait_obf_full(&ack)) return -3;
    if (ack != 0xFAu && ack != 0xAAu && ack != 0xEEu) {
        return -(int)ack;
    }

    for (uint32_t i = 0; i < req->param_len; i++) {
        if (!ps2_send_port_byte(req->port_id, req->params[i])) return -2;
        if (!ps2_wait_obf_full(&ack)) return -3;
        if (ack != 0xFAu) return -(int)ack;
    }

    for (uint32_t i = 0; i < req->reply_len; i++) {
        uint8_t b = 0u;
        if (!ps2_wait_obf_full(&b)) return -3;
        if (req->reply) req->reply[i] = b;
    }
    return 0;
}

static void hw_free_phys_pages(uint32_t phys_base, uint32_t aligned_size) {
    uint32_t pages = aligned_size / HW_PAGE_SIZE;
    for (uint32_t p = 0u; p < pages; p++) {
        memory_page_free((void *)(uintptr_t)(phys_base + p * HW_PAGE_SIZE));
    }
}

static hw_map_entry_t *alloc_hw_map_slot(void) {
    for (uint32_t i = 0; i < s_hw_map_cap; i++) {
        if (!s_hw_maps[i].active) return &s_hw_maps[i];
    }
    uint32_t nc = s_hw_map_cap ? (s_hw_map_cap * 2u) : 16u;
    hw_map_entry_t *nb = (hw_map_entry_t *)krealloc(s_hw_maps, nc * sizeof(hw_map_entry_t));
    if (!nb) return NULL;
    memset(&nb[s_hw_map_cap], 0, (nc - s_hw_map_cap) * sizeof(hw_map_entry_t));
    s_hw_maps = nb;
    hw_map_entry_t *res = &s_hw_maps[s_hw_map_cap];
    s_hw_map_cap = nc;
    return res;
}

static bool hw_virt_range_in_use(uint32_t pid, uint32_t start, uint32_t size) {
    uint64_t end = (uint64_t)start + size;
    for (uint32_t i = 0; i < s_hw_map_cap; i++) {
        if (!s_hw_maps[i].active || s_hw_maps[i].owner_pid != pid) continue;
        uint64_t s2 = s_hw_maps[i].virt_base;
        uint64_t e2 = s2 + s_hw_maps[i].aligned_size;
        if (start < e2 && end > s2) return true;
    }
    return false;
}

bool ipo_hw_user_range(uint32_t pid, const void *ptr, uint32_t length) {
    if (length == 0u) return true;
    if (!ptr || pid == 0u) return false;
    uint64_t start = (uint32_t)(uintptr_t)ptr;
    uint64_t end = start + length;
    for (uint32_t i = 0; i < s_hw_map_cap; i++) {
        if (!s_hw_maps[i].active || s_hw_maps[i].owner_pid != pid) continue;
        uint64_t s2 = s_hw_maps[i].virt_base;
        uint64_t e2 = s2 + s_hw_maps[i].aligned_size;
        if (start >= s2 && end <= e2) return true;
    }
    return false;
}

static uint32_t hw_find_free_va(uint32_t pid, uint32_t aligned_size) {
    for (uint32_t va = HW_MMIO_VBASE_START;
         va <= HW_MMIO_VBASE_END - aligned_size;
         va += HW_PAGE_SIZE) {
        if (!hw_virt_range_in_use(pid, va, aligned_size)) {
            return va;
        }
    }
    return 0u;
}

void *ipo_hw_map_mmio(uint32_t phys_addr, uint32_t size, uint32_t flags, uint32_t owner_pid) {
    if (size == 0u) return NULL;
    uint32_t page_off = phys_addr & (HW_PAGE_SIZE - 1u);
    uint32_t phys_base = phys_addr & ~(HW_PAGE_SIZE - 1u);
    uint32_t aligned_size = (page_off + size + HW_PAGE_SIZE - 1u) & ~(HW_PAGE_SIZE - 1u);
    if (aligned_size == 0u || aligned_size > (HW_MMIO_VBASE_END - HW_MMIO_VBASE_START)) {
        return NULL;
    }

    process_t *proc = owner_pid ? process_find_by_pid(owner_pid) : NULL;
    if (!proc || !proc->address_space) {
        /* Kernel / shared context can address 32-bit physical MMIO directly */
        return (void *)(uintptr_t)phys_addr;
    }

    uint32_t virt_base = hw_find_free_va(owner_pid, aligned_size);
    if (virt_base == 0u) return NULL;

    hw_map_entry_t *slot = alloc_hw_map_slot();
    if (!slot) return NULL;

    uint32_t pte_flags = ADDRESS_SPACE_PAGE_PRESENT | ADDRESS_SPACE_PAGE_WRITABLE | ADDRESS_SPACE_PAGE_USER;
    if (flags == 0u || (flags & IPO_MMIO_CACHE_DISABLE)) {
        pte_flags |= ADDRESS_SPACE_PAGE_CACHE_DISABLE;
    }
    if (flags == 0u || (flags & IPO_MMIO_WRITE_THROUGH)) {
        pte_flags |= ADDRESS_SPACE_PAGE_WRITE_THROUGH;
    }

    uint32_t pages = aligned_size / HW_PAGE_SIZE;
    for (uint32_t p = 0u; p < pages; p++) {
        if (!address_space_map(proc->address_space,
                               virt_base + p * HW_PAGE_SIZE,
                               phys_base + p * HW_PAGE_SIZE,
                               pte_flags)) {
            while (p > 0u) {
                p--;
                address_space_unmap(proc->address_space, virt_base + p * HW_PAGE_SIZE);
            }
            return NULL;
        }
    }

    slot->active = true;
    slot->is_dma = false;
    slot->owner_pid = owner_pid;
    slot->virt_base = virt_base;
    slot->phys_base = phys_base;
    slot->aligned_size = aligned_size;

    return (void *)(uintptr_t)(virt_base + page_off);
}

int ipo_hw_unmap_mmio(void *virt_addr, uint32_t size, uint32_t caller_pid) {
    (void)size;
    if (!virt_addr) return -1;
    uint32_t va_base = (uint32_t)(uintptr_t)virt_addr & ~(HW_PAGE_SIZE - 1u);

    for (uint32_t i = 0; i < s_hw_map_cap; i++) {
        hw_map_entry_t *m = &s_hw_maps[i];
        if (m->active && !m->is_dma && m->virt_base == va_base &&
            (caller_pid == 0u || m->owner_pid == caller_pid)) {
            process_t *proc = m->owner_pid ? process_find_by_pid(m->owner_pid) : NULL;
            if (proc && proc->address_space) {
                uint32_t pages = m->aligned_size / HW_PAGE_SIZE;
                for (uint32_t p = 0u; p < pages; p++) {
                    address_space_unmap(proc->address_space, m->virt_base + p * HW_PAGE_SIZE);
                }
            }
            m->active = false;
            return 0;
        }
    }
    return -1;
}

void *ipo_hw_alloc_dma(uint32_t size, uint32_t alignment, uint32_t *out_phys, uint32_t owner_pid) {
    if (size == 0u) return NULL;
    if (alignment < HW_PAGE_SIZE) alignment = HW_PAGE_SIZE;
    uint32_t aligned_size = (size + HW_PAGE_SIZE - 1u) & ~(HW_PAGE_SIZE - 1u);

    bool single_page = (aligned_size == HW_PAGE_SIZE && alignment == HW_PAGE_SIZE);
    void *phys_ptr = single_page ? memory_page_alloc() : memory_map_alloc(aligned_size, alignment);
    if (!phys_ptr) return NULL;
    memset(phys_ptr, 0, aligned_size);

    uint32_t phys_base = (uint32_t)(uintptr_t)phys_ptr;
    process_t *proc = owner_pid ? process_find_by_pid(owner_pid) : NULL;
    if (!proc || !proc->address_space) {
        if (out_phys) *out_phys = phys_base;
        return phys_ptr;
    }

    uint32_t virt_base = hw_find_free_va(owner_pid, aligned_size);
    if (virt_base == 0u) {
        hw_free_phys_pages(phys_base, aligned_size);
        return NULL;
    }

    hw_map_entry_t *slot = alloc_hw_map_slot();
    if (!slot) {
        hw_free_phys_pages(phys_base, aligned_size);
        return NULL;
    }

    uint32_t pte_flags = ADDRESS_SPACE_PAGE_PRESENT | ADDRESS_SPACE_PAGE_WRITABLE |
                         ADDRESS_SPACE_PAGE_USER | ADDRESS_SPACE_PAGE_CACHE_DISABLE;
    uint32_t pages = aligned_size / HW_PAGE_SIZE;
    for (uint32_t p = 0u; p < pages; p++) {
        if (!address_space_map(proc->address_space,
                               virt_base + p * HW_PAGE_SIZE,
                               phys_base + p * HW_PAGE_SIZE,
                               pte_flags)) {
            while (p > 0u) {
                p--;
                address_space_unmap(proc->address_space, virt_base + p * HW_PAGE_SIZE);
            }
            hw_free_phys_pages(phys_base, aligned_size);
            return NULL;
        }
    }

    slot->active = true;
    slot->is_dma = true;
    slot->owner_pid = owner_pid;
    slot->virt_base = virt_base;
    slot->phys_base = phys_base;
    slot->aligned_size = aligned_size;

    if (out_phys) *out_phys = phys_base;
    return (void *)(uintptr_t)virt_base;
}

int ipo_hw_free_dma(void *virt_addr, uint32_t caller_pid) {
    if (!virt_addr) return -1;
    uint32_t va_base = (uint32_t)(uintptr_t)virt_addr & ~(HW_PAGE_SIZE - 1u);

    for (uint32_t i = 0; i < s_hw_map_cap; i++) {
        hw_map_entry_t *m = &s_hw_maps[i];
        if (m->active && m->is_dma && m->virt_base == va_base &&
            (caller_pid == 0u || m->owner_pid == caller_pid)) {
            process_t *proc = m->owner_pid ? process_find_by_pid(m->owner_pid) : NULL;
            if (proc && proc->address_space) {
                uint32_t pages = m->aligned_size / HW_PAGE_SIZE;
                for (uint32_t p = 0u; p < pages; p++) {
                    address_space_unmap(proc->address_space, m->virt_base + p * HW_PAGE_SIZE);
                }
            }
            hw_free_phys_pages(m->phys_base, m->aligned_size);
            m->active = false;
            return 0;
        }
    }
    return -1;
}

uint32_t ipo_hw_virt_to_phys(const void *virt_addr, uint32_t caller_pid) {
    if (!virt_addr) return 0u;
    uint32_t va = (uint32_t)(uintptr_t)virt_addr;
    process_t *proc = caller_pid ? process_find_by_pid(caller_pid) : process_get_current();
    if (proc && proc->address_space) {
        return address_space_get_physical(proc->address_space, va);
    }
    return va;
}

int ipo_pci_fill_info(uint32_t index, ipo_pci_dev_info_t *out_info) {
    if (!out_info) return -1;
    const pci_device_t *dev = pci_get_device(index);
    if (!dev) return -1;

    memset(out_info, 0, sizeof(*out_info));
    out_info->bus = dev->bus;
    out_info->slot = dev->slot;
    out_info->func = dev->func;
    out_info->irq = dev->irq;
    out_info->vendor_id = dev->vendor_id;
    out_info->device_id = dev->device_id;
    out_info->subsys_vendor_id = dev->subsys_vendor_id;
    out_info->subsys_id = dev->subsys_id;
    out_info->class_code = dev->class_code;
    out_info->subclass = dev->subclass;
    out_info->prog_if = dev->prog_if;
    out_info->revision_id = dev->revision_id;
    out_info->header_type = dev->header_type;

    uint32_t cnt = (uint32_t)dev->bar_count;
    if (cnt > 6u) cnt = 6u;
    out_info->bar_count = (uint8_t)cnt;
    if (dev->bar && cnt > 0u) {
        for (uint32_t i = 0; i < cnt; i++) {
            out_info->bars[i] = dev->bar[i];
        }
    } else {
        out_info->bars[0] = dev->bar0;
        if (dev->bar0 != 0u) out_info->bar_count = 1u;
    }
    return 0;
}

int ipo_device_fill_info(uint32_t index, ipo_device_info_t *out_info) {
    if (!out_info) return -1;
    const device_node_t *curr = device_get_list();
    uint32_t idx = 0u;
    while (curr && idx < index) {
        curr = curr->next;
        idx++;
    }
    if (!curr) return -1;

    memset(out_info, 0, sizeof(*out_info));
    out_info->id = curr->id;
    out_info->parent_id = curr->parent_id;
    out_info->capabilities = curr->capabilities;
    out_info->type = (uint32_t)curr->type;
    out_info->state = (uint32_t)curr->state;
    out_info->bus_type = (uint32_t)curr->res.bus_type;
    out_info->pci_bus = curr->res.pci_bus;
    out_info->pci_slot = curr->res.pci_slot;
    out_info->pci_func = curr->res.pci_func;
    out_info->irq = curr->res.irq;
    out_info->vendor_id = curr->res.vendor_id;
    out_info->device_id = curr->res.device_id;
    out_info->io_base = curr->res.io_base;
    out_info->io_size = curr->res.io_size;
    out_info->mmio_base = curr->res.mmio_base;
    out_info->mmio_size = curr->res.mmio_size;
    out_info->size_bytes = curr->res.size_bytes;

    if (curr->name) strncpy(out_info->name, curr->name, sizeof(out_info->name) - 1u);
    if (curr->vendor) strncpy(out_info->vendor, curr->vendor, sizeof(out_info->vendor) - 1u);
    if (curr->model) strncpy(out_info->model, curr->model, sizeof(out_info->model) - 1u);
    if (curr->description) strncpy(out_info->description, curr->description, sizeof(out_info->description) - 1u);
    if (curr->driver_name) strncpy(out_info->driver_name, curr->driver_name, sizeof(out_info->driver_name) - 1u);
    return 0;
}

uint32_t ipo_device_register_user(const ipo_device_info_t *info, uint32_t owner_pid) {
    if (!info || info->name[0] == '\0') return 0u;

    device_node_t node;
    device_node_init(&node);
    node.parent_id = info->parent_id;
    node.capabilities = info->capabilities;
    node.type = (info->type <= DEVICE_TYPE_USB) ? (device_type_t)info->type : DEVICE_TYPE_UNKNOWN;
    node.state = (info->state <= DEVICE_STATE_ERROR) ? (device_state_t)info->state : DEVICE_STATE_ACTIVE;
    node.res.bus_type = (info->bus_type <= BUS_TYPE_VIRTUAL) ? (bus_type_t)info->bus_type : BUS_TYPE_VIRTUAL;
    node.res.pci_bus = info->pci_bus;
    node.res.pci_slot = info->pci_slot;
    node.res.pci_func = info->pci_func;
    node.res.irq = info->irq;
    node.res.vendor_id = info->vendor_id;
    node.res.device_id = info->device_id;
    node.res.io_base = info->io_base;
    node.res.io_size = info->io_size;
    node.res.mmio_base = info->mmio_base;
    node.res.mmio_size = info->mmio_size;
    node.res.size_bytes = info->size_bytes;

    device_node_set_name(&node, info->name);
    if (info->vendor[0]) device_node_set_vendor(&node, info->vendor);
    if (info->model[0]) device_node_set_model(&node, info->model);
    if (info->description[0]) device_node_set_description(&node, info->description);
    if (info->driver_name[0]) device_node_set_driver_name(&node, info->driver_name);

    uint32_t slot = s_user_dev_cap;
    for (uint32_t i = 0; i < s_user_dev_cap; i++) {
        if (!s_user_devs[i].active) { slot = i; break; }
    }
    if (slot == s_user_dev_cap) {
        uint32_t nc = s_user_dev_cap ? (s_user_dev_cap * 2u) : 8u;
        user_dev_entry_t *nb = (user_dev_entry_t *)krealloc(s_user_devs, nc * sizeof(user_dev_entry_t));
        if (!nb) {
            device_node_free(&node);
            return 0u;
        }
        memset(&nb[s_user_dev_cap], 0, (nc - s_user_dev_cap) * sizeof(user_dev_entry_t));
        s_user_devs = nb;
        s_user_dev_cap = nc;
    }

    device_node_t *reg = device_register(&node);
    device_node_free(&node);
    if (!reg) return 0u;

    s_user_devs[slot].active = true;
    s_user_devs[slot].dev_id = reg->id;
    s_user_devs[slot].owner_pid = owner_pid;
    return reg->id;
}

int ipo_device_unregister_user(uint32_t dev_id, uint32_t caller_pid) {
    if (dev_id == 0u) return -1;
    for (uint32_t i = 0; i < s_user_dev_cap; i++) {
        if (s_user_devs[i].active && s_user_devs[i].dev_id == dev_id) {
            if (caller_pid != 0u && s_user_devs[i].owner_pid != caller_pid) {
                return -2;
            }
            s_user_devs[i].active = false;
            return device_unregister(dev_id);
        }
    }
    if (caller_pid == 0u) {
        return device_unregister(dev_id);
    }
    return -1;
}

void input_ext_init(void) {
    s_pressed_count = 0u;
    s_ext_modifiers = 0u;
}

void input_ext_cleanup_pid(uint32_t pid) {
    if (pid == 0u) return;

    irq_cleanup_pid(pid);

    for (uint32_t i = 0; i < s_shortcut_cap; i++) {
        if (s_shortcuts[i].active && s_shortcuts[i].owner_pid == pid) {
            s_shortcuts[i].active = false;
            if (s_shortcuts[i].keys) {
                kfree(s_shortcuts[i].keys);
                s_shortcuts[i].keys = NULL;
            }
        }
    }

    for (uint32_t i = 0; i < s_shortcut_queue_cap; i++) {
        if (s_shortcut_queues[i].active && s_shortcut_queues[i].pid == pid) {
            s_shortcut_queues[i].active = false;
            if (s_shortcut_queues[i].ring) {
                kfree(s_shortcut_queues[i].ring);
                s_shortcut_queues[i].ring = NULL;
            }
            s_shortcut_queues[i].cap = 0u;
        }
    }

    for (uint32_t i = 0; i < s_key_sub_cap; i++) {
        if (s_key_subs[i].active && s_key_subs[i].pid == pid) {
            s_key_subs[i].active = false;
            if (s_key_subs[i].ring) {
                kfree(s_key_subs[i].ring);
                s_key_subs[i].ring = NULL;
            }
            s_key_subs[i].cap = 0u;
        }
    }

    for (uint32_t i = 0; i < s_input_dev_cap; i++) {
        if (s_input_devs[i].active && s_input_devs[i].owner_pid == pid) {
            free_input_dev_slot(&s_input_devs[i]);
        }
    }

    for (uint32_t i = 0; i < s_input_ev_queue_cap; i++) {
        if (s_input_ev_queues[i].active && s_input_ev_queues[i].pid == pid) {
            s_input_ev_queues[i].active = false;
            if (s_input_ev_queues[i].ring) {
                kfree(s_input_ev_queues[i].ring);
                s_input_ev_queues[i].ring = NULL;
            }
            s_input_ev_queues[i].cap = 0u;
        }
    }

    for (uint32_t i = 0; i < s_ps2_hook_cap; i++) {
        if (s_ps2_hooks[i].active && s_ps2_hooks[i].owner_pid == pid) {
            s_ps2_hooks[i].active = false;
            if (s_ps2_hooks[i].ring) {
                kfree(s_ps2_hooks[i].ring);
                s_ps2_hooks[i].ring = NULL;
            }
            s_ps2_hooks[i].cap = 0u;
        }
    }

    process_t *proc = process_find_by_pid(pid);
    for (uint32_t i = 0; i < s_hw_map_cap; i++) {
        hw_map_entry_t *m = &s_hw_maps[i];
        if (m->active && m->owner_pid == pid) {
            if (proc && proc->address_space) {
                uint32_t pages = m->aligned_size / HW_PAGE_SIZE;
                for (uint32_t p = 0u; p < pages; p++) {
                    address_space_unmap(proc->address_space, m->virt_base + p * HW_PAGE_SIZE);
                }
            }
            if (m->is_dma) {
                hw_free_phys_pages(m->phys_base, m->aligned_size);
            }
            m->active = false;
        }
    }

    for (uint32_t i = 0; i < s_user_dev_cap; i++) {
        if (s_user_devs[i].active && s_user_devs[i].owner_pid == pid) {
            device_unregister(s_user_devs[i].dev_id);
            s_user_devs[i].active = false;
        }
    }
}
