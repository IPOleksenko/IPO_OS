#ifndef KERNEL_INPUT_EXT_H
#define KERNEL_INPUT_EXT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Extended 32-bit Keycode Encoding
 * - 0x00000001 .. 0x0000007F : Set-1 single-byte scancodes (known or unknown)
 * - 0x0000E000 .. 0x0000E07F : 0xE0-prefixed extended scancodes (known or unknown)
 * - 0x00E10000 .. 0x00E17F7F : 0xE1 multi-byte scancodes
 * - 0x00010000 .. 0x7FFFFFFF : External driver / vendor-defined button codes
 */
#define IPO_KEY_MAKE_EXT_E0(sc)    (0x0000E000u | ((uint32_t)(sc) & 0x7Fu))
#define IPO_KEY_MAKE_EXT_E1(b1,b2) (0x00E10000u | (((uint32_t)(b1) & 0x7Fu) << 8) | ((uint32_t)(b2) & 0x7Fu))

#define IPO_KEY_FLAG_BREAK         (1u << 0)
#define IPO_KEY_FLAG_EXT_E0        (1u << 1)
#define IPO_KEY_FLAG_EXT_E1        (1u << 2)
#define IPO_KEY_FLAG_REPEAT        (1u << 3)
#define IPO_KEY_FLAG_INJECTED      (1u << 4)

#define IPO_MOD_SHIFT              (1u << 0)
#define IPO_MOD_CTRL               (1u << 1)
#define IPO_MOD_ALT                (1u << 2)
#define IPO_MOD_SUPER              (1u << 3)
#define IPO_MOD_CAPS               (1u << 4)
#define IPO_MOD_NUM                (1u << 5)

/* Shortcut Registration Flags */
#define IPO_SHORTCUT_FLAG_GLOBAL        (1u << 0) /* Match regardless of focused window/process */
#define IPO_SHORTCUT_FLAG_CONSUME       (1u << 1) /* Suppress default key/WM dispatch on match */
#define IPO_SHORTCUT_FLAG_EXACT_MODS    (1u << 2) /* Match exact IPO_MOD_{SHIFT,CTRL,ALT,SUPER} bits */
#define IPO_SHORTCUT_FLAG_ON_RELEASE    (1u << 3) /* Trigger when chord is released instead of pressed */
#define IPO_SHORTCUT_FLAG_PASSIVE       (1u << 4) /* Observe without consuming */
#define IPO_SHORTCUT_FLAG_ALLOW_REPEAT  (1u << 5) /* Re-trigger on repeated key presses while held */

/* Generic Input Event Types & Capabilities */
#define IPO_EV_SYN                 0x00u
#define IPO_EV_KEY                 0x01u
#define IPO_EV_REL                 0x02u
#define IPO_EV_ABS                 0x03u
#define IPO_EV_RAW                 0x04u

#define IPO_INPUT_CAP_KEY              (1u << 0)
#define IPO_INPUT_CAP_REL              (1u << 1)
#define IPO_INPUT_CAP_ABS              (1u << 2)
#define IPO_INPUT_CAP_KEYBOARD_ROUTE   (1u << 3)
#define IPO_INPUT_CAP_RAW              (1u << 4)

/* PS/2 Hook Channels, Flags & Transact Ports */
#define IPO_PS2_CHAN_KBD           (1u << 0)
#define IPO_PS2_CHAN_AUX           (1u << 1)

#define IPO_PS2_HOOK_OBSERVE       0x00u
#define IPO_PS2_HOOK_EXCLUSIVE     (1u << 0)

#define IPO_PS2_PORT_CTRL          0u
#define IPO_PS2_PORT_KBD           1u
#define IPO_PS2_PORT_AUX           2u

/* MMIO Mapping Flags */
#define IPO_MMIO_CACHE_DISABLE     (1u << 0)
#define IPO_MMIO_WRITE_THROUGH     (1u << 1)

typedef struct {
    uint32_t keycode;
    uint32_t modifiers;
    uint32_t flags;
    uint32_t source_dev_id;
    uint64_t timestamp_ms;
    uint8_t  raw_len;
    uint8_t  raw_bytes[4];
    uint8_t  reserved[3];
} ipo_key_event_t;

typedef struct {
    uint32_t id;
    uint32_t owner_pid;
    uint32_t window_handle;
    uint32_t mod_mask;
    uint32_t key_count;
    const uint32_t *keys;
    uint32_t flags;
    uint32_t user_tag;
} ipo_shortcut_spec_t;

typedef struct {
    uint32_t shortcut_id;
    uint32_t user_tag;
    uint32_t trigger_keycode;
    uint32_t modifiers;
    uint32_t window_handle;
    uint32_t reserved;
    uint64_t timestamp_ms;
} ipo_shortcut_event_t;

typedef struct {
    char     name[48];
    uint16_t bus_type;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version;
    uint32_t caps;
} ipo_input_dev_desc_t;

typedef struct {
    uint32_t id;
    uint32_t owner_pid;
    uint32_t device_node_id;
    uint32_t caps;
    uint16_t bus_type;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version;
    uint32_t event_count;
    char     name[48];
} ipo_input_dev_info_t;

typedef struct {
    uint32_t dev_id;
    uint16_t type;
    uint16_t flags;
    uint32_t code;
    int32_t  value;
    uint64_t timestamp_ms;
} ipo_input_event_t;

