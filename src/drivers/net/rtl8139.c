#include <driver/net/rtl8139.h>
#include <driver/net/pci.h>
#include <net/net_state.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <ioport.h>
#include <string.h>
#include <stdio.h>

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
    if (new_size < 4096) return false;
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx) return false;

    uint8_t *new_buf = (uint8_t *)krealloc(ctx->rx_buffer, new_size);
    if (!new_buf) return false;

    ctx->rx_buffer = new_buf;
    ctx->rx_buffer_size = new_size;
    if (ctx->rtl_dev.initialized && ctx->rtl_dev.io_base) {
        outl(ctx->rtl_dev.io_base + 0x30, (uint32_t)(uintptr_t)ctx->rx_buffer);
    }
    return true;
}

bool rtl8139_set_tx_buffer_size(size_t new_size) {
    if (new_size < 1500) return false;
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx || !ctx->tx_buffers) return false;

    for (size_t i = 0; i < ctx->tx_buffers_count; i++) {
        uint8_t *new_buf = (uint8_t *)krealloc(ctx->tx_buffers[i], new_size);
        if (!new_buf) return false;
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
    if (ctx->rtl_dev.initialized) return true;

    /* Scan PCI bus directly from hardware for network ethernet controllers */
    uint32_t pci_count = pci_get_count();
    pci_device_t pci_dev;
    bool found = false;

    /* 1. Try to find PCI Network Controller (Class 0x02) with I/O space BAR responding to RTL8139 registers */
    for (uint32_t i = 0; i < pci_count; i++) {
        const pci_device_t *cand = pci_get_device(i);
        if (!cand) continue;

        if (cand->class_code == PCI_CLASS_NETWORK && (cand->bar0 & 1)) {
            uint16_t io = (uint16_t)(cand->bar0 & ~3u);
            /* Probe the hardware I/O port: Command Register at offset 0x37 */
            uint8_t cr = inb(io + 0x37);
            if (cr != 0xFF) {
                pci_dev = *cand;
                found = true;
                break;
            }
        }
    }

    /* 2. Fallback: if not matched by I/O probe, search by class 0x02 / 0x00 directly */
    if (!found) {
        if (pci_find_class(PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHERNET, &pci_dev)) {
            found = true;
        }
    }

    if (!found) {
        serial_printf("[rtl8139] No compatible PCI Ethernet network controller found on hardware bus!\n");
        return false;
    }

    /* Extract device identification directly from hardware registers */
    uint16_t io_base = (uint16_t)(pci_dev.bar0 & ~0x3);
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

    /* 1. Turn on RTL8139 (CONFIG_1) */
    outb(io_base + 0x52, 0x00);

    /* 2. Software reset */
    outb(io_base + 0x37, 0x10);
    uint32_t timeout = 100000;
    while ((inb(io_base + 0x37) & 0x10) != 0 && --timeout) {
        io_wait();
    }

    /* 3. Read hardware MAC address dynamically allocated via kmalloc */
    size_t hw_mac_len = 6;
    if (!ctx->rtl_dev.mac || ctx->rtl_dev.mac_cap < hw_mac_len) {
        if (ctx->rtl_dev.mac) kfree(ctx->rtl_dev.mac);
        ctx->rtl_dev.mac = (uint8_t *)kmalloc(hw_mac_len);
        ctx->rtl_dev.mac_cap = hw_mac_len;
    }
    ctx->rtl_dev.mac_len = hw_mac_len;
    for (size_t i = 0; i < hw_mac_len; i++) {
        ctx->rtl_dev.mac[i] = inb(io_base + i);
    }

    /* 4. Set up self-expanding RX buffer dynamically allocated via kmalloc */
    if (!ctx->rx_buffer) {
        ctx->rx_buffer_size = RTL8139_RX_BUFFER_SIZE_DEFAULT;
        ctx->rx_buffer = (uint8_t *)kmalloc(ctx->rx_buffer_size);
    }
    if (ctx->rx_buffer) {
        memset(ctx->rx_buffer, 0, ctx->rx_buffer_size);
        outl(io_base + 0x30, (uint32_t)(uintptr_t)ctx->rx_buffer);
    }

    /* 5. Set up self-expanding TX buffers dynamically allocated via kmalloc */
    if (ctx->tx_buffer_size == 0) {
        ctx->tx_buffer_size = RTL8139_TX_BUFFER_SIZE_DEFAULT;
    }
    if (ctx->tx_buffers_count == 0 || !ctx->tx_buffers) {
        ctx->tx_buffers_count = 4;
        ctx->tx_buffers = (uint8_t **)kmalloc(ctx->tx_buffers_count * sizeof(uint8_t *));
        if (ctx->tx_buffers) {
            memset(ctx->tx_buffers, 0, ctx->tx_buffers_count * sizeof(uint8_t *));
        }
    }
    for (size_t i = 0; i < ctx->tx_buffers_count; i++) {
        if (!ctx->tx_buffers[i]) {
            ctx->tx_buffers[i] = (uint8_t *)kmalloc(ctx->tx_buffer_size);
        }
        if (ctx->tx_buffers[i]) {
            memset(ctx->tx_buffers[i], 0, ctx->tx_buffer_size);
            outl(io_base + 0x20 + (i * 4), (uint32_t)(uintptr_t)ctx->tx_buffers[i]);
        }
    }
    ctx->rtl_dev.tx_cur = 0;
    ctx->rtl_dev.rx_offset = 0;

    /* Initialize CAPR to rx_offset - 0x10 */
    outw(io_base + 0x38, (uint16_t)-0x10);

    /* 6. Configure Interrupt Mask (TOK=bit 2, ROK=bit 0) -> 0x0005 */
    outw(io_base + 0x3C, 0x0005);

    /* 7. Configure RCR: AAP(1) | APM(2) | AM(4) | AB(8) | WRAP(0x80) */
    outl(io_base + 0x44, 0x0000008F);

    /* 8. Enable TX and RX (TE=0x04, RE=0x08 -> 0x0C) */
    outb(io_base + 0x37, 0x0C);

    ctx->rtl_dev.initialized = true;

    /* Wait briefly for physical PHY link to stabilize if needed (up to 2000 ms) */
    uint32_t link_start = timer_millis();
    while (!rtl8139_is_link_up() && (timer_millis() - link_start < 2000)) {
        io_wait();
    }

    return true;
}

