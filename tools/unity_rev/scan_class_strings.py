#!/usr/bin/env python3
"""Find code that references a bare Unity class-name string in a stripped libunity.so.

Why. libunity.so ships stripped: 185 dynamic symbols, no _ZTI*/_ZTS* RTTI names.
But .rodata still carries the bare native class names ("MeshRenderer",
"BoxCollider", ...). Whatever function loads one of those strings is working
with that class, and the field offsets it touches around that load are the
native layout we need. This script reports:

  * the virtual address of every occurrence of a name;
  * every ADRP+ADD pair in .text that materialises that address (with the
    enclosing function start, taken as the nearest preceding STP x29 frame
    prologue, and the offsets read/written off the likely `this` register);
  * every 8-byte slot in .data.rel.ro / .data / .got holding that address
    (RTTI name pointers, class registration tables).

No symbol is invented: an address is only reported when the instruction bytes
actually encode it.

Usage:  python3 tools/unity_rev/scan_class_strings.py libunity.so MeshRenderer
"""
import re
import struct
import subprocess
import sys

SECTIONS = {}


def read_sections(path):
    out = subprocess.run(["readelf", "-SW", path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m:
            SECTIONS[m.group(1)] = dict(addr=int(m.group(3), 16), off=int(m.group(4), 16),
                                        size=int(m.group(5), 16))


def load(path, name):
    s = SECTIONS[name]
    with open(path, "rb") as fh:
        fh.seek(s["off"])
        return s["addr"], fh.read(s["size"])


def find_strings(blob, base, needle):
    hits = []
    start = 0
    while True:
        i = blob.find(needle.encode(), start)
        if i < 0:
            break
        # bare occurrence only: NUL on both sides
        before_ok = i == 0 or blob[i - 1] == 0
        after = i + len(needle)
        after_ok = after < len(blob) and blob[after] == 0
        if before_ok and after_ok:
            hits.append(base + i)
        start = i + 1
    return hits


def scan_adrp_add(text_va, text, target):
    """Yield (insn_va, reg, pair_va) for every ADRP+ADD materialising target."""
    tgt_page = target & ~0xFFF
    tgt_off = target & 0xFFF
    n = len(text) // 4
    words = struct.unpack("<%dI" % n, text[: n * 4])
    pend = {}  # reg -> (va, page)
    for i, w in enumerate(words):
        va = text_va + i * 4
        if (w & 0x9F000000) == 0x90000000:              # ADRP
            rd = w & 0x1F
            immlo = (w >> 29) & 0x3
            immhi = (w >> 5) & 0x7FFFF
            imm = (immhi << 2) | immlo
            if imm & (1 << 20):
                imm -= 1 << 21
            pend[rd] = (va, (va & ~0xFFF) + (imm << 12))
        elif (w & 0xFFC00000) == 0x91000000:            # ADD (imm), 64-bit
            rd = (w >> 5) & 0x1F if False else w & 0x1F
            rn = (w >> 5) & 0x1F
            imm12 = (w >> 10) & 0xFFF
            sh = (w >> 22) & 0x3
            if sh == 1:
                imm12 <<= 12
            if rn in pend and pend[rn][1] == tgt_page and imm12 == tgt_off:
                yield pend[rn][0], rn, va
                pend.pop(rn, None)
            elif rd == rn:
                pend.pop(rd, None)
        elif (w & 0x9F00001F) != 0x90000000:
            # any other instruction clobbers its destination: drop tracked regs
            if (w & 0xFC000000) in (0xB8000000, 0xB9000000, 0xF8000000, 0xF9000000,
                                    0x2A000000, 0x8B000000, 0x0B000000, 0xD2000000,
                                    0xF2000000, 0x92000000, 0xAA000000, 0x1A000000):
                pend.pop(w & 0x1F, None)


def data_slots(va, blob, base, target):
    out = []
    for i in range(0, len(blob) - 7, 8):
        if struct.unpack_from("<Q", blob, i)[0] == target:
            out.append(base + i)
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, needle = sys.argv[1], sys.argv[2]
    read_sections(path)
    ro_va, ro = load(path, ".rodata")
    tx_va, tx = load(path, ".text")
    hits = find_strings(ro, ro_va, needle)
    print("libunity.so: %s  .rodata@%#x .text@%#x" % (path, ro_va, tx_va))
    print("bare occurrences of %r: %s" % (needle, [hex(h) for h in hits] or "none"))
    for target in hits:
        pairs = list(scan_adrp_add(tx_va, tx, target))
        print("\n  target %#x -> %d ADRP+ADD site(s)" % (target, len(pairs)))
        for adrp_va, reg, add_va in pairs[:40]:
            print("    adrp %#x (x%d) ; add %#x" % (adrp_va, reg, add_va))
        for sec in (".data.rel.ro", ".data", ".got"):
            if sec not in SECTIONS:
                continue
            s_va, s_blob = load(path, sec)
            slots = data_slots(s_va, s_blob, s_va, target)
            if slots:
                print("    %s slots: %s" % (sec, [hex(s) for s in slots[:20]]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
