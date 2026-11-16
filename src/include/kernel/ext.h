#ifndef KERNEL_EXT_H
#define KERNEL_EXT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <types.h>

/* Dynamic extension and service registry */

#define IPO_EXT_FLAG_KERNEL     (1u << 0)
#define IPO_EXT_FLAG_USER       (1u << 1)
#define IPO_EXT_FLAG_ACTIVE     (1u << 2)
#define IPO_EXT_FLAG_UPDATABLE  (1u << 3)

typedef int (*ipo_ext_invoke_fn_t)(uint32_t op, ipo_uarg_t a0, ipo_uarg_t a1,
                                   ipo_uarg_t a2, ipo_uarg_t a3, void *priv);

typedef struct {
    const char          *name;
    const char          *category;
    const char          *description;
    uint32_t            flags;
    ipo_ext_invoke_fn_t invoke;
    void                *ops_table;
    uint32_t            ops_size;
    void                *priv_data;
} ipo_ext_desc_t;

typedef struct {
    uint32_t id;
    uint32_t owner_pid;
    uint32_t flags;
    uint32_t call_count;
    uint32_t update_count;
    char     *name;
    uint32_t name_cap;
    uint32_t name_len;
    char     *category;
    uint32_t category_cap;
    uint32_t category_len;
    char     *description;
    uint32_t desc_cap;
    uint32_t desc_len;
} ipo_ext_info_t;

void     ipo_ext_init(void);
uint32_t ipo_ext_register(const ipo_ext_desc_t *desc, uint32_t owner_pid);
int      ipo_ext_unregister(uint32_t ext_id, uint32_t caller_pid);
int      ipo_ext_unregister_by_name(const char *name, uint32_t caller_pid);
int      ipo_ext_update(uint32_t ext_id, const ipo_ext_desc_t *new_desc, uint32_t caller_pid);
int      ipo_ext_call(const char *name, uint32_t op, ipo_uarg_t a0, ipo_uarg_t a1,
                      ipo_uarg_t a2, ipo_uarg_t a3);
void    *ipo_ext_get_ops(const char *name, uint32_t *out_size);
uint32_t ipo_ext_list(ipo_ext_info_t *out_arr, uint32_t max_count);

/* Protocol-agnostic raw frame channel bus */

#define IPO_CHAN_FILTER_ALL     0x0000u

typedef void (*ipo_chan_rx_cb_t)(uint32_t chan_id, const void *frame,
                                 uint32_t len, void *ctx);

typedef struct {
    const char    *name;
    uint32_t      mtu;
    const uint8_t *hw_addr;
    uint32_t      hw_addr_len;
    void          *priv;

    int      (*transmit)(const void *frame, uint32_t len, void *priv);
    int      (*poll_rx)(void *buf, uint32_t max_len, void *priv);
    int      (*peek_rx_size)(void *priv);
    bool     (*is_link_up)(void *priv);
    int      (*set_hw_addr)(const uint8_t *addr, uint32_t len, void *priv);
    int      (*ioctl)(uint32_t cmd, void *arg, void *priv);
} ipo_chan_desc_t;

typedef struct {
    uint32_t id;
    char     *name;
    uint32_t name_cap;
    uint32_t name_len;
    uint32_t mtu;
    uint8_t  *hw_addr;
    uint32_t hw_addr_cap;
    uint32_t hw_addr_len;
    uint8_t  link_up;
    uint8_t  pad0;
    uint16_t pad1;
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t tx_errors;
    uint32_t rx_drops;
} ipo_chan_info_t;

uint32_t ipo_chan_register(const ipo_chan_desc_t *desc, uint32_t owner_pid);
int      ipo_chan_unregister(uint32_t chan_id, uint32_t caller_pid);
uint32_t ipo_chan_find_by_name(const char *name);
int      ipo_chan_get_info(uint32_t chan_id, ipo_chan_info_t *out_info);
uint32_t ipo_chan_list(ipo_chan_info_t *out_arr, uint32_t max_count);

int      ipo_chan_transmit(uint32_t chan_id, const void *frame, uint32_t len);
int      ipo_chan_receive(uint32_t chan_id, void *buf, uint32_t max_len);
void     ipo_chan_deliver_rx(uint32_t chan_id, const void *frame, uint32_t len);
void     ipo_chan_poll_all(void);
int      ipo_chan_ioctl(uint32_t chan_id, uint32_t cmd, void *arg);

uint32_t ipo_chan_subscribe(uint32_t chan_id, uint16_t ethertype_filter,
                            ipo_chan_rx_cb_t cb, void *ctx, uint32_t owner_pid);
int      ipo_chan_unsubscribe(uint32_t sub_id, uint32_t caller_pid);
int      ipo_chan_poll_sub(uint32_t sub_id, void *buf, uint32_t max_len);

#endif /* KERNEL_EXT_H */
