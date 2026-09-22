#!/usr/bin/env python3
"""
Backwards-compatibility forwarder to load_applications.py
"""
import sys
from load_applications import main

if __name__ == "__main__":
    raise SystemExit(main())
