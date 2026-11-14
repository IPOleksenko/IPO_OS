#include <driver/net/rtl8139.h>
#include <driver/net/pci.h>
#include <net/net_state.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <ioport.h>
#include <string.h>
#include <stdio.h>

static uint16_t rtl8139_probe_hw_io_base(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx && ctx->rtl_dev.io_base != 0) {
        return ctx->rtl_dev.io_base;
    }
    uint32_t pci_count = pci_get_count();
    for (uint32_t i = 0; i < pci_count; i++) {
        const pci_device_t *cand = pci_get_device(i);
        if (cand && cand->vendor_id == 0x10ECu && cand->device_id == 0x8139u && (cand->bar0 & 1u)) {
            uint16_t io = (uint16_t)(cand->bar0 & ~3u);
            if (io != 0 && inb(io + 0x37) != 0xFF) {
                return io;
            }
        }
    }
    return 0;
}

size_t rtl8139_get_hw_rx_ring_size(void) {
    uint16_t io = rtl8139_probe_hw_io_base();
    if (io != 0) {
        /* Read RCR register (offset 0x44): bits 11..12 (RBLEN) encode hardware RX ring size */
        uint32_t rcr = inl(io + 0x44);
        uint32_t rblen = (rcr >> 11) & 0x3u;
        return (size_t)1u << (13u + rblen);
    }
    return (size_t)1u << 13u;
}

size_t rtl8139_get_hw_tx_descriptor_count(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx && ctx->tx_buffers_count > 0) {
        return ctx->tx_buffers_count;
    }
    /* RTL8139 hardware TX Status/Address descriptor register file: 0x10..0x1F (TSD0..3) */
    return (0x20u - 0x10u) / sizeof(uint32_t);
}

size_t rtl8139_get_hw_max_dma_burst(void) {
    uint16_t io = rtl8139_probe_hw_io_base();
    if (io != 0) {
        /* Read TCR register (offset 0x40): bits 8..10 (MXDMA) encode max DMA burst */
        uint32_t tcr = inl(io + 0x40);
        uint32_t mxdma = (tcr >> 8) & 0x7u;
        return (size_t)16u << mxdma;
    }
    return (size_t)16u << 7u;
}

size_t rtl8139_get_hw_socket_capacity(void) {
    size_t rx_ring = rtl8139_get_hw_rx_ring_size();
    size_t dma_burst = rtl8139_get_hw_max_dma_burst();
    size_t tx_descs = rtl8139_get_hw_tx_descriptor_count();
    size_t slots = (dma_burst > 0) ? (rx_ring / dma_burst) : tx_descs;
    if (slots < tx_descs) {
        slots = tx_descs;
    }
    return slots * tx_descs;
}

uint32_t rtl8139_get_hw_max_window(void) {
    size_t rx_ring = rtl8139_get_hw_rx_ring_size();
    size_t tx_descs = rtl8139_get_hw_tx_descriptor_count();
    size_t hw_wnd = rx_ring * tx_descs * sizeof(uint16_t);
    uint32_t wire_max = (uint32_t)((1ULL << (sizeof(uint16_t) * 8u)) - 1ULL);
    if (hw_wnd == 0 || hw_wnd > (size_t)wire_max) {
        return wire_max;
    }
    return (uint32_t)hw_wnd;
}

uint16_t rtl8139_get_hw_mss(void) {
    size_t rx_buf = rtl8139_get_rx_buffer_size();
    size_t rx_ring = rtl8139_get_hw_rx_ring_size();
    size_t hdr_margin = sizeof(uint32_t) * 4u;
    size_t mtu = (rx_buf > (rx_ring + hdr_margin))
                     ? (rx_buf - rx_ring - hdr_margin)
                     : rtl8139_get_tx_buffer_size();
    size_t proto_hdrs = 20u + 20u;
    return (uint16_t)((mtu > proto_hdrs) ? (mtu - proto_hdrs) : mtu);
}

