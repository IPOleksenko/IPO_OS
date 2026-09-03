#!/usr/bin/env python3
import gzip
import struct
from pathlib import Path

def extract_psf1(path):
    with gzip.open(path, 'rb') as f:
        data = f.read()

    magic, mode, charsize = struct.unpack('<HBB', data[:4])
    num_glyphs = 512 if (mode & 0x01) else 256
    glyph_data = data[4 : 4 + num_glyphs * charsize]
    table_data = data[4 + num_glyphs * charsize :]

    codepoints = {}
    ptr = 0
    for g in range(num_glyphs):
        bmp = glyph_data[g * charsize : (g + 1) * charsize]
        while ptr < len(table_data):
            val = struct.unpack('<H', table_data[ptr : ptr + 2])[0]
            ptr += 2
            if val == 0xFFFF: # separator between glyphs
                break
            elif val == 0xFFFE: # sequence separator
                pass
            else:
                codepoints[val] = bmp
    return codepoints

def extract_pcf_hanzi(path):
    with gzip.open(path, 'rb') as f:
        data = f.read()

    table_count = struct.unpack('<I', data[4:8])[0]
    tables = {
        struct.unpack('<IIII', data[8+i*16:24+i*16])[0]: struct.unpack('<IIII', data[8+i*16:24+i*16])[1:]
        for i in range(table_count)
    }

    # PCF_BITMAPS (8)
    bfmt, bsz, boff = tables[8]
    bendian = '>' if (bfmt & 4) else '<'
    num_glyphs = struct.unpack(f'{bendian}I', data[boff+4:boff+8])[0]
    offsets = struct.unpack(f'{bendian}{num_glyphs}I', data[boff+8:boff+8+num_glyphs*4])
    bdata_start = boff + 8 + num_glyphs*4 + 16

    # PCF_BDF_ENCODINGS (32)
    efmt, esz, eoff = tables[32]
    eendian = '>' if (efmt & 4) else '<'
    b2_min, b2_max, b1_min, b1_max, def_c = struct.unpack(f'{eendian}HHHHH', data[eoff+4:eoff+14])
    num_enc = (b2_max - b2_min + 1) * (b1_max - b1_min + 1)
    indices = struct.unpack(f'{eendian}{num_enc}H', data[eoff+14:eoff+14+num_enc*2])

    hanzi_8x16 = {}
    for b1 in range(b1_min, b1_max + 1):
        for b2 in range(b2_min, b2_max + 1):
            idx = (b1 - b1_min) * (b2_max - b2_min + 1) + (b2 - b2_min)
            glyph_idx = indices[idx]
            if glyph_idx == 0xFFFF or glyph_idx >= num_glyphs:
                continue
            try:
                ch = bytes([b1 + 0x80, b2 + 0x80]).decode('gb2312')
            except UnicodeDecodeError:
                continue

            goff = bdata_start + offsets[glyph_idx]
            rows16 = [struct.unpack(f'{bendian}H', data[goff + r*4 : goff + r*4 + 2])[0] for r in range(16)]

            # Downsample 16x16 -> 8x16 (2 pixels to 1)
            bmp8 = bytearray(16)
            for r in range(16):
                val = 0
                for col in range(8):
                    if (rows16[r] & (3 << (14 - 2 * col))):
                        val |= (1 << (7 - col))
                bmp8[r] = val

            hanzi_8x16[ord(ch)] = bytes(bmp8)

    return hanzi_8x16

def main():
    out_dir = Path("build/system")
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / "fonts.bin"

    print("Extracting Cyrillic console font...")
    cyr = extract_psf1("/usr/share/consolefonts/FullCyrSlav-Fixed16.psf.gz")
    print(f"Loaded {len(cyr)} Cyrillic/Slavic glyphs")

    print("Extracting Chinese Hanzi font...")
    hanzi = extract_pcf_hanzi("/usr/share/fonts/X11/misc/gb16fs.pcf.gz")
    print(f"Loaded {len(hanzi)} Chinese Hanzi glyphs")

    # Combine into single map
    combined = {}
    for cp, bmp in cyr.items():
        if cp >= 0x80: # Skip standard ASCII to save space
            combined[cp] = bmp

    for cp, bmp in hanzi.items():
        combined[cp] = bmp

    sorted_records = sorted(combined.items(), key=lambda x: x[0])
    count = len(sorted_records)
    print(f"Total packed font records: {count}")

    # File format:
    # Magic: "IFNT" (4 bytes)
    # Count: uint32 (4 bytes)
    # Records: count * (codepoint: uint32, bitmap: 16 bytes)
    with open(out_file, "wb") as f:
        f.write(b"IFNT")
        f.write(struct.pack("<I", count))
        for cp, bmp in sorted_records:
            f.write(struct.pack("<I", cp))
            f.write(bmp[:16])

    print(f"Generated {out_file} ({out_file.stat().st_size} bytes)")

if __name__ == "__main__":
    main()
