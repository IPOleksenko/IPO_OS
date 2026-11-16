#include <kernel/ext.h>
#include <kernel/smp.h>
#include <kernel/process.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

static char *ext_strdup_local(const char *s) {
    if (!s) s = "";
    size_t len = strlen(s) + 1u;
    char *p = (char *)kmalloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

static void copy_str_to_caller_buf(const char *src, char *dst, uint32_t cap, uint32_t *out_len) {
    uint32_t slen = src ? (uint32_t)strlen(src) : 0u;
    if (out_len) *out_len = slen;
    if (dst && cap > 0u) {
        uint32_t n = (slen < cap - 1u) ? slen : (cap - 1u);
        if (n > 0u && src) memcpy(dst, src, n);
        dst[n] = '\0';
    }
}

/* System extension registry */

typedef struct ipo_ext_entry {
    bool                active;
    uint32_t            id;
    uint32_t            owner_pid;
    uint32_t            flags;
    uint32_t            call_count;
    uint32_t            update_count;
    char                *name;
    char                *category;
    char                *description;
    ipo_ext_invoke_fn_t invoke;
    void                *ops_table;
    uint32_t            ops_size;
    void                *priv_data;
} ipo_ext_entry_t;

static ipo_ext_entry_t *s_exts = NULL;
static uint32_t         s_ext_cap = 0u;
static uint32_t         s_next_ext_id = 1u;
static spinlock_t       s_ext_lock = {0};
static bool             s_ext_inited = false;

static spinlock_t       s_chan_lock = {0};

static void ext_entry_free_strings(ipo_ext_entry_t *e) {
    if (!e) return;
    if (e->name) { kfree(e->name); e->name = NULL; }
    if (e->category) { kfree(e->category); e->category = NULL; }
    if (e->description) { kfree(e->description); e->description = NULL; }
}

void ipo_ext_init(void) {
    if (s_ext_inited) return;
    spinlock_init(&s_ext_lock);
    spinlock_init(&s_chan_lock);
    s_ext_inited = true;
}

uint32_t ipo_ext_register(const ipo_ext_desc_t *desc, uint32_t owner_pid) {
    if (!desc || !desc->name || desc->name[0] == '\0') return 0u;
    if (!s_ext_inited) ipo_ext_init();

    char *name_copy = ext_strdup_local(desc->name);
    char *cat_copy  = ext_strdup_local(desc->category);
    char *desc_copy = ext_strdup_local(desc->description);
    if (!name_copy || !cat_copy || !desc_copy) {
        if (name_copy) kfree(name_copy);
        if (cat_copy)  kfree(cat_copy);
        if (desc_copy) kfree(desc_copy);
        return 0u;
    }

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_ext_lock);

    for (uint32_t i = 0; i < s_ext_cap; i++) {
        if (s_exts[i].active && s_exts[i].name && strcmp(s_exts[i].name, desc->name) == 0) {
            spin_unlock_irqrestore(&s_ext_lock, irq_flags);
            kfree(name_copy);
            kfree(cat_copy);
            kfree(desc_copy);
            return 0u;
        }
    }

    uint32_t slot = s_ext_cap;
    for (uint32_t i = 0; i < s_ext_cap; i++) {
        if (!s_exts[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == s_ext_cap) {
        uint32_t ncap = s_ext_cap ? (s_ext_cap * 2u) : 16u;
        ipo_ext_entry_t *narr = (ipo_ext_entry_t *)krealloc(s_exts, (size_t)ncap * sizeof(ipo_ext_entry_t));
        if (!narr) {
            spin_unlock_irqrestore(&s_ext_lock, irq_flags);
            kfree(name_copy);
            kfree(cat_copy);
            kfree(desc_copy);
            return 0u;
        }
        memset(&narr[s_ext_cap], 0, (size_t)(ncap - s_ext_cap) * sizeof(ipo_ext_entry_t));
        s_exts = narr;
        s_ext_cap = ncap;
    }

    ipo_ext_entry_t *e = &s_exts[slot];
    memset(e, 0, sizeof(*e));
    e->active       = true;
    e->id           = s_next_ext_id++;
    e->owner_pid    = owner_pid;
    e->flags        = desc->flags | IPO_EXT_FLAG_ACTIVE;
    e->call_count   = 0u;
    e->update_count = 0u;
    e->name         = name_copy;
    e->category     = cat_copy;
    e->description  = desc_copy;
    e->invoke       = desc->invoke;
    e->ops_table    = desc->ops_table;
    e->ops_size     = desc->ops_size;
    e->priv_data    = desc->priv_data;

    uint32_t id = e->id;
    spin_unlock_irqrestore(&s_ext_lock, irq_flags);

    serial_printf("[ext] Registered extension '%s' (id=%u, cat='%s', owner_pid=%u)\n",
                  name_copy, id, cat_copy, owner_pid);
    return id;
}

int ipo_ext_unregister(uint32_t ext_id, uint32_t caller_pid) {
    if (ext_id == 0u || !s_ext_inited) return -1;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_ext_lock);

    for (uint32_t i = 0; i < s_ext_cap; i++) {
        if (s_exts[i].active && s_exts[i].id == ext_id) {
            if (caller_pid != 0u && s_exts[i].owner_pid != 0u && s_exts[i].owner_pid != caller_pid) {
                spin_unlock_irqrestore(&s_ext_lock, irq_flags);
                return -2; /* Permission denied */
            }
            serial_printf("[ext] Unregistered extension '%s' (id=%u)\n",
                          s_exts[i].name ? s_exts[i].name : "", ext_id);
            ext_entry_free_strings(&s_exts[i]);
            memset(&s_exts[i], 0, sizeof(s_exts[i]));
            spin_unlock_irqrestore(&s_ext_lock, irq_flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&s_ext_lock, irq_flags);
    return -3; /* Not found */
}

int ipo_ext_unregister_by_name(const char *name, uint32_t caller_pid) {
    if (!name || !*name || !s_ext_inited) return -1;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_ext_lock);

    for (uint32_t i = 0; i < s_ext_cap; i++) {
        if (s_exts[i].active && s_exts[i].name && strcmp(s_exts[i].name, name) == 0) {
            if (caller_pid != 0u && s_exts[i].owner_pid != 0u && s_exts[i].owner_pid != caller_pid) {
                spin_unlock_irqrestore(&s_ext_lock, irq_flags);
                return -2;
            }
            serial_printf("[ext] Unregistered extension '%s' (id=%u)\n", s_exts[i].name, s_exts[i].id);
            ext_entry_free_strings(&s_exts[i]);
            memset(&s_exts[i], 0, sizeof(s_exts[i]));
            spin_unlock_irqrestore(&s_ext_lock, irq_flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&s_ext_lock, irq_flags);
    return -3;
}

int ipo_ext_update(uint32_t ext_id, const ipo_ext_desc_t *new_desc, uint32_t caller_pid) {
    if (ext_id == 0u || !new_desc || !s_ext_inited) return -1;

    char *new_desc_str = (new_desc->description && new_desc->description[0])
                         ? ext_strdup_local(new_desc->description)
                         : NULL;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_ext_lock);

    for (uint32_t i = 0; i < s_ext_cap; i++) {
        if (s_exts[i].active && s_exts[i].id == ext_id) {
            if (caller_pid != 0u && s_exts[i].owner_pid != 0u && s_exts[i].owner_pid != caller_pid) {
                spin_unlock_irqrestore(&s_ext_lock, irq_flags);
                if (new_desc_str) kfree(new_desc_str);
                return -2;
            }
            if (new_desc_str) {
                if (s_exts[i].description) kfree(s_exts[i].description);
                s_exts[i].description = new_desc_str;
            }
            if (new_desc->invoke) s_exts[i].invoke = new_desc->invoke;
            if (new_desc->ops_table) {
                s_exts[i].ops_table = new_desc->ops_table;
                s_exts[i].ops_size  = new_desc->ops_size;
            }
            if (new_desc->priv_data) s_exts[i].priv_data = new_desc->priv_data;
            s_exts[i].update_count++;
            spin_unlock_irqrestore(&s_ext_lock, irq_flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&s_ext_lock, irq_flags);
    if (new_desc_str) kfree(new_desc_str);
    return -3;
}

int ipo_ext_call(const char *name, uint32_t op, ipo_uarg_t a0, ipo_uarg_t a1,
                 ipo_uarg_t a2, ipo_uarg_t a3) {
    if (!name || !*name || !s_ext_inited) return -1;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_ext_lock);
    ipo_ext_invoke_fn_t fn = NULL;
    void *priv = NULL;
    uint32_t owner_pid = 0u;

    for (uint32_t i = 0; i < s_ext_cap; i++) {
        if (s_exts[i].active && s_exts[i].name && strcmp(s_exts[i].name, name) == 0) {
            fn = s_exts[i].invoke;
            priv = s_exts[i].priv_data;
            owner_pid = s_exts[i].owner_pid;
            s_exts[i].call_count++;
            break;
        }
    }
    spin_unlock_irqrestore(&s_ext_lock, irq_flags);

    if (!fn) return -2; /* Extension not found or has no invoke handler */

    if (owner_pid != 0u) {
        process_t *owner = process_find_by_pid(owner_pid);
        if (!owner || !process_is_valid(owner)) return -3;
        process_t *prev_mapped = process_get_mapped_app();
        process_t *prev_curr   = process_get_current();
        process_map_app(owner);
        process_set_current(owner);
        int rc = fn(op, a0, a1, a2, a3, priv);
        process_map_app(prev_mapped && process_is_valid(prev_mapped) ? prev_mapped : NULL);
        process_set_current(prev_curr && process_is_valid(prev_curr) ? prev_curr : NULL);
        return rc;
    }

    return fn(op, a0, a1, a2, a3, priv);
}

void *ipo_ext_get_ops(const char *name, uint32_t *out_size) {
    if (!name || !*name || !s_ext_inited) return NULL;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_ext_lock);
    void *ops = NULL;
    uint32_t sz = 0u;
    for (uint32_t i = 0; i < s_ext_cap; i++) {
        if (s_exts[i].active && s_exts[i].name && strcmp(s_exts[i].name, name) == 0) {
            ops = s_exts[i].ops_table;
            sz = s_exts[i].ops_size;
            break;
        }
    }
    spin_unlock_irqrestore(&s_ext_lock, irq_flags);
    if (out_size) *out_size = sz;
    return ops;
}

uint32_t ipo_ext_list(ipo_ext_info_t *out_arr, uint32_t max_count) {
    if (!s_ext_inited) return 0u;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_ext_lock);
    uint32_t count = 0u;
    for (uint32_t i = 0; i < s_ext_cap; i++) {
        if (!s_exts[i].active) continue;
        if (out_arr && count < max_count) {
            ipo_ext_info_t *dst = &out_arr[count];
            dst->id           = s_exts[i].id;
            dst->owner_pid    = s_exts[i].owner_pid;
            dst->flags        = s_exts[i].flags;
            dst->call_count   = s_exts[i].call_count;
            dst->update_count = s_exts[i].update_count;

            copy_str_to_caller_buf(s_exts[i].name, dst->name, dst->name_cap, &dst->name_len);
            copy_str_to_caller_buf(s_exts[i].category, dst->category, dst->category_cap, &dst->category_len);
            copy_str_to_caller_buf(s_exts[i].description, dst->description, dst->desc_cap, &dst->desc_len);
        }
        count++;
    }
    spin_unlock_irqrestore(&s_ext_lock, irq_flags);
    return count;
}

/* Hardware data channel and frame bus */

typedef struct {
    uint32_t len;
    uint32_t cap;
    uint8_t  *data;
} chan_frame_slot_t;

typedef struct {
    bool               active;
    uint32_t           id;
    uint32_t           chan_id;
    uint32_t           owner_pid;
    uint16_t           ethertype_filter;
    ipo_chan_rx_cb_t   cb;
    void              *ctx;
    chan_frame_slot_t *queue;
    uint32_t           q_cap;
    uint32_t           q_head;
    uint32_t           q_tail;
    uint32_t           q_count;
} chan_sub_entry_t;

typedef struct {
    bool            active;
    uint32_t        id;
    uint32_t        owner_pid;
    char           *name;
    uint8_t        *hw_addr;
    uint32_t        hw_addr_len;
    ipo_chan_desc_t desc;
    uint32_t        tx_packets;
    uint32_t        rx_packets;
    uint32_t        tx_bytes;
    uint32_t        rx_bytes;
    uint32_t        tx_errors;
    uint32_t        rx_drops;
} chan_entry_t;

static chan_entry_t     *s_chans = NULL;
static uint32_t          s_chan_cap = 0u;
static uint32_t          s_next_chan_id = 1u;

static chan_sub_entry_t *s_subs = NULL;
static uint32_t          s_sub_cap = 0u;
static uint32_t          s_next_sub_id = 1u;

static void sub_free_queue(chan_sub_entry_t *sub) {
    if (!sub || !sub->queue) return;
    for (uint32_t i = 0; i < sub->q_cap; i++) {
        if (sub->queue[i].data) {
            kfree(sub->queue[i].data);
            sub->queue[i].data = NULL;
        }
    }
    kfree(sub->queue);
    sub->queue = NULL;
}

uint32_t ipo_chan_register(const ipo_chan_desc_t *desc, uint32_t owner_pid) {
    if (!desc || !desc->name || desc->name[0] == '\0') return 0u;
    if (!s_ext_inited) ipo_ext_init();

    char *name_copy = ext_strdup_local(desc->name);
    if (!name_copy) return 0u;

    uint8_t *hw_copy = NULL;
    if (desc->hw_addr && desc->hw_addr_len > 0u) {
        hw_copy = (uint8_t *)kmalloc(desc->hw_addr_len);
        if (!hw_copy) {
            kfree(name_copy);
            return 0u;
        }
        memcpy(hw_copy, desc->hw_addr, desc->hw_addr_len);
    }

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);

    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (s_chans[i].active && s_chans[i].name && strcmp(s_chans[i].name, desc->name) == 0) {
            spin_unlock_irqrestore(&s_chan_lock, irq_flags);
            kfree(name_copy);
            if (hw_copy) kfree(hw_copy);
            return 0u;
        }
    }

    uint32_t slot = s_chan_cap;
    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (!s_chans[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == s_chan_cap) {
        uint32_t ncap = s_chan_cap ? (s_chan_cap * 2u) : 8u;
        chan_entry_t *narr = (chan_entry_t *)krealloc(s_chans, (size_t)ncap * sizeof(chan_entry_t));
        if (!narr) {
            spin_unlock_irqrestore(&s_chan_lock, irq_flags);
            kfree(name_copy);
            if (hw_copy) kfree(hw_copy);
            return 0u;
        }
        memset(&narr[s_chan_cap], 0, (size_t)(ncap - s_chan_cap) * sizeof(chan_entry_t));
        s_chans = narr;
        s_chan_cap = ncap;
    }

    chan_entry_t *c = &s_chans[slot];
    memset(c, 0, sizeof(*c));
    c->active      = true;
    c->id          = s_next_chan_id++;
    c->owner_pid   = owner_pid;
    c->name        = name_copy;
    c->hw_addr     = hw_copy;
    c->hw_addr_len = hw_copy ? desc->hw_addr_len : 0u;
    c->desc        = *desc;
    c->desc.name   = name_copy;
    c->desc.hw_addr = hw_copy;
    if (c->desc.mtu == 0u) c->desc.mtu = 1500u;

    uint32_t id = c->id;
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);

    serial_printf("[chan] Registered hardware data channel '%s' (id=%u, mtu=%u)\n",
                  name_copy, id, c->desc.mtu);
    return id;
}

