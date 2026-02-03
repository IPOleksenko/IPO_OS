#!/usr/bin/env python3
import sys, struct
code_size = len(open(sys.argv[1], 'rb').read())
total = code_size + 20
h = b'IPO_B' + b'\x00\x00\x00' + struct.pack('<II I', 20, total, 0)
open(sys.argv[2], 'wb').write(h)