bool rtl8139_is_present(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.initialized;
}

bool rtl8139_is_link_up(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx->rtl_dev.initialized) return false;
    uint8_t msr = inb(ctx->rtl_dev.io_base + 0x58);
    /* In RTL8139, MSR (0x58) bit 2 is LINKB (active-low: 0 = Link OK, 1 = Link Fail) */
    return (msr & 0x04) == 0;
}

const uint8_t *rtl8139_get_mac(size_t *out_len) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (out_len) {
        *out_len = ctx->rtl_dev.mac_len;
    }
    return ctx->rtl_dev.mac;
}

size_t rtl8139_get_mac_len(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.mac_len;
}

uint16_t rtl8139_get_vendor_id(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.vendor_id;
}

uint16_t rtl8139_get_device_id(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.device_id;
}

uint16_t rtl8139_get_subsys_vendor_id(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.subsys_vendor_id;
}

uint16_t rtl8139_get_subsys_id(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.subsys_id;
}

uint8_t rtl8139_get_revision_id(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.revision_id;
}

uint8_t rtl8139_get_bus(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.bus;
}

uint8_t rtl8139_get_slot(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.slot;
}

uint8_t rtl8139_get_func(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.func;
}

int rtl8139_send_packet(const void *data, uint16_t len) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx->rtl_dev.initialized || !data || len == 0) {
        return -1;
    }

    /* Dynamically auto-expand TX buffer via krealloc if packet exceeds current capacity */
    if (len > ctx->tx_buffer_size) {
        size_t new_cap = len + 128;
        for (size_t i = 0; i < ctx->tx_buffers_count; i++) {
            uint8_t *new_buf = (uint8_t *)krealloc(ctx->tx_buffers[i], new_cap);
            if (new_buf) {
                ctx->tx_buffers[i] = new_buf;
                outl(ctx->rtl_dev.io_base + 0x20 + (i * 4), (uint32_t)(uintptr_t)ctx->tx_buffers[i]);
            }
        }
        ctx->tx_buffer_size = new_cap;
    }

    uint16_t io = ctx->rtl_dev.io_base;
    uint8_t cur = ctx->rtl_dev.tx_cur;

    /* Wait for previous transmit on this descriptor if busy */
    uint32_t start_ms = timer_millis();
    while ((inl(io + 0x10 + (cur * 4)) & 0x2000) == 0) {
        /* Wait for OWN bit (0x2000) indicating descriptor is ready */
        if (inl(io + 0x10 + (cur * 4)) == 0) break; // First transmit
        if (timer_millis() - start_ms > 100) {
            return -1; // Timed out waiting for descriptor
        }
        io_wait();
    }

    uint16_t send_len = len < 60 ? 60 : len;
    memset(ctx->tx_buffers[cur], 0, 60);
    memcpy(ctx->tx_buffers[cur], data, len);

    /* Start transmit by writing length to TSD */
    outl(io + 0x10 + (cur * 4), send_len);

    ctx->rtl_dev.tx_cur = (cur + 1) % (ctx->tx_buffers_count ? ctx->tx_buffers_count : 4);
    return (int)len;
}