int ipo_chan_unregister(uint32_t chan_id, uint32_t caller_pid) {
    if (chan_id == 0u || !s_ext_inited) return -1;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);

    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (s_chans[i].active && s_chans[i].id == chan_id) {
            if (caller_pid != 0u && s_chans[i].owner_pid != 0u && s_chans[i].owner_pid != caller_pid) {
                spin_unlock_irqrestore(&s_chan_lock, irq_flags);
                return -2;
            }
            for (uint32_t s = 0; s < s_sub_cap; s++) {
                if (s_subs[s].active && s_subs[s].chan_id == chan_id) {
                    sub_free_queue(&s_subs[s]);
                    memset(&s_subs[s], 0, sizeof(s_subs[s]));
                }
            }
            serial_printf("[chan] Unregistered hardware data channel '%s' (id=%u)\n",
                          s_chans[i].name ? s_chans[i].name : "", chan_id);
            if (s_chans[i].name) kfree(s_chans[i].name);
            if (s_chans[i].hw_addr) kfree(s_chans[i].hw_addr);
            memset(&s_chans[i], 0, sizeof(s_chans[i]));
            spin_unlock_irqrestore(&s_chan_lock, irq_flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&s_chan_lock, irq_flags);
    return -3;
}

uint32_t ipo_chan_find_by_name(const char *name) {
    if (!name || !*name || !s_ext_inited) return 0u;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);
    uint32_t id = 0u;
    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (s_chans[i].active && s_chans[i].name && strcmp(s_chans[i].name, name) == 0) {
            id = s_chans[i].id;
            break;
        }
    }
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);
    return id;
}