size_t rtl8139_get_rx_buffer_size(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx && ctx->rx_buffer_size > 0) return ctx->rx_buffer_size;
    return RTL8139_RX_BUFFER_SIZE_DEFAULT;
}

size_t rtl8139_get_tx_buffer_size(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx && ctx->tx_buffer_size > 0) return ctx->tx_buffer_size;
    return RTL8139_TX_BUFFER_SIZE_DEFAULT;
}

bool rtl8139_set_rx_buffer_size(size_t new_size) {
    size_t min_size = rtl8139_get_hw_rx_ring_size() + 16u + 1500u;
    if (new_size < min_size) return false;
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx) return false;

    bool was_active = (ctx->rtl_dev.initialized && ctx->rtl_dev.io_base != 0);
    uint8_t saved_cr = 0;
    if (was_active) {
        saved_cr = inb(ctx->rtl_dev.io_base + 0x37);
        outb(ctx->rtl_dev.io_base + 0x37, saved_cr & ~0x08u);
    }

    uint8_t *new_buf = (uint8_t *)krealloc(ctx->rx_buffer, new_size);
    if (!new_buf) {
        if (was_active) {
            outb(ctx->rtl_dev.io_base + 0x37, saved_cr);
        }
        return false;
    }

    ctx->rx_buffer = new_buf;
    ctx->rx_buffer_size = new_size;
    if (was_active) {
        outl(ctx->rtl_dev.io_base + 0x30, (uint32_t)(uintptr_t)ctx->rx_buffer);
        ctx->rtl_dev.rx_offset = 0;
        outw(ctx->rtl_dev.io_base + 0x38, (uint16_t)-0x10);
        outb(ctx->rtl_dev.io_base + 0x37, saved_cr);
    }
    return true;
}

bool rtl8139_set_tx_buffer_size(size_t new_size) {
    if (new_size < 1500) return false;
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx || !ctx->tx_buffers) return false;

    for (size_t i = 0; i < ctx->tx_buffers_count; i++) {
        uint8_t *new_buf = (uint8_t *)krealloc(ctx->tx_buffers[i], new_size);
        if (!new_buf) {
            /* Retain old tx_buffer_size if expanding fails partway so no descriptor is undersized */
            if (new_size < ctx->tx_buffer_size) {
                ctx->tx_buffer_size = new_size;
            }
            return false;
        }
        ctx->tx_buffers[i] = new_buf;
        if (ctx->rtl_dev.initialized && ctx->rtl_dev.io_base) {
            outl(ctx->rtl_dev.io_base + 0x20 + (i * 4), (uint32_t)(uintptr_t)ctx->tx_buffers[i]);
        }
    }
    ctx->tx_buffer_size = new_size;
    return true;
}

