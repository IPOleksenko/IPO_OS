#ifndef KERNEL_DEVICE_H
#define KERNEL_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <kernel/smp.h>

/* Device categories / functional types */
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

/* Device operational state tracking */
typedef enum {
    DEVICE_STATE_DISCOVERED = 0,     /* Detected on bus / firmware */
    DEVICE_STATE_DRIVER_ATTACHED,    /* Software driver matched */
    DEVICE_STATE_INITIALIZED,        /* Initialized by driver/kernel */
    DEVICE_STATE_ACTIVE,             /* Online, active and serving I/O */
    DEVICE_STATE_DISABLED,           /* Disabled by user or power policy */
    DEVICE_STATE_ERROR               /* Failed hardware or driver error */
} device_state_t;

/* Bus interface types */
typedef enum {
    BUS_TYPE_INTERNAL = 0,
    BUS_TYPE_PCI,
    BUS_TYPE_ISA,
    BUS_TYPE_IDE,
    BUS_TYPE_USB,
    BUS_TYPE_UART,
    BUS_TYPE_VIRTUAL
} bus_type_t;

/* Device capability flags */
#define DEVICE_CAP_READONLY     (1 << 0)
#define DEVICE_CAP_REMOVABLE    (1 << 1)
#define DEVICE_CAP_HOTPLUGGABLE (1 << 2)
#define DEVICE_CAP_DMA          (1 << 3)
#define DEVICE_CAP_VIRTUAL      (1 << 4)

/* Forward declaration */
struct device_node;

/* Standard device operations */
typedef struct device_ops {
    int (*open)(struct device_node *dev);
    int (*close)(struct device_node *dev);
    int (*read)(struct device_node *dev, void *buf, size_t size, uint64_t offset);
    int (*write)(struct device_node *dev, const void *buf, size_t size, uint64_t offset);
    int (*ioctl)(struct device_node *dev, uint32_t cmd, void *arg);
} device_ops_t;

/* Hardware resource descriptors with self-expanding BAR array */
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
    uint32_t   *bar;
    size_t     bar_count;
    size_t     bar_cap;
    uint16_t   io_base;
    uint16_t   io_size;
    uint32_t   mmio_base;
    uint32_t   mmio_size;
    uint8_t    irq;
    uint64_t   size_bytes;           /* Capacity for storage/memory */
} device_resources_t;

/* Unified Device Node Structure (Dynamically allocated strings & buffers, spinlock synchronized) */
typedef struct device_node {
    uint32_t           id;            /* Unique system-wide device ID */
    uint32_t           parent_id;     /* Parent controller/bus device ID (0 if root) */
    uint32_t           capabilities;  /* Capability bitmask (DEVICE_CAP_*) */
    device_type_t      type;          /* Device category */
    device_state_t     state;         /* Current operational state */
    char               *name;         /* Canonical identifier, e.g. "cpu0", "pci00:01.0" */
    size_t             name_len;
    size_t             name_cap;
    char               *vendor;       /* Hardware vendor name */
    size_t             vendor_len;
    size_t             vendor_cap;
    char               *model;        /* Product model name */
    size_t             model_len;
    size_t             model_cap;
    char               *description;  /* Human-readable functional description */
    size_t             description_len;
    size_t             description_cap;
    device_resources_t res;           /* Resource assignment */
    char               *driver_name;  /* Associated driver name */
    size_t             driver_name_len;
    size_t             driver_name_cap;
    void              *driver_ptr;    /* Pointer to driver_t if attached */
    const device_ops_t *ops;          /* Device I/O operations */
    void              *priv_data;     /* Subsystem-specific private data */
    struct device_node *next;         /* Linked list pointer */
} device_node_t;

/* Lifecycle helpers for dynamic device resources */
void device_resources_init(device_resources_t *res);
void device_resources_free(device_resources_t *res);
int  device_resources_add_bar(device_resources_t *res, uint32_t bar);
int  device_resources_set_bar(device_resources_t *res, size_t index, uint32_t bar);
int  device_resources_copy(device_resources_t *dst, const device_resources_t *src);

/* Lifecycle helpers for dynamic device nodes */
void device_node_init(device_node_t *dev);
void device_node_free(device_node_t *dev);
int  device_node_set_name(device_node_t *dev, const char *name);
int  device_node_set_vendor(device_node_t *dev, const char *vendor);
int  device_node_set_model(device_node_t *dev, const char *model);
int  device_node_set_description(device_node_t *dev, const char *desc);
int  device_node_set_driver_name(device_node_t *dev, const char *driver);
int  device_node_copy(device_node_t *dst, const device_node_t *src);

/* Device Registry API */
void device_init(void);

/* Registration, query and state management */
device_node_t *device_register(const device_node_t *dev);
int device_unregister(uint32_t id);
device_node_t *device_find_by_id(uint32_t id);
device_node_t *device_find_by_name(const char *name);
int device_set_state(uint32_t id, device_state_t state);
int device_set_driver(uint32_t id, const char *driver_name, void *driver_ptr);

/* Statistics and list access */
uint32_t device_get_count(void);
uint32_t device_get_count_by_type(device_type_t type);
uint32_t device_get_count_by_state(device_state_t state);
const device_node_t *device_get_list(void);

/* Dynamic hardware rescan and hotplug discovery */
int device_rescan(void);

/* String conversion helpers */
const char *device_type_to_string(device_type_t type);
const char *device_state_to_string(device_state_t state);
const char *bus_type_to_string(bus_type_t bus);

#endif /* KERNEL_DEVICE_H */

