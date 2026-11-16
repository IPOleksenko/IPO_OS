#ifndef KERNEL_DEVICE_H
#define KERNEL_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <types.h>
#include <kernel/smp.h>

typedef enum {
    DEVICE_TYPE_UNKNOWN = 0,
    DEVICE_TYPE_CPU,
    DEVICE_TYPE_MEMORY,
    DEVICE_TYPE_PCI,
    DEVICE_TYPE_STORAGE,
    DEVICE_TYPE_NETWORK,
    DEVICE_TYPE_INPUT,
    DEVICE_TYPE_AUDIO,
    DEVICE_TYPE_VIDEO,
    DEVICE_TYPE_SYSTEM,
    DEVICE_TYPE_USB
} device_type_t;

typedef enum {
    DEVICE_STATE_DISCOVERED = 0,
    DEVICE_STATE_DRIVER_ATTACHED,
    DEVICE_STATE_INITIALIZED,
    DEVICE_STATE_ACTIVE,
    DEVICE_STATE_DISABLED,
    DEVICE_STATE_ERROR
} device_state_t;

typedef enum {
    BUS_TYPE_INTERNAL = 0,
    BUS_TYPE_PCI,
    BUS_TYPE_ISA,
    BUS_TYPE_IDE,
    BUS_TYPE_USB,
    BUS_TYPE_UART,
    BUS_TYPE_VIRTUAL
} bus_type_t;

#define DEVICE_CAP_READONLY     (1 << 0)
#define DEVICE_CAP_REMOVABLE    (1 << 1)
#define DEVICE_CAP_HOTPLUGGABLE (1 << 2)
#define DEVICE_CAP_DMA          (1 << 3)
#define DEVICE_CAP_VIRTUAL      (1 << 4)

struct device_node;

typedef struct device_ops {
    int (*open)(struct device_node *dev);
    int (*close)(struct device_node *dev);
    int (*read)(struct device_node *dev, void *buf, size_t size, uint64_t offset);
    int (*write)(struct device_node *dev, const void *buf, size_t size, uint64_t offset);
    int (*ioctl)(struct device_node *dev, uint32_t cmd, void *arg);
} device_ops_t;

typedef struct {
    bus_type_t bus_type;
    uint8_t    pci_bus;
    uint8_t    pci_slot;
    uint8_t    pci_func;
    uint16_t   vendor_id;
    uint16_t   device_id;
    uint8_t    pci_class;
    uint8_t    pci_subclass;
    uint8_t    pci_progif;
    ipo_paddr_t *bar;
    size_t     bar_count;
    size_t     bar_cap;
    uint16_t   io_base;
    uint16_t   io_size;
    ipo_paddr_t mmio_base;
    ipo_size_t  mmio_size;
    uint8_t    irq;
    uint64_t   size_bytes;           /* storage/memory capacity */
} device_resources_t;

/* Device node (heap-allocated strings; registry protected by spinlock) */
typedef struct device_node {
    uint32_t           id;
    uint32_t           parent_id;     /* 0 if root */
    uint32_t           capabilities;  /* DEVICE_CAP_* */
    device_type_t      type;
    device_state_t     state;
    char               *name;         /* e.g. "cpu0", "pci00:01.0" */
    size_t             name_len;
    size_t             name_cap;
    char               *vendor;
    size_t             vendor_len;
    size_t             vendor_cap;
    char               *model;
    size_t             model_len;
    size_t             model_cap;
    char               *description;
    size_t             description_len;
    size_t             description_cap;
    device_resources_t res;
    char               *driver_name;
    size_t             driver_name_len;
    size_t             driver_name_cap;
    void              *driver_ptr;    /* attached driver_t* */
    const device_ops_t *ops;
    void              *priv_data;
    struct device_node *next;
} device_node_t;

void device_resources_init(device_resources_t *res);
void device_resources_free(device_resources_t *res);
int  device_resources_add_bar(device_resources_t *res, ipo_paddr_t bar);
int  device_resources_copy(device_resources_t *dst, const device_resources_t *src);

void device_node_init(device_node_t *dev);
void device_node_free(device_node_t *dev);
int  device_node_set_name(device_node_t *dev, const char *name);
int  device_node_set_vendor(device_node_t *dev, const char *vendor);
int  device_node_set_model(device_node_t *dev, const char *model);
int  device_node_set_description(device_node_t *dev, const char *desc);
int  device_node_set_driver_name(device_node_t *dev, const char *driver);
int  device_node_copy(device_node_t *dst, const device_node_t *src);

void device_init(void);

device_node_t *device_register(const device_node_t *dev);
int device_unregister(uint32_t id);
device_node_t *device_find_by_id(uint32_t id);
device_node_t *device_find_by_name(const char *name);
int device_set_state(uint32_t id, device_state_t state);
int device_set_driver(uint32_t id, const char *driver_name, void *driver_ptr);

uint32_t device_get_count(void);
uint32_t device_get_count_by_type(device_type_t type);
uint32_t device_get_count_by_state(device_state_t state);
const device_node_t *device_get_list(void);

int device_rescan(void);

const char *device_type_to_string(device_type_t type);
const char *device_state_to_string(device_state_t state);
const char *bus_type_to_string(bus_type_t bus);

#endif /* KERNEL_DEVICE_H */