static void fill_chan_info_locked(chan_entry_t *c, ipo_chan_info_t *out) {
    out->id = c->id;
    copy_str_to_caller_buf(c->name, out->name, out->name_cap, &out->name_len);
    out->mtu = c->desc.mtu;

    out->hw_addr_len = c->hw_addr_len;
    if (c->hw_addr && c->hw_addr_len > 0u && out->hw_addr && out->hw_addr_cap > 0u) {
        uint32_t n = (c->hw_addr_len < out->hw_addr_cap) ? c->hw_addr_len : out->hw_addr_cap;
        memcpy(out->hw_addr, c->hw_addr, n);
    }

    out->link_up = (c->desc.is_link_up ? c->desc.is_link_up(c->desc.priv) : true) ? 1u : 0u;
    out->pad0 = 0u;
    out->pad1 = 0u;
    out->tx_packets = c->tx_packets;
    out->rx_packets = c->rx_packets;
    out->tx_bytes   = c->tx_bytes;
    out->rx_bytes   = c->rx_bytes;
    out->tx_errors  = c->tx_errors;
    out->rx_drops   = c->rx_drops;
}

int ipo_chan_get_info(uint32_t chan_id, ipo_chan_info_t *out_info) {
    if (chan_id == 0u || !out_info || !s_ext_inited) return -1;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);
    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (s_chans[i].active && s_chans[i].id == chan_id) {
            fill_chan_info_locked(&s_chans[i], out_info);
            spin_unlock_irqrestore(&s_chan_lock, irq_flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);
    return -2;
}