int rtl8139_peek_packet_size(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx->rtl_dev.initialized) {
        return 0;
    }

    uint16_t io = ctx->rtl_dev.io_base;

    uint8_t cr = inb(io + 0x37);
    if (cr & 0x01) {
        /* Buffer Empty bit set in Command Register */
        return 0;
    }

    uint16_t offset = ctx->rtl_dev.rx_offset;
    uint8_t *rx_ptr = ctx->rx_buffer + offset;

    uint16_t status = *(uint16_t *)rx_ptr;
    uint16_t length = *(uint16_t *)(rx_ptr + 2);

    /* Check for Receive OK bit (bit 0 of status) and packet length bounds */
    if ((status & 0x01) == 0 || length < 4 || length > RTL8139_RX_BUFFER_SIZE) {
        return 0;
    }

    return (int)(length - 4);
}

int rtl8139_receive_packet(void *buf, uint32_t max_len) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx->rtl_dev.initialized || !buf || max_len == 0) {
        return 0;
    }

    uint16_t io = ctx->rtl_dev.io_base;

    uint8_t cr = inb(io + 0x37);
    if (cr & 0x01) {
        /* Buffer Empty bit set in Command Register */
        return 0;
    }

    uint16_t offset = ctx->rtl_dev.rx_offset;
    uint8_t *rx_ptr = ctx->rx_buffer + offset;

    uint16_t status = *(uint16_t *)rx_ptr;
    uint16_t length = *(uint16_t *)(rx_ptr + 2);

    /* Check for Receive OK bit (bit 0 of status) */
    if ((status & 0x01) == 0) {
        if (length >= 4 && length <= RTL8139_RX_BUFFER_SIZE) {
            offset = (uint16_t)((offset + length + 4 + 3) & ~3);
            offset %= 8192;
            ctx->rtl_dev.rx_offset = offset;
            outw(io + 0x38, (uint16_t)(offset - 0x10));
            outw(io + 0x3E, 0x0005);
        } else {
            /* Corrupt packet descriptor: resync read pointer with hardware Current Buffer Address (CBR at 0x3A) */
            uint16_t cbr = inw(io + 0x3A);
            ctx->rtl_dev.rx_offset = cbr % 8192;
            outw(io + 0x38, (uint16_t)(ctx->rtl_dev.rx_offset - 0x10));
            outw(io + 0x3E, 0x0005);
        }
        return 0;
    }

    if (length < 4 || length > RTL8139_RX_BUFFER_SIZE) {
        uint16_t cbr = inw(io + 0x3A);
        ctx->rtl_dev.rx_offset = cbr % 8192;
        outw(io + 0x38, (uint16_t)(ctx->rtl_dev.rx_offset - 0x10));
        outw(io + 0x3E, 0x0005);
        return 0;
    }

    uint32_t packet_len = (uint32_t)(length - 4); // Exclude 4-byte CRC
    uint32_t copy_len = packet_len < max_len ? packet_len : max_len;

    memcpy(buf, rx_ptr + 4, copy_len);

    /* Update ring buffer read pointer */
    offset = (uint16_t)((offset + length + 4 + 3) & ~3);
    offset %= 8192;
    ctx->rtl_dev.rx_offset = offset;

    /* Update CAPR register */
    outw(io + 0x38, (uint16_t)(offset - 0x10));

    /* Clear ISR acknowledge */
    outw(io + 0x3E, 0x0005);

    return (int)copy_len;
}
