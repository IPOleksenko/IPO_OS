#include <driver/net/pci.h>
#include <ioport.h>
#include <memory/kmalloc.h>
#include <stdio.h>
#include <string.h>

static pci_device_t *s_pci_devices = NULL;
static uint32_t s_pci_count = 0;
static uint32_t s_pci_capacity = 0;

uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((1u << 31) |
                                 ((uint32_t)bus << 16) |
                                 ((uint32_t)slot << 11) |
                                 ((uint32_t)func << 8) |
                                 (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_read_dword(bus, slot, func, offset);
    return (uint16_t)((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_read_dword(bus, slot, func, offset);
    return (uint8_t)((dword >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = (uint32_t)((1u << 31) |
                                 ((uint32_t)bus << 16) |
                                 ((uint32_t)slot << 11) |
                                 ((uint32_t)func << 8) |
                                 (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, val);
}

void pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t dword = pci_read_dword(bus, slot, func, offset);
    uint32_t shift = (offset & 2) * 8;
    dword = (dword & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    pci_write_dword(bus, slot, func, offset, dword);
}

void pci_write_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t val) {
    uint32_t dword = pci_read_dword(bus, slot, func, offset);
    uint32_t shift = (offset & 3) * 8;
    dword = (dword & ~(0xFFu << shift)) | ((uint32_t)val << shift);
    pci_write_dword(bus, slot, func, offset, dword);
}

static char *pci_dup_clean_ascii(const uint8_t *raw, size_t max_len) {
    if (!raw || max_len == 0) return NULL;
    while (max_len > 0 && (*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n')) {
        raw++;
        max_len--;
    }
    size_t len = 0;
    while (len < max_len && raw[len] != '\0' && raw[len] != '\r' && raw[len] != '\n') {
        if (raw[len] < 0x20 || raw[len] > 0x7E) {
            return NULL;
        }
        len++;
    }
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t')) {
        len--;
    }
    if (len == 0) return NULL;
    char *out = (char *)kmalloc(len + 1);
    if (!out) return NULL;
    memcpy(out, raw, len);
    out[len] = '\0';
    return out;
}

static void pci_set_str(char **field, char *new_str) {
    if (!new_str) return;
    if (*field) {
        kfree(new_str);
        return;
    }
    *field = new_str;
}

/* Parse VPD (Vital Product Data) buffer for Identifier String (0x82) and VPD-R keywords (0x90: MN, PN) */
static void pci_parse_vpd_bytes(const uint8_t *vpd, size_t len, pci_device_t *dev) {
    size_t pos = 0;
    while (pos < len) {
        uint8_t tag = vpd[pos];
        if (tag == 0x00 || tag == 0xFF || tag == 0x78) break;
        if (tag & 0x80) {
            if (pos + 3 > len) break;
            uint16_t item_len = (uint16_t)vpd[pos + 1] | ((uint16_t)vpd[pos + 2] << 8);
            pos += 3;
            if (pos + item_len > len) break;
            if (tag == 0x82) {
                /* Identifier String Descriptor */
                pci_set_str(&dev->device_name, pci_dup_clean_ascii(vpd + pos, item_len));
            } else if (tag == 0x90) {
                /* VPD-R Read-Only Fields */
                size_t fpos = 0;
                while (fpos + 3 <= item_len) {
                    uint8_t k0 = vpd[pos + fpos];
                    uint8_t k1 = vpd[pos + fpos + 1];
                    uint8_t flen = vpd[pos + fpos + 2];
                    fpos += 3;
                    if (fpos + flen > item_len) break;
                    if (k0 == 'M' && k1 == 'N') {
                        pci_set_str(&dev->vendor_name, pci_dup_clean_ascii(vpd + pos + fpos, flen));
                    } else if (k0 == 'P' && k1 == 'N' && !dev->device_name) {
                        pci_set_str(&dev->device_name, pci_dup_clean_ascii(vpd + pos + fpos, flen));
                    }
                    fpos += flen;
                }
            }
            pos += item_len;
        } else {
            uint8_t item_len = tag & 0x07;
            pos += 1 + item_len;
        }
    }
}

/* Probe PCI Vital Product Data (VPD) capability (Cap ID 0x03) directly via PCI config space */
static void pci_probe_vpd_capability(pci_device_t *dev) {
    uint16_t status = pci_read_word(dev->bus, dev->slot, dev->func, 0x06);
    if (!(status & 0x0010u)) return; /* No Capabilities List */

    uint8_t cap_ptr = pci_read_byte(dev->bus, dev->slot, dev->func, 0x34) & ~3u;
    int guard = 48;
    while (cap_ptr >= 0x40 && guard-- > 0) {
        uint8_t cap_id = pci_read_byte(dev->bus, dev->slot, dev->func, cap_ptr);
        uint8_t next_ptr = pci_read_byte(dev->bus, dev->slot, dev->func, cap_ptr + 1) & ~3u;
        if (cap_id == 0x03) {
            size_t max_vpd = 256;
            uint8_t *vpd_buf = (uint8_t *)kmalloc(max_vpd);
            if (!vpd_buf) return;
            size_t read_bytes = 0;
            for (uint16_t addr = 0; addr < max_vpd; addr += 4) {
                pci_write_word(dev->bus, dev->slot, dev->func, cap_ptr + 2, addr & 0x7FFCu);
                int timeout = 10000;
                uint16_t vpd_addr = 0;
                while (timeout-- > 0) {
                    vpd_addr = pci_read_word(dev->bus, dev->slot, dev->func, cap_ptr + 2);
                    if (vpd_addr & 0x8000u) break;
                }
                if (!(vpd_addr & 0x8000u)) break;
                uint32_t dword = pci_read_dword(dev->bus, dev->slot, dev->func, cap_ptr + 4);
                *(uint32_t *)(vpd_buf + addr) = dword;
                read_bytes += 4;
                if ((dword & 0xFF) == 0x78 || dword == 0xFFFFFFFFu) break;
            }
            if (read_bytes > 0) {
                pci_parse_vpd_bytes(vpd_buf, read_bytes, dev);
            }
            kfree(vpd_buf);
            return;
        }
        cap_ptr = next_ptr;
    }
}

/* Inspect a PCI Option ROM image (either shadowed in 0xC0000..0xEFFFF or mapped via Expansion ROM BAR) */
static void pci_parse_rom_image(const uint8_t *rom, size_t max_len, pci_device_t *dev, bool direct_bar) {
    if (!rom || max_len < 64) return;
    if (rom[0] != 0x55 || rom[1] != 0xAA) return;

    size_t rom_len = (size_t)rom[2] * 512u;
    if (rom_len == 0 || rom_len > max_len) rom_len = max_len;

    uint16_t pcir_off = *(const uint16_t *)(rom + 0x18);
    uint16_t pnp_off  = *(const uint16_t *)(rom + 0x1A);

    bool matches = direct_bar;
    uint16_t vpd_off = 0;

    if (pcir_off >= 0x1C && (size_t)pcir_off + 0x18 <= rom_len &&
        memcmp(rom + pcir_off, "PCIR", 4) == 0) {
        uint16_t rom_ven = *(const uint16_t *)(rom + pcir_off + 0x04);
        uint16_t rom_dev = *(const uint16_t *)(rom + pcir_off + 0x06);
        vpd_off          = *(const uint16_t *)(rom + pcir_off + 0x08);
        uint8_t rom_cls  = rom[pcir_off + 0x0F];
        uint8_t rom_sub  = rom[pcir_off + 0x0E];

        if (rom_ven == dev->vendor_id && rom_dev == dev->device_id) {
            matches = true;
        } else if (!direct_bar && dev->class_code == 0x03 && rom_cls == 0x03 && rom_sub == dev->subclass) {
            matches = true;
        }
    }

    if (!matches) return;

    /* 1. Extract strings from $PnP Expansion Header if present (PnP BIOS Spec v1.0A Table 3-3: 0x0E=Mfg, 0x10=Product) */
    if (pnp_off >= 0x1C && (size_t)pnp_off + 0x14 <= rom_len &&
        memcmp(rom + pnp_off, "$PnP", 4) == 0) {
        uint16_t mfg_off = *(const uint16_t *)(rom + pnp_off + 0x0E);
        uint16_t prd_off = *(const uint16_t *)(rom + pnp_off + 0x10);
        if (mfg_off > 0 && mfg_off < rom_len) {
            pci_set_str(&dev->vendor_name, pci_dup_clean_ascii(rom + mfg_off, rom_len - mfg_off));
        }
        if (prd_off > 0 && prd_off < rom_len) {
            pci_set_str(&dev->device_name, pci_dup_clean_ascii(rom + prd_off, rom_len - prd_off));
        }
    }

    /* 2. Extract strings from ROM VPD if present */
    if (vpd_off > 0 && vpd_off < rom_len) {
        pci_parse_vpd_bytes(rom + vpd_off, rom_len - vpd_off, dev);
    }

    /* 3. For VGA/Display Option ROMs, extract VBE 2.0 OemVendorNamePtr (0x16), OemProductNamePtr (0x1A), and OemStringPtr (0x06) */
    if (dev->class_code == 0x03 && (!dev->vendor_name || !dev->device_name)) {
        for (size_t i = 0; i + 16 < rom_len; i++) {
            if (rom[i] == 'V' && rom[i + 1] == 'E' && rom[i + 2] == 'S' && rom[i + 3] == 'A') {
                size_t wend = (i + 180 < rom_len) ? (i + 180) : rom_len;
                uint32_t last_imm = 0;
                for (size_t k = i + 4; k + 4 < wend; k++) {
                    if (rom[k] == 0x66 && (rom[k + 1] >= 0xB8 && rom[k + 1] <= 0xBB) && k + 6 <= wend) {
                        uint32_t imm = *(const uint32_t *)(rom + k + 2);
                        if (imm >= 0x20 && imm < rom_len) {
                            last_imm = imm;
                        }
                        k += 5;
                        continue;
                    }
                    if (rom[k] == 0x89 && k + 2 < wend && (rom[k + 1] & 0xC0) == 0x40) {
                        uint8_t disp = rom[k + 2];
                        if (last_imm >= 0x20 && last_imm < rom_len) {
                            if (disp == 0x16 && !dev->vendor_name) {
                                pci_set_str(&dev->vendor_name, pci_dup_clean_ascii(rom + last_imm, rom_len - last_imm));
                            } else if (disp == 0x1A && !dev->device_name) {
                                pci_set_str(&dev->device_name, pci_dup_clean_ascii(rom + last_imm, rom_len - last_imm));
                            } else if (disp == 0x06 && !dev->device_name) {
                                pci_set_str(&dev->device_name, pci_dup_clean_ascii(rom + last_imm, rom_len - last_imm));
                            }
                        }
                    }
                }
                if (dev->vendor_name || dev->device_name) break;
            }
        }
    }

    /* 4. If device_name is still unknown, scan the early ROM header (0x1E..0x180) for an ASCII banner */
    if (!dev->device_name) {
        size_t scan_limit = (rom_len < 0x180) ? rom_len : 0x180;
        for (size_t i = 0x1E; i + 8 < scan_limit; i++) {
            if (i >= pcir_off && i < (size_t)pcir_off + 0x18) continue;
            if (pnp_off && i >= pnp_off && i < (size_t)pnp_off + 0x20) continue;
            size_t slen = 0;
            bool has_alpha = false;
            while (i + slen < scan_limit && rom[i + slen] >= 0x20 && rom[i + slen] <= 0x7E) {
                char c = (char)rom[i + slen];
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) has_alpha = true;
                slen++;
            }
            if (has_alpha && slen >= 8 && (i + slen < scan_limit) &&
                (rom[i + slen] == '\0' || rom[i + slen] == '\r' || rom[i + slen] == '\n')) {
                pci_set_str(&dev->device_name, pci_dup_clean_ascii(rom + i, slen));
                break;
            }
            if (slen > 0) i += slen;
        }
    }
}

/* Probe shadowed Option ROMs in physical 0xC0000..0xEFFFF and the device's own Expansion ROM BAR */
static void pci_probe_option_roms(pci_device_t *dev) {
    /* 1. Check shadowed Option ROM region 0xC0000 - 0xEFFFF */
    for (uint32_t addr = 0xC0000u; addr < 0xF0000u; addr += 0x800u) {
        const uint8_t *rom = (const uint8_t *)(uintptr_t)addr;
        if (rom[0] == 0x55 && rom[1] == 0xAA) {
            pci_parse_rom_image(rom, 0xF0000u - addr, dev, false);
            if (dev->vendor_name && dev->device_name) return;
        }
    }

    /* 2. Check the PCI device's own Expansion ROM BAR directly in hardware */
    uint8_t hdr = dev->header_type & 0x7Fu;
    uint8_t rom_reg = (hdr == 0x00) ? 0x30 : ((hdr == 0x01) ? 0x38 : 0);
    if (rom_reg == 0) return;

    uint32_t orig_rom = pci_read_dword(dev->bus, dev->slot, dev->func, rom_reg);
    pci_write_dword(dev->bus, dev->slot, dev->func, rom_reg, 0xFFFFF800u);
    uint32_t mask = pci_read_dword(dev->bus, dev->slot, dev->func, rom_reg);
    pci_write_dword(dev->bus, dev->slot, dev->func, rom_reg, orig_rom);

    if (mask == 0 || mask == 0xFFFFFFFFu || (mask & ~0x7FFu) == 0) return;

    uint32_t rom_size = ~(mask & ~0x7FFu) + 1u;
    if (rom_size == 0 || rom_size > 0x100000u) return;

    uint32_t map_addr = orig_rom & ~0x7FFu;
    if (map_addr < 0x01000000u || map_addr >= 0xFFE00000u) {
        map_addr = 0xFE800000u;
    }

    uint16_t orig_cmd = pci_read_word(dev->bus, dev->slot, dev->func, 0x04);
    pci_write_dword(dev->bus, dev->slot, dev->func, rom_reg, map_addr | 1u);
    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, orig_cmd | 0x0002u);

    const uint8_t *mapped_rom = (const uint8_t *)(uintptr_t)map_addr;
    if (mapped_rom[0] == 0x55 && mapped_rom[1] == 0xAA) {
        pci_parse_rom_image(mapped_rom, rom_size, dev, true);
    }

    pci_write_dword(dev->bus, dev->slot, dev->func, rom_reg, orig_rom);
    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, orig_cmd);
}

/* Probe SMBIOS Type 41 (Onboard Devices Extended Information) in 0xF0000..0xFFFFF */
static void pci_probe_smbios(pci_device_t *dev) {
    for (uint32_t addr = 0xF0000u; addr < 0x100000u; addr += 16) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
        if (memcmp(p, "_SM_", 4) != 0) continue;
        uint8_t ep_len = p[5];
        if (ep_len < 0x1E || addr + ep_len > 0x100000u) continue;
        uint8_t sum = 0;
        for (uint8_t i = 0; i < ep_len; i++) sum = (uint8_t)(sum + p[i]);
        if (sum != 0) continue;

        uint16_t tbl_len = *(const uint16_t *)(p + 0x16);
        uint32_t tbl_addr = *(const uint32_t *)(p + 0x18);
        uint16_t num_structs = *(const uint16_t *)(p + 0x1C);
        if (tbl_addr == 0 || tbl_len == 0) break;

        const uint8_t *cur = (const uint8_t *)(uintptr_t)tbl_addr;
        const uint8_t *end = cur + tbl_len;
        for (uint16_t s = 0; s < num_structs && cur + 4 <= end; s++) {
            uint8_t type = cur[0];
            uint8_t hlen = cur[1];
            if (hlen < 4 || cur + hlen > end) break;
            const uint8_t *str_area = cur + hlen;
            const uint8_t *next = str_area;
            while (next + 1 < end && !(next[0] == 0 && next[1] == 0)) next++;
            next += 2;

            if (type == 41 && hlen >= 11) {
                uint8_t str_idx = cur[4];
                uint8_t bus_num = cur[9];
                uint8_t dev_fn  = cur[10];
                if (bus_num == dev->bus && dev_fn == ((dev->slot << 3) | dev->func) && str_idx > 0) {
                    const uint8_t *sp = str_area;
                    uint8_t idx = 1;
                    while (sp < next - 2 && idx < str_idx) {
                        while (sp < next - 2 && *sp != 0) sp++;
                        sp++;
                        idx++;
                    }
                    if (idx == str_idx && sp < next - 2 && *sp != 0) {
                        pci_set_str(&dev->device_name, pci_dup_clean_ascii(sp, (size_t)(next - sp)));
                    }
                }
            }
            if (type == 127) break;
            cur = next;
        }
        break;
    }
}

static void pci_probe_hardware_strings(pci_device_t *dev) {
    pci_probe_vpd_capability(dev);
    pci_probe_option_roms(dev);
    pci_probe_smbios(dev);
}

static bool pci_store_device(const pci_device_t *dev) {
    if (s_pci_count >= s_pci_capacity) {
        uint32_t new_cap = (s_pci_capacity == 0) ? 8 : s_pci_capacity * 2;
        pci_device_t *new_arr = (pci_device_t *)kmalloc(new_cap * sizeof(pci_device_t));
        if (!new_arr) return false;
        if (s_pci_devices && s_pci_count > 0) {
            memcpy(new_arr, s_pci_devices, s_pci_count * sizeof(pci_device_t));
            kfree(s_pci_devices);
        }
        s_pci_devices = new_arr;
        s_pci_capacity = new_cap;
    }
    s_pci_devices[s_pci_count++] = *dev;
    return true;
}

static bool pci_device_add_bar(pci_device_t *dev, uint32_t val) {
    if (!dev) return false;
    if (dev->bar_count >= dev->bar_cap) {
        size_t new_cap = (dev->bar_cap == 0) ? 4 : (dev->bar_cap * 2);
        uint32_t *new_bar = (uint32_t *)krealloc(dev->bar, new_cap * sizeof(uint32_t));
        if (!new_bar) return false;
        dev->bar = new_bar;
        dev->bar_cap = new_cap;
    }
    dev->bar[dev->bar_count++] = val;
    return true;
}

uint32_t pci_scan_all(void (*callback)(const pci_device_t *dev, void *user_data), void *user_data) {
    /* Free any previously allocated hardware strings and BAR arrays before rescan */
    for (uint32_t i = 0; i < s_pci_count; i++) {
        if (s_pci_devices[i].vendor_name) {
            kfree(s_pci_devices[i].vendor_name);
            s_pci_devices[i].vendor_name = NULL;
        }
        if (s_pci_devices[i].device_name) {
            kfree(s_pci_devices[i].device_name);
            s_pci_devices[i].device_name = NULL;
        }
        if (s_pci_devices[i].bar) {
            kfree(s_pci_devices[i].bar);
            s_pci_devices[i].bar = NULL;
        }
        s_pci_devices[i].bar_count = 0;
        s_pci_devices[i].bar_cap = 0;
    }
    s_pci_count = 0;

    /* Scan up to 256 buses, stopping on early unused high buses */
    for (uint16_t bus = 0; bus < 256; bus++) {
        bool bus_has_device = false;

        for (uint8_t slot = 0; slot < 32; slot++) {
            /* Check function 0 to determine if device is present */
            uint16_t ven = pci_read_word((uint8_t)bus, slot, 0, 0x00);
            if (ven == 0xFFFF || ven == 0x0000) {
                continue;
            }

            bus_has_device = true;
            uint8_t header_type = pci_read_byte((uint8_t)bus, slot, 0, 0x0E);
            uint8_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                uint16_t func_ven = pci_read_word((uint8_t)bus, slot, func, 0x00);
                if (func_ven == 0xFFFF || func_ven == 0x0000) {
                    continue;
                }

                pci_device_t dev;
                memset(&dev, 0, sizeof(dev));
                dev.bus = (uint8_t)bus;
                dev.slot = slot;
                dev.func = func;
                dev.vendor_id = func_ven;
                dev.device_id = pci_read_word((uint8_t)bus, slot, func, 0x02);
                dev.revision_id = pci_read_byte((uint8_t)bus, slot, func, 0x08);

                uint16_t class_rev = pci_read_word((uint8_t)bus, slot, func, 0x0A);
                dev.class_code = (uint8_t)(class_rev >> 8);
                dev.subclass   = (uint8_t)(class_rev & 0xFF);
                dev.prog_if    = pci_read_byte((uint8_t)bus, slot, func, 0x09);
                dev.header_type= pci_read_byte((uint8_t)bus, slot, func, 0x0E);

                uint8_t hdr_kind = dev.header_type & 0x7Fu;
                if (hdr_kind == 0x00) {
                    dev.subsys_vendor_id = pci_read_word((uint8_t)bus, slot, func, 0x2C);
                    dev.subsys_id        = pci_read_word((uint8_t)bus, slot, func, 0x2E);
                }

                uint8_t bar_end_off = (hdr_kind == 0x00) ? 0x28 : ((hdr_kind == 0x01) ? 0x18 : 0x14);
                for (uint8_t off = 0x10; off < bar_end_off; off += 4) {
                    pci_device_add_bar(&dev, pci_read_dword((uint8_t)bus, slot, func, off));
                }
                dev.bar0 = (dev.bar_count > 0 && dev.bar) ? dev.bar[0] : 0;
                dev.irq = pci_read_byte((uint8_t)bus, slot, func, 0x3C);

                pci_probe_hardware_strings(&dev);
                pci_store_device(&dev);

                if (callback) {
                    callback(&dev, user_data);
                }
            }
        }

        /* If bus 0 had devices but an empty higher bus is encountered, check if we can stop */
        if (!bus_has_device && bus > 1) {
            /* Check if any bridge previously detected points to a higher bus */
            bool higher_bus_expected = false;
            for (uint32_t i = 0; i < s_pci_count; i++) {
                if (s_pci_devices[i].class_code == 0x06 && s_pci_devices[i].subclass == 0x04) {
                    uint8_t sec_bus = pci_read_byte(s_pci_devices[i].bus, s_pci_devices[i].slot, s_pci_devices[i].func, 0x19);
                    if (sec_bus >= bus) {
                        higher_bus_expected = true;
                        break;
                    }
                }
            }
            if (!higher_bus_expected) {
                break;
            }
        }
    }

    return s_pci_count;
}

uint32_t pci_get_count(void) {
    if (s_pci_count == 0) {
        pci_scan_all(NULL, NULL);
    }
    return s_pci_count;
}

const pci_device_t *pci_get_device(uint32_t index) {
    if (s_pci_count == 0) {
        pci_scan_all(NULL, NULL);
    }
    if (index >= s_pci_count) {
        return NULL;
    }
    return &s_pci_devices[index];
}

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out_dev) {
    if (s_pci_count == 0) {
        pci_scan_all(NULL, NULL);
    }

    for (uint32_t i = 0; i < s_pci_count; i++) {
        if (s_pci_devices[i].vendor_id == vendor_id && s_pci_devices[i].device_id == device_id) {
            if (out_dev) {
                *out_dev = s_pci_devices[i];
            }
            return true;
        }
    }
    return false;
}

void pci_enable_bus_mastering(const pci_device_t *dev) {
    if (!dev) return;
    uint16_t cmd = pci_read_word(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x0005; // Bit 0: I/O Space, Bit 2: Bus Master
    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