uint32_t ipo_chan_list(ipo_chan_info_t *out_arr, uint32_t max_count) {
    if (!s_ext_inited) return 0u;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);
    uint32_t count = 0u;
    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (!s_chans[i].active) continue;
        if (out_arr && count < max_count) {
            fill_chan_info_locked(&s_chans[i], &out_arr[count]);
        }
        count++;
    }
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);
    return count;
}

static bool sub_ensure_slot_capacity(chan_sub_entry_t *sub) {
    if (sub->q_count < sub->q_cap) return true;
    uint32_t old_cap = sub->q_cap;
    uint32_t new_cap = old_cap ? (old_cap * 2u) : 16u;
    chan_frame_slot_t *new_q = (chan_frame_slot_t *)kmalloc((size_t)new_cap * sizeof(chan_frame_slot_t));
    if (!new_q) return false;
    memset(new_q, 0, (size_t)new_cap * sizeof(chan_frame_slot_t));
    for (uint32_t i = 0; i < sub->q_count; i++) {
        new_q[i] = sub->queue[(sub->q_head + i) % old_cap];
    }
    if (sub->queue) kfree(sub->queue);
    sub->queue  = new_q;
    sub->q_cap  = new_cap;
    sub->q_head = 0u;
    sub->q_tail = sub->q_count;
    return true;
}

