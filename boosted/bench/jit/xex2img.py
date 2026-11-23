#!/usr/bin/env python3
"""hakuX-Boosted jit bench: turns a homebrew .xex into the image and manifest
core/bench_jit.cc loads, so the translator can be measured on a whole
executable of compiled code without the kernel, the loader or a title.

    xex2img.py <file.xex> <output directory> [name]

Only what xsdk's elf2xex.py writes is read: XEX2, not encrypted, "basic"
compression (blocks of data with runs of zeros left out). The functions
listed are the entry point and every target of a `bl` inside the code
sections whose first word is an instruction, which is what a title's code
reaches by direct calls. Nothing in the image is run by the benchmark: its
imports are not bound.
"""
import os
import struct
import sys

HEADER_FILE_FORMAT_INFO = 0x000003FF
HEADER_ENTRY_POINT = 0x00010100
HEADER_IMAGE_BASE_ADDRESS = 0x00010201
PE_SECTION_CODE = 0x00000020


def fail(message):
    sys.exit("xex2img: " + message)


def main():
    path, out = sys.argv[1], sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else os.path.splitext(os.path.basename(path))[0]
    data = open(path, "rb").read()
    magic, _flags, data_offset, _reserved, security_offset, header_count = struct.unpack_from(">4sIIIII", data, 0)
    if magic != b"XEX2":
        fail(path + " is not an XEX2 file")
    headers = {}
    for i in range(header_count):
        key, value = struct.unpack_from(">II", data, 24 + i * 8)
        headers[key] = value
    image_size = struct.unpack_from(">I", data, security_offset + 4)[0]
    base = struct.unpack_from(">I", data, security_offset + 0x110)[0]
    base = headers.get(HEADER_IMAGE_BASE_ADDRESS, base)
    entry = headers.get(HEADER_ENTRY_POINT, 0)

    if HEADER_FILE_FORMAT_INFO not in headers:
        fail("no file format header")
    at = headers[HEADER_FILE_FORMAT_INFO]
    info_size, encryption, compression = struct.unpack_from(">IHH", data, at)
    if encryption != 0 or compression != 1:
        fail("%s: encryption %d, compression %d; only plain files with basic compression are read"
             % (path, encryption, compression))
    image = bytearray()
    cursor = data_offset
    for i in range((info_size - 8) // 8):
        data_size, zero_size = struct.unpack_from(">II", data, at + 8 + i * 8)
        image += data[cursor:cursor + data_size]
        image += bytes(zero_size)
        cursor += data_size
    image += bytes(max(0, image_size - len(image)))

    # The PE headers at the start of the image give the code sections.
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe:pe + 4] != b"PE\0\0":
        fail("no PE header in the image")
    section_count, optional_size = struct.unpack_from("<H", image, pe + 6)[0], struct.unpack_from("<H", image, pe + 20)[0]
    code = []
    for i in range(section_count):
        _name, vsize, vaddr, _rsize, _rptr, _a, _b, _c, _d, flags = struct.unpack_from(
            "<8sIIIIIIHHI", image, pe + 24 + optional_size + i * 40)
        if flags & PE_SECTION_CODE:
            code.append((vaddr, min(vaddr + vsize, len(image))))
    if not code:
        fail("no code section")

    def is_code(address):
        offset = address - base
        return any(start <= offset < end for start, end in code) and offset % 4 == 0

    def word(address):
        return struct.unpack_from(">I", image, address - base)[0]

    functions = set()
    if entry and is_code(entry):
        functions.add(entry)
    for start, end in code:
        for offset in range(start, end - 3, 4):
            w = struct.unpack_from(">I", image, offset)[0]
            if w >> 26 == 18 and w & 3 == 1:  # bl, relative
                disp = w & 0x03FFFFFC
                if disp & 0x02000000:
                    disp -= 0x04000000
                target = base + offset + disp
                # An import thunk's slot holds a record, not code, until the
                # loader binds it: its first byte is 1 or 2, no instruction.
                if is_code(target) and word(target) >> 26 != 0:
                    functions.add(target)

    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, name + ".img"), "wb") as f:
        f.write(image)
    with open(os.path.join(out, name + ".manifest"), "w") as m:
        m.write("image %s\nfile %s.img\n" % (name, name))
        m.write("base %08X\nfile_base %08X\nsize %X\n" % (base, base, len(image)))
        for start, end in code:
            m.write("text %08X %08X\n" % (base + start, base + end))
        for address in sorted(functions):
            m.write("fn %08X\n" % address)
    print("%s: %d functions, %d bytes of code sections" % (name, len(functions), sum(e - s for s, e in code)))


if __name__ == "__main__":
    main()
