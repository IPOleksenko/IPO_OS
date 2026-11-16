#include <driver/vbe_bga.h>
#include <driver/pci.h>
#include <ioport.h>
#include <vga.h>
#include <vga_gfx.h>
#include <stdio.h>
#include <string.h>
#include <driver/input/mouse.h>

static vbe_bga_info_t g_bga_info = {
    .available = false,
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
    /* Probe PCI display adapter (0x1234:0x1111) for LFB BAR0 */
    pci_device_t dev;
    bool pci_found = pci_find_device(BGA_PCI_VENDOR_ID, BGA_PCI_DEVICE_ID, &dev);
    if (pci_found) {
        /* Enable I/O space (bit 0), memory space (bit 1), and bus mastering (bit 2) */
        uint16_t cmd = pci_read_word(dev.bus, dev.slot, dev.func, 0x04);
        cmd |= 0x07;
        pci_write_word(dev.bus, dev.slot, dev.func, 0x04, cmd);

        if ((dev.bar0 & 1u) == 0) {
            uint32_t bar0 = dev.bar0 & ~0x0Fu;
            if (bar0 >= 0x01000000u && bar0 < 0xFF000000u) {
                g_bga_info.lfb_addr = bar0;
            }
        }
    }

    /* Probe DISPI framebuffer registers on I/O ports 0x1CE / 0x1CF */
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

static int s_bga_enabled_cached = -1;

static void bga_program_mode_regs(uint16_t w, uint16_t h, uint16_t bpp) {
    bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write_register(VBE_DISPI_INDEX_XRES, w);
    bga_write_register(VBE_DISPI_INDEX_YRES, h);
    bga_write_register(VBE_DISPI_INDEX_BPP, bpp);
    bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    s_bga_enabled_cached = 1;
}

bool vbe_bga_is_enabled(void) {
    if (!vbe_bga_is_available()) {
        return false;
    }
    if (s_bga_enabled_cached < 0) {
        uint16_t en = bga_read_register(VBE_DISPI_INDEX_ENABLE);
        s_bga_enabled_cached = ((en & VBE_DISPI_ENABLED) != 0) ? 1 : 0;
    }
    return s_bga_enabled_cached != 0;
}

bool vbe_bga_mode_request_valid(int width, int height, int bpp) {
    if (!vbe_bga_is_available() || width < 0 || height < 0 ||
        width > 0xFFFF || height > 0xFFFF ||
        (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32)) {
        return false;
    }

    return true;
}

void vbe_bga_disable(void) {
    if (vbe_bga_is_available()) {
        bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        s_bga_enabled_cached = 0;
    }
}

ipo_paddr_t vbe_bga_get_lfb_address(void) {
    if (!g_bga_info.available) {
        vbe_bga_probe(NULL);
    }
    return g_bga_info.lfb_addr;
}

bool vbe_bga_set_mode(int width, int height, int bpp) {
    if (!vbe_bga_mode_request_valid(width, height, bpp)) {
        return false;
    }

    serial_printf("[vbe_bga] requesting mode %dx%d at %d bpp\n", width, height, bpp);

    bool prev_enabled = vbe_bga_is_enabled();
    int prev_w = g_bga_info.width;
    int prev_h = g_bga_info.height;
    int prev_bpp = g_bga_info.bpp;

    /* Save standard VGA text state before mode switch */
    vga_save_text_state();

    bga_program_mode_regs((uint16_t)width, (uint16_t)height, (uint16_t)bpp);

    uint16_t actual_width = bga_read_register(VBE_DISPI_INDEX_XRES);
    uint16_t actual_height = bga_read_register(VBE_DISPI_INDEX_YRES);
    uint16_t actual_bpp = bga_read_register(VBE_DISPI_INDEX_BPP);
    uint16_t actual_enable = bga_read_register(VBE_DISPI_INDEX_ENABLE);

    bool device_enabled = ((actual_enable & VBE_DISPI_ENABLED) != 0) &&
                          actual_width > 0 && actual_height > 0 &&
                          actual_bpp == (uint16_t)bpp;
    if (!device_enabled) {
        serial_printf("[vbe_bga] device rejected mode; read back %ux%u at %u bpp (en=0x%x)\n",
                      actual_width, actual_height, actual_bpp, actual_enable);
        s_bga_enabled_cached = 0;
        if (prev_enabled && prev_w > 0 && prev_h > 0 && prev_bpp > 0) {
            bga_program_mode_regs((uint16_t)prev_w, (uint16_t)prev_h, (uint16_t)prev_bpp);
            g_bga_info.width = (int)bga_read_register(VBE_DISPI_INDEX_XRES);
            g_bga_info.height = (int)bga_read_register(VBE_DISPI_INDEX_YRES);
            g_bga_info.bpp = (int)bga_read_register(VBE_DISPI_INDEX_BPP);
        } else {
            /* Disable DISPI framebuffer without restoring text mode so saved
             * text VRAM stays intact across Mode 13h fallback until exit. */
            vbe_bga_disable();
        }
        mouse_set_bounds_from_display();
        return false;
    }

    /* DISPI hardware aligns XRES down to an 8-pixel boundary (e.g. 999 -> 992)
     * and clamps out-of-range requests while staying enabled. */
    bool width_ok = (actual_width == (uint16_t)width) ||
                    (width >= 8 && actual_width == (uint16_t)(width & ~7));
    if (!width_ok || actual_height != (uint16_t)height) {
        g_bga_info.width = (int)actual_width;
        g_bga_info.height = (int)actual_height;
        g_bga_info.bpp = (int)actual_bpp;
        s_bga_enabled_cached = 1;
        serial_printf("[vbe_bga] device clamped mode %dx%d -> %ux%u at %u bpp\n",
                      width, height, actual_width, actual_height, actual_bpp);
        mouse_set_bounds_from_display();
        return false;
    }

    g_bga_info.width = actual_width;
    g_bga_info.height = actual_height;
    g_bga_info.bpp = actual_bpp;
    mouse_set_bounds_from_display();
    return true;
}

void vbe_bga_restore_text_mode(void) {
    if (vbe_bga_is_available()) {
        bga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
        s_bga_enabled_cached = 0;
    }
    vga_set_mode_text_hardware();
    vga_show_cursor();
    vga_sanitize_text_vram();
}

void *vbe_bga_get_framebuffer(void) {
    return (void *)(ipo_vaddr_t)vbe_bga_get_lfb_address();
}

int vbe_bga_get_width(void)  { return g_bga_info.width; }
int vbe_bga_get_height(void) { return g_bga_info.height; }
int vbe_bga_get_bpp(void)    { return g_bga_info.bpp; }