typedef struct {
    uint32_t chan_mask;
    uint8_t  byte;
    uint8_t  chan;
    uint8_t  reserved[2];
    uint64_t timestamp_ms;
} ipo_ps2_byte_event_t;

typedef struct {
    uint8_t  cmd;
    uint8_t  port_id;
    uint8_t  param_len;
    uint8_t  reply_len;
    const uint8_t *params;
    uint8_t *reply;
    uint32_t flags;
} ipo_ps2_transact_req_t;

typedef struct {
    uint8_t  bus, slot, func, irq;
    uint16_t vendor_id, device_id;
    uint16_t subsys_vendor_id, subsys_id;
    uint8_t  class_code, subclass, prog_if, revision_id;
    uint8_t  header_type, bar_count, reserved[2];
    uint32_t bars[6];
} ipo_pci_dev_info_t;

typedef struct {
    uint32_t id;
    uint32_t parent_id;
    uint32_t capabilities;
    uint32_t type;
    uint32_t state;
    uint32_t bus_type;
    uint8_t  pci_bus, pci_slot, pci_func, irq;
    uint16_t vendor_id, device_id;
    uint16_t io_base, io_size;
    uint32_t mmio_base, mmio_size;
    uint64_t size_bytes;
    char     name[64];
    char     vendor[64];
    char     model[64];
    char     description[96];
    char     driver_name[48];
} ipo_device_info_t;

/* Set-1 multi-byte hardware scancode decoder (reused by keyboard and WM) */
typedef struct {
    uint8_t state;
    uint8_t len;
    uint8_t e1_b1;
    uint8_t raw_buf[4];
} ipo_key_decoder_t;

void input_ext_init(void);
void input_ext_cleanup_pid(uint32_t pid);
void input_ext_clear_key_state(void);

void ipo_key_decoder_reset(ipo_key_decoder_t *dec);
bool ipo_key_decoder_feed(ipo_key_decoder_t *dec, uint8_t raw_byte, ipo_key_event_t *out_ev);

/* Key processing & extended query */
bool ipo_input_process_key_event(const ipo_key_event_t *in_ev,
                                 uint32_t wm_window_handle,
                                 uint32_t target_pid,
                                 bool in_wm,
                                 bool *out_consumed);
bool ipo_input_is_key_down(uint32_t keycode);
uint32_t ipo_input_get_modifiers(void);
bool ipo_input_poll_key_event(uint32_t caller_pid, ipo_key_event_t *out_ev);

/* Custom Shortcuts */
uint32_t ipo_shortcut_register(const ipo_shortcut_spec_t *spec, uint32_t owner_pid);
int ipo_shortcut_unregister(uint32_t shortcut_id, uint32_t caller_pid);
bool ipo_shortcut_poll(uint32_t caller_pid, ipo_shortcut_event_t *out_ev);

/* Generic Input Device Registry & Events */
uint32_t ipo_input_dev_register(const ipo_input_dev_desc_t *desc, uint32_t owner_pid);
int ipo_input_dev_unregister(uint32_t dev_id, uint32_t caller_pid);
int ipo_input_dev_emit(uint32_t dev_id, const ipo_input_event_t *ev, uint32_t caller_pid);
uint32_t ipo_input_dev_list(ipo_input_dev_info_t *out_arr, uint32_t max_count);
bool ipo_input_dev_poll_event(uint32_t caller_pid, uint32_t dev_filter_id, ipo_input_event_t *out_ev);
int ipo_input_dev_get_state(uint32_t dev_id, uint16_t type, uint32_t code, int32_t *out_val);

/* Raw PS/2 Byte Stream Hooks & Low-Level Controller Transactions */
uint32_t ipo_ps2_hook_register(uint32_t chan_mask, uint32_t flags, uint32_t owner_pid);
int ipo_ps2_hook_unregister(uint32_t handle, uint32_t caller_pid);
uint32_t ipo_ps2_hook_poll(uint32_t handle, uint32_t caller_pid,
                           ipo_ps2_byte_event_t *out_events, uint32_t max_count);
bool ipo_ps2_dispatch_byte(bool is_aux, uint8_t data);
int ipo_ps2_transact(const ipo_ps2_transact_req_t *req);

/* Low-Level Hardware Memory / Bus Helpers */
void *ipo_hw_map_mmio(uint32_t phys_addr, uint32_t size, uint32_t flags, uint32_t owner_pid);
int ipo_hw_unmap_mmio(void *virt_addr, uint32_t size, uint32_t caller_pid);
void *ipo_hw_alloc_dma(uint32_t size, uint32_t alignment, uint32_t *out_phys, uint32_t owner_pid);
int ipo_hw_free_dma(void *virt_addr, uint32_t caller_pid);
uint32_t ipo_hw_virt_to_phys(const void *virt_addr, uint32_t caller_pid);
bool ipo_hw_user_range(uint32_t pid, const void *ptr, uint32_t length);

int ipo_pci_fill_info(uint32_t index, ipo_pci_dev_info_t *out_info);
int ipo_device_fill_info(uint32_t index, ipo_device_info_t *out_info);
uint32_t ipo_device_register_user(const ipo_device_info_t *info, uint32_t owner_pid);
int ipo_device_unregister_user(uint32_t dev_id, uint32_t caller_pid);

#endif /* KERNEL_INPUT_EXT_H */