bool rtl8139_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx) return false;
    if (ctx->rtl_dev.initialized) return true;

    /* Probe PCI bus for a Realtek RTL8139 controller with a valid I/O BAR */
    uint32_t pci_count = pci_get_count();
    pci_device_t pci_dev;
    bool found = false;

    for (uint32_t i = 0; i < pci_count; i++) {
        const pci_device_t *cand = pci_get_device(i);
        if (!cand) continue;

        if (cand->vendor_id == 0x10ECu && cand->device_id == 0x8139u &&
            (cand->bar0 & 1u) != 0 && (cand->bar0 & ~3u) != 0) {
            uint16_t io = (uint16_t)(cand->bar0 & ~3u);
            if (inb(io + 0x37) != 0xFF) {
                pci_dev = *cand;
                found = true;
                break;
            }
        }
    }

    if (!found && pci_find_device(0x10ECu, 0x8139u, &pci_dev) &&
        (pci_dev.bar0 & 1u) != 0 && (pci_dev.bar0 & ~3u) != 0) {
        found = true;
    }

    if (!found) {
        serial_printf("[rtl8139] No compatible PCI Ethernet network controller found on hardware bus!\n");
        return false;
    }

    uint16_t io_base = (uint16_t)(pci_dev.bar0 & ~0x3u);
    ctx->rtl_dev.vendor_id        = pci_dev.vendor_id;
    ctx->rtl_dev.device_id        = pci_dev.device_id;
    ctx->rtl_dev.subsys_vendor_id = pci_dev.subsys_vendor_id;
    ctx->rtl_dev.subsys_id        = pci_dev.subsys_id;
    ctx->rtl_dev.revision_id      = pci_dev.revision_id;
    ctx->rtl_dev.bus              = pci_dev.bus;
    ctx->rtl_dev.slot             = pci_dev.slot;
    ctx->rtl_dev.func             = pci_dev.func;
    ctx->rtl_dev.io_base          = io_base;

    printf("[rtl8139] Hardware probed at PCI %02x:%02x.%x: Vendor ID=0x%04x, Device ID=0x%04x, Rev=0x%02x, IO base=0x%x, IRQ=%u\n",
                  (unsigned int)pci_dev.bus, (unsigned int)pci_dev.slot, (unsigned int)pci_dev.func,
                  (unsigned int)ctx->rtl_dev.vendor_id, (unsigned int)ctx->rtl_dev.device_id,
                  (unsigned int)ctx->rtl_dev.revision_id,
                  (unsigned int)io_base, (unsigned int)pci_dev.irq);
    pci_enable_bus_mastering(&pci_dev);

    /* Power on controller (CONFIG_1) and issue software reset */
    outb(io_base + 0x52, 0x00);
    outb(io_base + 0x37, 0x10);
    uint32_t timeout = 100000;
    while ((inb(io_base + 0x37) & 0x10) != 0 && --timeout) {
        io_wait();
    }

    /* Read hardware MAC address */
    size_t hw_mac_len = 6;
    if (!ctx->rtl_dev.mac || ctx->rtl_dev.mac_cap < hw_mac_len) {
        uint8_t *new_mac = (uint8_t *)kmalloc(hw_mac_len);
        if (!new_mac) return false;
        if (ctx->rtl_dev.mac) kfree(ctx->rtl_dev.mac);
        ctx->rtl_dev.mac = new_mac;
        ctx->rtl_dev.mac_cap = hw_mac_len;
    }
    ctx->rtl_dev.mac_len = hw_mac_len;
    for (size_t i = 0; i < hw_mac_len; i++) {
        ctx->rtl_dev.mac[i] = inb(io_base + i);
    }

    /* Allocate RX ring buffer + wrap margin */
    if (!ctx->rx_buffer || ctx->rx_buffer_size < RTL8139_RX_BUFFER_SIZE_DEFAULT) {
        uint8_t *new_rx = (uint8_t *)krealloc(ctx->rx_buffer, RTL8139_RX_BUFFER_SIZE_DEFAULT);
        if (!new_rx) return false;
        ctx->rx_buffer = new_rx;
        ctx->rx_buffer_size = RTL8139_RX_BUFFER_SIZE_DEFAULT;
    }
    memset(ctx->rx_buffer, 0, ctx->rx_buffer_size);
    outl(io_base + 0x30, (uint32_t)(uintptr_t)ctx->rx_buffer);

    /* Allocate TX descriptor buffers */
    if (ctx->tx_buffer_size == 0) {
        ctx->tx_buffer_size = RTL8139_TX_BUFFER_SIZE_DEFAULT;
    }
    if (ctx->tx_buffers_count == 0 || !ctx->tx_buffers) {
        size_t count = 4;
        uint8_t **arr = (uint8_t **)kmalloc(count * sizeof(uint8_t *));
        if (!arr) return false;
        memset(arr, 0, count * sizeof(uint8_t *));
        ctx->tx_buffers = arr;
        ctx->tx_buffers_count = count;
    }
    for (size_t i = 0; i < ctx->tx_buffers_count; i++) {
        if (!ctx->tx_buffers[i]) {
            ctx->tx_buffers[i] = (uint8_t *)kmalloc(ctx->tx_buffer_size);
            if (!ctx->tx_buffers[i]) return false;
        }
        memset(ctx->tx_buffers[i], 0, ctx->tx_buffer_size);
        outl(io_base + 0x20 + (i * 4), (uint32_t)(uintptr_t)ctx->tx_buffers[i]);
    }
    ctx->rtl_dev.tx_cur = 0;
    ctx->rtl_dev.rx_offset = 0;

    /* Initialize CAPR, interrupt mask (TOK | ROK), RCR (AAP|APM|AM|AB|WRAP), and enable TE/RE */
    outw(io_base + 0x38, (uint16_t)-0x10);
    outw(io_base + 0x3C, 0x0005);
    outl(io_base + 0x44, 0x0000008F);
    outb(io_base + 0x37, 0x0C);

    ctx->rtl_dev.initialized = true;

    /* Wait briefly for physical link stabilization (up to 2000 ms) */
    uint32_t link_start = timer_millis();
    while (!rtl8139_is_link_up() && (timer_millis() - link_start < 2000)) {
        io_wait();
    }

    return true;
}