void ipo_chan_deliver_rx(uint32_t chan_id, const void *frame, uint32_t len) {
    if (chan_id == 0u || !frame || len == 0u || !s_ext_inited) return;

    uint16_t ethertype = 0u;
    if (len >= 14u) {
        const uint8_t *b = (const uint8_t *)frame;
        ethertype = (uint16_t)(((uint16_t)b[12] << 8) | b[13]);
    }

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);
    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (s_chans[i].active && s_chans[i].id == chan_id) {
            s_chans[i].rx_packets++;
            s_chans[i].rx_bytes += len;
            break;
        }
    }

    for (uint32_t s = 0; s < s_sub_cap; s++) {
        chan_sub_entry_t *sub = &s_subs[s];
        if (!sub->active || sub->chan_id != chan_id) continue;
        if (sub->ethertype_filter != IPO_CHAN_FILTER_ALL && sub->ethertype_filter != ethertype) {
            continue;
        }
        if (sub_ensure_slot_capacity(sub)) {
            chan_frame_slot_t *slot = &sub->queue[sub->q_tail];
            if (len > slot->cap || !slot->data) {
                uint8_t *nb = (uint8_t *)krealloc(slot->data, len);
                if (nb) {
                    slot->data = nb;
                    slot->cap  = len;
                }
            }
            if (slot->data && slot->cap >= len) {
                slot->len = len;
                memcpy(slot->data, frame, len);
                sub->q_tail = (sub->q_tail + 1u) % sub->q_cap;
                sub->q_count++;
            }
        }
        if (sub->cb) {
            ipo_chan_rx_cb_t cb = sub->cb;
            void *ctx = sub->ctx;
            spin_unlock_irqrestore(&s_chan_lock, irq_flags);
            cb(chan_id, frame, len, ctx);
            irq_flags = spin_lock_irqsave(&s_chan_lock);
        }
    }
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);
}

