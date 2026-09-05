#!/usr/bin/env python3
"""Normalize the version resource tree produced by GNU windres.

GNU windres (>= 2.39, <= current in many MinGW toolchains) represents the
RT_VERSION (type 16) subtree with a *named* second-level entry whose name string
is "VS_VERSION_INFO":

    RT_VERSION -> [named] "VS_VERSION_INFO" -> { 1033 -> data }

Windows' version API (GetFileVersionInfoSize / VerQueryValue, and therefore
Explorer's Details/Version tabs and PowerShell's FileVersionInfo) requires a
*numeric* language-id entry at that level instead:

    RT_VERSION -> id 1 -> 1033 -> data

When the malformed tree is linked into an executable, every Windows reader
reports an empty version. Newer binutils windres emits the numeric form
directly, so this fix is a no-op whenever the tree is already correct.

The fix rewrites a single resource directory entry: the high bit bit-clear flag
of the second-level name field is cleared and the parent directory entry counts
are adjusted, which converts the named entry into the numeric id 1 entry without
moving any bytes.

Usage:
    python fix_version_resource.py <coff-object-file>

The file is patched in place.
"""

import struct
import sys

RT_VERSION = 16


def read_counts(b, o):
    return (
        b[o + 12] | b[o + 13] << 8,
        b[o + 14] | b[o + 15] << 8,
    )


def set_counts(b, o, named, ids):
    b[o + 12] = named & 0xFF
    b[o + 13] = (named >> 8) & 0xFF
    b[o + 14] = ids & 0xFF
    b[o + 15] = (ids >> 8) & 0xFF


def entry_name(b, base, name_field):
    """Return the UTF-16 name string for a named directory entry or None."""
    if not (name_field & 0x80000000):
        return None
    off = base + (name_field & 0x7FFFFFFF)
    length = struct.unpack_from("<H", b, off)[0]
    raw = b[off + 2: off + 2 + length * 2]
    try:
        return raw.decode("utf-16le")
    except UnicodeDecodeError:
        return None


def fix_coff(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())

    if data[:2] != b"MZ":
        # Not a PE; assume plain COFF object (windres -O coff output).
        num_sections = struct.unpack_from("<H", data, 2)[0]
        opt_size = struct.unpack_from("<H", data, 16)[0]
        sec_tab = 20 + opt_size
        rsrc = None
        for i in range(num_sections):
            b = sec_tab + i * 40
            name = data[b:b + 8].rstrip(b"\x00")
            vsz, vma, raw_size, raw_ptr = struct.unpack_from("<IIII", data, b + 8)
            if name == b".rsrc":
                rsrc = bytearray(data[raw_ptr:raw_ptr + raw_size])
                break
        if rsrc is None:
            return "no .rsrc section"
        base = 0
    else:
        # PE executable/object: locate .rsrc via the data directory.
        pe_off = struct.unpack_from("<I", data, 0x3C)[0]
        num_sections = struct.unpack_from("<H", data, pe_off + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
        opt_start = pe_off + 24
        dd_rsrc = opt_start + 112 + 16
        rsrc_rva, rsrc_size = struct.unpack_from("<II", data, dd_rsrc)
        sec_tab = opt_start + opt_size
        rsrc = None
        for i in range(num_sections):
            b = sec_tab + i * 40
            name = data[b:b + 8].rstrip(b"\x00")
            vsz, vma, raw_size, raw_ptr = struct.unpack_from("<IIII", data, b + 8)
            if name == b".rsrc":
                file_off = raw_ptr + (rsrc_rva - vma)
                rsrc = bytearray(data[file_off:file_off + raw_size])
                break
        if rsrc is None:
            return "no .rsrc section"
        base = 0

    patched = False
    named_root, id_root = read_counts(rsrc, 0)
    for i in range(named_root + id_root):
        eo = 16 + i * 8
        name_field = struct.unpack_from("<I", rsrc, eo)[0]
        val = struct.unpack_from("<I", rsrc, eo + 4)[0]
        if name_field & 0x80000000:
            continue
        if name_field != RT_VERSION or not (val & 0x80000000):
            continue
        sub = val & 0x7FFFFFFF
        named2, id2 = read_counts(rsrc, sub)
        if named2 != 1 or id2 != 0:
            continue
        e2 = sub + 16
        n2 = struct.unpack_from("<I", rsrc, e2)[0]
        if entry_name(rsrc, base, n2) != "VS_VERSION_INFO":
            continue
        struct.pack_into("<I", rsrc, e2, 1)  # numeric id 1
        set_counts(rsrc, sub, 0, 1)
        print("fixed RT_VERSION second-level entry (named -> id 1)")
        patched = True

    if not patched:
        return "no-op (resource tree already correct)"

    if data[:2] != b"MZ":
        # Write back into COFF .rsrc section.
        for i in range(num_sections):
            b = sec_tab + i * 40
            name = data[b:b + 8].rstrip(b"\x00")
            if name == b".rsrc":
                vsz, vma, raw_size, raw_ptr = struct.unpack_from("<IIII", data, b + 8)
                data[raw_ptr:raw_ptr + raw_size] = rsrc
                break
    else:
        file_off = raw_ptr + (rsrc_rva - vma)
        data[file_off:file_off + raw_size] = rsrc

    with open(path, "wb") as f:
        f.write(data)
    return "patched"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: fix_version_resource.py <resource-object>", file=sys.stderr)
        sys.exit(2)
    print(fix_coff(sys.argv[1]))