bool rtl8139_is_present(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx && ctx->rtl_dev.initialized;
}

bool rtl8139_is_link_up(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx || !ctx->rtl_dev.initialized) return false;
    uint8_t msr = inb(ctx->rtl_dev.io_base + 0x58);
    /* In RTL8139, MSR (0x58) bit 2 is LINKB (active-low: 0 = Link OK, 1 = Link Fail) */
    return (msr & 0x04) == 0;
}

const uint8_t *rtl8139_get_mac(size_t *out_len) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) {
        *out_len = ctx->rtl_dev.mac_len;
    }
    return ctx->rtl_dev.mac;
}

size_t rtl8139_get_mac_len(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx ? ctx->rtl_dev.mac_len : 0;
}

uint16_t rtl8139_get_vendor_id(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx ? ctx->rtl_dev.vendor_id : 0;
}

uint16_t rtl8139_get_device_id(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx ? ctx->rtl_dev.device_id : 0;
}

uint16_t rtl8139_get_subsys_vendor_id(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx ? ctx->rtl_dev.subsys_vendor_id : 0;
}

uint16_t rtl8139_get_subsys_id(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx ? ctx->rtl_dev.subsys_id : 0;
}

uint8_t rtl8139_get_revision_id(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx ? ctx->rtl_dev.revision_id : 0;
}

uint8_t rtl8139_get_bus(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx ? ctx->rtl_dev.bus : 0;
}

uint8_t rtl8139_get_slot(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx ? ctx->rtl_dev.slot : 0;
}

uint8_t rtl8139_get_func(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx ? ctx->rtl_dev.func : 0;
}

static void rtl8139_resync_rx(net_shared_ctx_t *ctx, uint16_t io, size_t hw_ring_len) {
    uint16_t cbr = inw(io + 0x3A);
    ctx->rtl_dev.rx_offset = (uint16_t)(cbr % hw_ring_len);
    outw(io + 0x38, (uint16_t)(ctx->rtl_dev.rx_offset - 0x10));
    outw(io + 0x3E, 0x0005);
}