int ipo_chan_transmit(uint32_t chan_id, const void *frame, uint32_t len) {
    if (chan_id == 0u || !frame || len == 0u || !s_ext_inited) {
        return -1;
    }

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);
    int (*tx_fn)(const void *, uint32_t, void *) = NULL;
    void *priv = NULL;
    uint32_t slot = s_chan_cap;

    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (s_chans[i].active && s_chans[i].id == chan_id) {
            tx_fn = s_chans[i].desc.transmit;
            priv  = s_chans[i].desc.priv;
            slot  = i;
            break;
        }
    }
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);

    if (slot == s_chan_cap || !tx_fn) return -2;

    int rc = tx_fn(frame, len, priv);
    irq_flags = spin_lock_irqsave(&s_chan_lock);
    if (slot < s_chan_cap && s_chans[slot].active) {
        if (rc >= 0) {
            s_chans[slot].tx_packets++;
            s_chans[slot].tx_bytes += len;
        } else {
            s_chans[slot].tx_errors++;
        }
    }
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);

    if (rc >= 0 && len >= 14u) {
        const uint8_t *b = (const uint8_t *)frame;
        uint16_t etype = (uint16_t)(((uint16_t)b[12] << 8) | b[13]);
        if (etype >= 0x88B5u) {
            ipo_chan_deliver_rx(chan_id, frame, len);
        }
    }

    return rc;
}

int ipo_chan_receive(uint32_t chan_id, void *buf, uint32_t max_len) {
    if (chan_id == 0u || !buf || max_len == 0u || !s_ext_inited) return -1;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);
    int (*rx_fn)(void *, uint32_t, void *) = NULL;
    void *priv = NULL;

    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (s_chans[i].active && s_chans[i].id == chan_id) {
            rx_fn = s_chans[i].desc.poll_rx;
            priv  = s_chans[i].desc.priv;
            break;
        }
    }
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);

    if (!rx_fn) return -2;
    int got = rx_fn(buf, max_len, priv);
    if (got > 0) {
        ipo_chan_deliver_rx(chan_id, buf, (uint32_t)got);
    }
    return got;
}

void ipo_chan_poll_all(void) {
    if (!s_ext_inited) return;

    static uint8_t  *s_poll_buf = NULL;
    static uint32_t  s_poll_cap = 0u;

    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (!s_chans[i].active || !s_chans[i].desc.poll_rx) continue;
        uint32_t cid = s_chans[i].id;
        int (*rx_fn)(void *, uint32_t, void *) = s_chans[i].desc.poll_rx;
        int (*peek_fn)(void *) = s_chans[i].desc.peek_rx_size;
        void *priv = s_chans[i].desc.priv;

        uint32_t need = s_chans[i].desc.mtu + 64u;
        if (peek_fn) {
            int p = peek_fn(priv);
            if (p <= 0) continue;
            if ((uint32_t)p > need) need = (uint32_t)p;
        }
        if (need < 2048u) need = 2048u;

        if (need > s_poll_cap || !s_poll_buf) {
            uint8_t *nb = (uint8_t *)krealloc(s_poll_buf, need);
            if (!nb) continue;
            s_poll_buf = nb;
            s_poll_cap = need;
        }

        int len = rx_fn(s_poll_buf, s_poll_cap, priv);
        if (len > 0) {
            ipo_chan_deliver_rx(cid, s_poll_buf, (uint32_t)len);
        }
    }
}

int ipo_chan_ioctl(uint32_t chan_id, uint32_t cmd, void *arg) {
    if (chan_id == 0u || !s_ext_inited) return -1;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);
    int (*ioctl_fn)(uint32_t, void *, void *) = NULL;
    void *priv = NULL;
    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (s_chans[i].active && s_chans[i].id == chan_id) {
            ioctl_fn = s_chans[i].desc.ioctl;
            priv     = s_chans[i].desc.priv;
            break;
        }
    }
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);
    if (!ioctl_fn) return -2;
    return ioctl_fn(cmd, arg, priv);
}

