#ifndef LIB_DRIVER_VBE_BGA_H
#define LIB_DRIVER_VBE_BGA_H

#include <stdint.h>
#include <stdbool.h>

#define VBE_DISPI_IOPORT_INDEX          0x01CE
#define VBE_DISPI_IOPORT_DATA           0x01CF

#define VBE_DISPI_INDEX_ID              0
#define VBE_DISPI_INDEX_XRES            1
#define VBE_DISPI_INDEX_YRES            2
#define VBE_DISPI_INDEX_BPP             3
#define VBE_DISPI_INDEX_ENABLE          4
#define VBE_DISPI_INDEX_BANK            5
#define VBE_DISPI_INDEX_VIRT_WIDTH      6
#define VBE_DISPI_INDEX_VIRT_HEIGHT     7
#define VBE_DISPI_INDEX_X_OFFSET        8
#define VBE_DISPI_INDEX_Y_OFFSET        9

#define VBE_DISPI_DISABLED              0x00
#define VBE_DISPI_ENABLED               0x01
#define VBE_DISPI_GETCAPS               0x02
#define VBE_DISPI_8BIT_DAC              0x20
#define VBE_DISPI_LFB_ENABLED           0x40
#define VBE_DISPI_NOCLEARMEM            0x80

#define VBE_DISPI_ID0                   0xB0C0
#define VBE_DISPI_ID1                   0xB0C1
#define VBE_DISPI_ID2                   0xB0C2
#define VBE_DISPI_ID3                   0xB0C3
#define VBE_DISPI_ID4                   0xB0C4
#define VBE_DISPI_ID5                   0xB0C5

#define BGA_PCI_VENDOR_ID               0x1234
#define BGA_PCI_DEVICE_ID               0x1111

#define BGA_DEFAULT_LFB_ADDR            0xE0000000

typedef struct {
    bool     available;
    uint16_t version;
    uint32_t lfb_addr;
    int      width;
    int      height;
    int      bpp;
} vbe_bga_info_t;

bool     vbe_bga_probe(vbe_bga_info_t *out_info);
bool     vbe_bga_is_available(void);
bool     vbe_bga_is_enabled(void);
uint32_t vbe_bga_get_lfb_address(void);
bool     vbe_bga_set_mode(int width, int height, int bpp);
void     vbe_bga_restore_text_mode(void);
void    *vbe_bga_get_framebuffer(void);
int      vbe_bga_get_width(void);
int      vbe_bga_get_height(void);
int      vbe_bga_get_bpp(void);

#endif /* LIB_DRIVER_VBE_BGA_H */

