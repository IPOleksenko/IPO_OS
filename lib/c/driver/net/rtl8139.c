#include <driver/net/rtl8139.h>
#include <driver/net/pci.h>
#include <net/net_state.h>
#include <ioport.h>
#include <string.h>
#include <stdio.h>

bool rtl8139_init(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (ctx->rtl_dev.initialized) return true;

    pci_device_t pci_dev;
    if (!pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, &pci_dev)) {
        serial_printf("[rtl8139] PCI device 0x10EC:0x8139 NOT found!\n");
        return false;
    }

    uint16_t io_base = (uint16_t)(pci_dev.bar0 & ~0x3);
    serial_printf("[rtl8139] Found PCI device at bus=%x slot=%x func=%x, IO base: 0x%x, IRQ: %u\n",
                  (unsigned int)pci_dev.bus, (unsigned int)pci_dev.slot, (unsigned int)pci_dev.func, (unsigned int)io_base, (unsigned int)pci_dev.irq);
    ctx->rtl_dev.io_base = io_base;
    pci_enable_bus_mastering(&pci_dev);

    /* 1. Turn on RTL8139 (CONFIG_1) */
    outb(io_base + 0x52, 0x00);

    /* 2. Software reset */
    outb(io_base + 0x37, 0x10);
    uint32_t timeout = 100000;
    while ((inb(io_base + 0x37) & 0x10) != 0 && --timeout) {
        io_wait();
    }

    /* 3. Read hardware MAC address */
    for (int i = 0; i < 6; i++) {
        ctx->rtl_dev.mac[i] = inb(io_base + i);
    }

    /* 4. Set up RX buffer in shared physical memory */
    memset(ctx->rx_buffer, 0, sizeof(ctx->rx_buffer));
    outl(io_base + 0x30, (uint32_t)(uintptr_t)ctx->rx_buffer);

    /* 5. Set up TX buffers in shared physical memory */
    for (int i = 0; i < 4; i++) {
        memset(ctx->tx_buffers[i], 0, sizeof(ctx->tx_buffers[i]));
        outl(io_base + 0x20 + (i * 4), (uint32_t)(uintptr_t)ctx->tx_buffers[i]);
    }
    ctx->rtl_dev.tx_cur = 0;
    ctx->rtl_dev.rx_offset = 0;

    /* 6. Configure Interrupt Mask (TOK=bit 2, ROK=bit 0) -> 0x0005 */
    outw(io_base + 0x3C, 0x0005);

    /* 7. Configure RCR: AAP(1) | APM(2) | AM(4) | AB(8) | WRAP(0x80) */
    outl(io_base + 0x44, 0x0000008F);

    /* 8. Enable TX and RX (TE=0x04, RE=0x08 -> 0x0C) */
    outb(io_base + 0x37, 0x0C);

    ctx->rtl_dev.initialized = true;
    return true;
}

bool rtl8139_is_present(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.initialized;
}

const uint8_t *rtl8139_get_mac(void) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    return ctx->rtl_dev.mac;
}

int rtl8139_send_packet(const void *data, uint16_t len) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx->rtl_dev.initialized || !data || len == 0 || len > RTL8139_TX_BUFFER_SIZE) {
        return -1;
    }

    uint16_t io = ctx->rtl_dev.io_base;
    uint8_t cur = ctx->rtl_dev.tx_cur;

    /* Wait for previous transmit on this descriptor if busy */
    uint32_t timeout = 50000;
    while ((inl(io + 0x10 + (cur * 4)) & 0x2000) == 0 && --timeout) {
        /* Wait for OWN bit (0x2000) indicating descriptor is ready */
        if (inl(io + 0x10 + (cur * 4)) == 0) break; // First transmit
        io_wait();
    }

    uint16_t send_len = len < 60 ? 60 : len;
    memset(ctx->tx_buffers[cur], 0, 60);
    memcpy(ctx->tx_buffers[cur], data, len);

    /* Start transmit by writing length to TSD */
    outl(io + 0x10 + (cur * 4), send_len);

    ctx->rtl_dev.tx_cur = (cur + 1) % 4;
    return (int)len;
}

int rtl8139_receive_packet(void *buf, uint16_t max_len) {
    net_shared_ctx_t *ctx = net_get_shared_context();
    if (!ctx->rtl_dev.initialized || !buf || max_len == 0) {
        return 0;
    }

    uint16_t io = ctx->rtl_dev.io_base;

    uint8_t cr = inb(io + 0x37);
    uint16_t isr = inw(io + 0x3E);
    uint16_t capr = inw(io + 0x38);
    uint16_t cbr = inw(io + 0x3A);

    /* If CR says not empty OR ISR says ROK OR CAPR != CBR */
    if ((cr & 0x01) && (isr & 0x01) == 0 && capr == (uint16_t)(cbr - 0x10)) {
        return 0;
    }

    uint16_t offset = ctx->rtl_dev.rx_offset;
    uint8_t *rx_ptr = ctx->rx_buffer + offset;

    uint16_t status = *(uint16_t *)rx_ptr;
    uint16_t length = *(uint16_t *)(rx_ptr + 2);

    /* Check for Receive OK bit (bit 0 of status) */
    if ((status & 0x01) == 0 || length < 4 || length > 1536) {
        /* Packet bad, reset receiver offset */
        ctx->rtl_dev.rx_offset = 0;
        outw(io + 0x38, (uint16_t)-0x10);
        return 0;
    }

    uint16_t packet_len = length - 4; // Exclude 4-byte CRC
    uint16_t copy_len = packet_len < max_len ? packet_len : max_len;

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
