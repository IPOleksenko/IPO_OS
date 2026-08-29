#!/usr/bin/env python3
import struct
import sys

IMAGE_BASE = 0x00800000
HEADER_SIZE = 20

code_size = len(open(sys.argv[1], 'rb').read())
entry_address = int(sys.argv[3], 0)
entry_offset = entry_address - IMAGE_BASE
total_size = code_size + HEADER_SIZE

if entry_offset < 0 or entry_offset >= code_size:
	raise SystemExit("main is outside the flat application image")

header = b'IPO_B' + b'\x00\x00\x00' + struct.pack(
	'<III', entry_offset, total_size, 0
)
open(sys.argv[2], 'wb').write(header)