uint32_t ipo_chan_subscribe(uint32_t chan_id, uint16_t ethertype_filter,
                            ipo_chan_rx_cb_t cb, void *ctx, uint32_t owner_pid) {
    if (chan_id == 0u || !s_ext_inited) return 0u;

    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);
    bool chan_ok = false;
    for (uint32_t i = 0; i < s_chan_cap; i++) {
        if (s_chans[i].active && s_chans[i].id == chan_id) {
            chan_ok = true;
            break;
        }
    }
    if (!chan_ok) {
        spin_unlock_irqrestore(&s_chan_lock, irq_flags);
        return 0u;
    }

    uint32_t slot = s_sub_cap;
    for (uint32_t i = 0; i < s_sub_cap; i++) {
        if (!s_subs[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == s_sub_cap) {
        uint32_t ncap = s_sub_cap ? (s_sub_cap * 2u) : 8u;
        chan_sub_entry_t *narr = (chan_sub_entry_t *)krealloc(s_subs, (size_t)ncap * sizeof(chan_sub_entry_t));
        if (!narr) {
            spin_unlock_irqrestore(&s_chan_lock, irq_flags);
            return 0u;
        }
        memset(&narr[s_sub_cap], 0, (size_t)(ncap - s_sub_cap) * sizeof(chan_sub_entry_t));
        s_subs = narr;
        s_sub_cap = ncap;
    }

    uint32_t init_qcap = 16u;
    chan_frame_slot_t *q = (chan_frame_slot_t *)kmalloc((size_t)init_qcap * sizeof(chan_frame_slot_t));
    if (!q) {
        spin_unlock_irqrestore(&s_chan_lock, irq_flags);
        return 0u;
    }
    memset(q, 0, (size_t)init_qcap * sizeof(chan_frame_slot_t));

    chan_sub_entry_t *sub = &s_subs[slot];
    memset(sub, 0, sizeof(*sub));
    sub->active           = true;
    sub->id               = s_next_sub_id++;
    sub->chan_id          = chan_id;
    sub->owner_pid        = owner_pid;
    sub->ethertype_filter = ethertype_filter;
    sub->cb               = cb;
    sub->ctx              = ctx;
    sub->queue            = q;
    sub->q_cap            = init_qcap;
    uint32_t id = sub->id;

    spin_unlock_irqrestore(&s_chan_lock, irq_flags);
    return id;
}

int ipo_chan_unsubscribe(uint32_t sub_id, uint32_t caller_pid) {
    if (sub_id == 0u || !s_ext_inited) return -1;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);
    for (uint32_t i = 0; i < s_sub_cap; i++) {
        if (s_subs[i].active && s_subs[i].id == sub_id) {
            if (caller_pid != 0u && s_subs[i].owner_pid != 0u && s_subs[i].owner_pid != caller_pid) {
                spin_unlock_irqrestore(&s_chan_lock, irq_flags);
                return -2;
            }
            sub_free_queue(&s_subs[i]);
            memset(&s_subs[i], 0, sizeof(s_subs[i]));
            spin_unlock_irqrestore(&s_chan_lock, irq_flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);
    return -3;
}

int ipo_chan_poll_sub(uint32_t sub_id, void *buf, uint32_t max_len) {
    if (sub_id == 0u || !buf || max_len == 0u || !s_ext_inited) return -1;
    ipo_reg_t irq_flags = spin_lock_irqsave(&s_chan_lock);
    for (uint32_t i = 0; i < s_sub_cap; i++) {
        chan_sub_entry_t *sub = &s_subs[i];
        if (sub->active && sub->id == sub_id) {
            if (!sub->queue || sub->q_count == 0u) {
                spin_unlock_irqrestore(&s_chan_lock, irq_flags);
                return 0;
            }
            chan_frame_slot_t *slot = &sub->queue[sub->q_head];
            uint32_t copy_len = (slot->len < max_len) ? slot->len : max_len;
            if (slot->data && copy_len > 0u) {
                memcpy(buf, slot->data, copy_len);
            }
            sub->q_head = (sub->q_head + 1u) % sub->q_cap;
            sub->q_count--;
            spin_unlock_irqrestore(&s_chan_lock, irq_flags);
            return (int)copy_len;
        }
    }
    spin_unlock_irqrestore(&s_chan_lock, irq_flags);
    return -2;
}
