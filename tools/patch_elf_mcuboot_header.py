#!/usr/bin/env python3
"""Patch MCUboot image header into Zephyr ELF so GDB flashing preserves the header."""
import struct
import sys

elf_path = sys.argv[1]
header_path = sys.argv[2]
header_size = int(sys.argv[3], 16)

with open(header_path, "rb") as f:
    header = f.read(header_size)

with open(elf_path, "r+b") as f:
    elf = bytearray(f.read())

# Parse 32-bit little-endian ELF program headers
e_phoff = struct.unpack_from("<I", elf, 0x1C)[0]
e_phentsize = struct.unpack_from("<H", elf, 0x2A)[0]
e_phnum = struct.unpack_from("<H", elf, 0x2C)[0]

patched = False
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from("<IIIII", elf, off)
    if p_type == 1 and p_filesz >= header_size:  # PT_LOAD
        elf[p_offset : p_offset + header_size] = header
        print(f"Patched MCUboot header at ELF offset {hex(p_offset)} (paddr={hex(p_paddr)})")
        patched = True
        break

if not patched:
    print("WARNING: No suitable PT_LOAD segment found; ELF not patched", file=sys.stderr)
    sys.exit(1)

with open(elf_path, "wb") as f:
    f.write(elf)
