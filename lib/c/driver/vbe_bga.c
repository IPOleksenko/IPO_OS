#include <driver/vbe_bga.h>
#include <driver/net/pci.h>
#include <ioport.h>
#include <vga.h>
#include <vga_gfx.h>
#include <stdio.h>
#include <string.h>

static vbe_bga_info_t g_bga_info = {
    .available = false,
    .version   = 0,
    .lfb_addr  = BGA_DEFAULT_LFB_ADDR,
    .width     = 0,
    .height    = 0,
    .bpp       = 0
};

static inline void bga_write_register(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static inline uint16_t bga_read_register(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

bool vbe_bga_probe(vbe_bga_info_t *out_info) {
    /* 1. Probe PCI for device 0x1234:0x1111 to find exact LFB BAR0 address */
    pci_device_t dev;
    bool pci_found = pci_find_device(BGA_PCI_VENDOR_ID, BGA_PCI_DEVICE_ID, &dev);
    if (pci_found) {
        /* Enable Memory Space (bit 1), I/O Space (bit 0), and Bus Master (bit 2) */
        uint16_t cmd = pci_read_word(dev.bus, dev.slot, dev.func, 0x04);
        cmd |= 0x07;
        pci_write_word(dev.bus, dev.slot, dev.func, 0x04, cmd);

        uint32_t bar0 = dev.bar0 & ~0x0Fu;
        if (bar0 >= 0x01000000u && bar0 < 0xFF000000u) {
            g_bga_info.lfb_addr = bar0;
        }
    }

    /* 2. Check I/O ports 0x1CE / 0x1CF */
    bga_write_register(VBE_DISPI_INDEX_ID, VBE_DISPI_ID5);
    uint16_t id = bga_read_register(VBE_DISPI_INDEX_ID);
    if (id < VBE_DISPI_ID0 || id > VBE_DISPI_ID5) {
        bga_write_register(VBE_DISPI_INDEX_ID, VBE_DISPI_ID4);
        id = bga_read_register(VBE_DISPI_INDEX_ID);
    }

    if (id < VBE_DISPI_ID0 || id > VBE_DISPI_ID5) {
        g_bga_info.available = false;
        if (out_info) memset(out_info, 0, sizeof(*out_info));
        return false;
    }

    g_bga_info.available = true;
    g_bga_info.version   = id;
    if (!pci_found) {
        g_bga_info.lfb_addr = BGA_DEFAULT_LFB_ADDR;
    }

    if (out_info) {
        *out_info = g_bga_info;
    }
    return true;
}

bool vbe_bga_is_available(void) {
    if (!g_bga_info.available) {
        vbe_bga_probe(NULL);
    }
    return g_bga_info.available;
}

bool vbe_bga_is_enabled(void) {
    if (!vbe_bga_is_available()) {
        return false;
    }
    uint16_t en = bga_read_register(VBE_DISPI_INDEX_ENABLE);
    return (en & VBE_DISPI_ENABLED) != 0;
}

uint32_t vbe_bga_get_lfb_address(void) {
    if (!g_bga_info.available) {
        vbe_bga_probe(NULL);
    }
    return g_bga_info.lfb_addr;
}

bool vbe_bga_set_mode(int width, int height, int bpp) {
    if (!vbe_bga_is_available()) {
        return false;
    }

    /* Save pristine standard VGA text state (fonts, registers, cursor, palette) */
    vga_save_text_state();

    bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write_register(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    bga_write_register(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    bga_write_register(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    g_bga_info.width  = width;
    g_bga_info.height = height;
    g_bga_info.bpp    = bpp;
    return true;
}

void vbe_bga_restore_text_mode(void) {
    if (vbe_bga_is_available()) {
        bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    }
    vga_set_mode_text_hardware();
    vga_show_cursor();
    vga_sanitize_text_vram();
}

void *vbe_bga_get_framebuffer(void) {
    return (void *)vbe_bga_get_lfb_address();
}

int vbe_bga_get_width(void)  { return g_bga_info.width; }
int vbe_bga_get_height(void) { return g_bga_info.height; }
int vbe_bga_get_bpp(void)    { return g_bga_info.bpp; }