int rtl8139_send_packet(const void *data, uint16_t len) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx || !ctx->rtl_dev.initialized || !data || len == 0 || !ctx->tx_buffers || ctx->tx_buffers_count == 0) {
        return -1;
    }

    uint16_t io = ctx->rtl_dev.io_base;
    uint8_t cur = ctx->rtl_dev.tx_cur % ctx->tx_buffers_count;

    /* Wait for previous transmit on this descriptor before touching/reallocating TX buffers */
    uint32_t start_ms = timer_millis();
    while ((inl(io + 0x10 + (cur * 4)) & 0x2000) == 0) {
        if (inl(io + 0x10 + (cur * 4)) == 0) break;
        if (timer_millis() - start_ms > 100) {
            return -1;
        }
        io_wait();
    }

    if (len > ctx->tx_buffer_size) {
        if (!rtl8139_set_tx_buffer_size((size_t)len + 128u)) {
            return -1;
        }
    }
    if (!ctx->tx_buffers[cur] || len > ctx->tx_buffer_size) {
        return -1;
    }

    uint16_t send_len = len < 60 ? 60 : len;
    memset(ctx->tx_buffers[cur], 0, 60);
    memcpy(ctx->tx_buffers[cur], data, len);

    /* Start transmit by writing length to TSD */
    outl(io + 0x10 + (cur * 4), send_len);

    ctx->rtl_dev.tx_cur = (uint8_t)((cur + 1) % ctx->tx_buffers_count);
    return (int)len;
}

int rtl8139_peek_packet_size(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx || !ctx->rtl_dev.initialized || !ctx->rx_buffer) {
        return 0;
    }

    uint16_t io = ctx->rtl_dev.io_base;
    if (inb(io + 0x37) & 0x01) {
        return 0;
    }

    uint16_t offset = ctx->rtl_dev.rx_offset;
    if ((size_t)offset + 4u > ctx->rx_buffer_size) {
        return 0;
    }
    const uint8_t *rx_ptr = ctx->rx_buffer + offset;
    uint16_t status = *(const uint16_t *)rx_ptr;
    uint16_t length = *(const uint16_t *)(rx_ptr + 2);

    if ((status & 0x01) == 0 || length < 4 || (size_t)offset + 4u + (size_t)length > ctx->rx_buffer_size) {
        return 0;
    }

    return (int)(length - 4);
}

int rtl8139_receive_packet(void *buf, uint32_t max_len) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx || !ctx->rtl_dev.initialized || !ctx->rx_buffer || !buf || max_len == 0) {
        return 0;
    }

    uint16_t io = ctx->rtl_dev.io_base;
    if (inb(io + 0x37) & 0x01) {
        return 0;
    }

    size_t hw_ring_len = rtl8139_get_hw_rx_ring_size();
    uint16_t offset = ctx->rtl_dev.rx_offset;
    if ((size_t)offset + 4u > ctx->rx_buffer_size) {
        rtl8139_resync_rx(ctx, io, hw_ring_len);
        return 0;
    }

    uint8_t *rx_ptr = ctx->rx_buffer + offset;
    uint16_t status = *(uint16_t *)rx_ptr;
    uint16_t length = *(uint16_t *)(rx_ptr + 2);

    if ((status & 0x01) == 0) {
        if (length >= 4 && (size_t)offset + 4u + (size_t)length <= ctx->rx_buffer_size) {
            offset = (uint16_t)(((size_t)offset + length + 4u + 3u) & ~3u);
            offset = (uint16_t)(offset % hw_ring_len);
            ctx->rtl_dev.rx_offset = offset;
            outw(io + 0x38, (uint16_t)(offset - 0x10));
            outw(io + 0x3E, 0x0005);
        } else {
            rtl8139_resync_rx(ctx, io, hw_ring_len);
        }
        return 0;
    }

    if (length < 4 || (size_t)offset + 4u + (size_t)length > ctx->rx_buffer_size) {
        rtl8139_resync_rx(ctx, io, hw_ring_len);
        return 0;
    }

    uint32_t packet_len = (uint32_t)(length - 4);
    uint32_t copy_len = packet_len < max_len ? packet_len : max_len;
    memcpy(buf, rx_ptr + 4, copy_len);

    offset = (uint16_t)(((size_t)offset + length + 4u + 3u) & ~3u);
    offset = (uint16_t)(offset % hw_ring_len);
    ctx->rtl_dev.rx_offset = offset;

    outw(io + 0x38, (uint16_t)(offset - 0x10));
    outw(io + 0x3E, 0x0005);

    return (int)copy_len;
}
