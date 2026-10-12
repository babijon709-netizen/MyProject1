#!/usr/bin/env python3
"""Minimal AArch64 disassembler for reverse-engineering a stripped libunity.so.

Why a hand-rolled decoder: the sandbox objdump is x86-only ("can't use supplied
machine aarch64") and capstone cannot be installed offline. We do not need a
full decoder -- we need to read the few instruction forms that reveal a native
class layout:

    ADRP / ADD imm          -> materialise a .rodata pointer (class name,
                               format string); the target is resolved and, when
                               it lands in .rodata, the string is printed;
    LDR / STR (imm, pre, post, register)
    LDP / STP               -> field accesses off `this` or a loaded object;
    MOVZ / MOVK / MOV reg   -> constants (offsets, sizes, masks);
    BL / B / CBZ / CBNZ / TBZ / TBNZ / RET -> control flow, so a function body
                               can be followed and its extent found.

Everything else is printed as `.word 0x........` rather than guessed: this tool
never invents a mnemonic.

Usage:
  python3 tools/unity_rev/adis.py libunity.so --range 0x76a6f0 0x76a760
  python3 tools/unity_rev/adis.py libunity.so --func 0x76a6f0        # back up to
                                                                     # the prologue
  python3 tools/unity_rev/adis.py libunity.so --string MeshRenderer   # find the
                                          # functions that reference a class name
"""
import argparse
import re
import struct
import subprocess
import sys

SEC = {}


def read_sections(path):
    out = subprocess.run(["readelf", "-SW", path], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m:
            SEC[m.group(1)] = dict(addr=int(m.group(3), 16), off=int(m.group(4), 16),
                                   size=int(m.group(5), 16))


def section_of(va):
    for name, s in SEC.items():
        if s["addr"] <= va < s["addr"] + s["size"] and s["size"]:
            return name
    return None


REGN = {31: "sp", 30: "lr"}


def rn(i, sf64=True):
    if i == 31:
        return "sp" if sf64 else "wsp"
    return ("x" if sf64 else "w") + str(i)


def rt(i, sf64=True):
    return ("x" if sf64 else "w") + str(i) if i != 31 else ("xzr" if sf64 else "wzr")


class Image:
    def __init__(self, path):
        self.path = path
        read_sections(path)
        self.fh = open(path, "rb")
        self.cache = {}

    def va_to_off(self, va):
        for name, s in SEC.items():
            if s["size"] and s["addr"] <= va < s["addr"] + s["size"] and name != ".bss":
                return s["off"] + (va - s["addr"])
        return None

    def word(self, va):
        off = self.va_to_off(va)
        if off is None:
            return None
        self.fh.seek(off)
        b = self.fh.read(4)
        return struct.unpack("<I", b)[0] if len(b) == 4 else None

    def cstr(self, va, maxlen=96):
        off = self.va_to_off(va)
        if off is None:
            return None
        self.fh.seek(off)
        b = self.fh.read(maxlen)
        end = b.find(b"\x00")
        if end < 0:
            return None
        try:
            s = b[:end].decode("utf-8")
        except UnicodeDecodeError:
            return None
        return s if s and all(0x20 <= ord(c) < 0x7F for c in s) else None


def decode(w, va, img):
    """Return (text, notes). notes is a dict with resolved targets when any."""
    notes = {}
    # ---- ADRP -------------------------------------------------------------
    if (w & 0x9F000000) == 0x90000000:
        rd = w & 0x1F
        immlo = (w >> 29) & 0x3
        immhi = (w >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & (1 << 20):
            imm -= 1 << 21
        page = (va & ~0xFFF) + (imm << 12)
        notes["adrp"] = (rd, page)
        return "adrp    %s, %#x" % (rt(rd), page), notes
    # ---- ADD / SUB immediate ---------------------------------------------
    if (w & 0x1F000000) == 0x11000000:
        sf = (w >> 31) & 1
        op = (w >> 30) & 1
        sh = (w >> 22) & 1
        imm12 = (w >> 10) & 0xFFF
        if sh:
            imm12 <<= 12
        r_n = (w >> 5) & 0x1F
        rd = w & 0x1F
        mnemonic = "add" if op == 0 else "sub"
        if not sf:
            return "%s    %s, %s, %#x" % (mnemonic, rt(rd, False), rn(r_n, False), imm12), notes
        if op == 0 and imm12 and r_n == 31:
            notes["addsp"] = (rd, imm12)
        if op == 0:
            notes["add"] = (rd, r_n, imm12)
        return "%s    %s, %s, %#x" % (mnemonic, rt(rd), rn(r_n), imm12), notes
    # ---- MOVZ / MOVN / MOVK ----------------------------------------------
    if (w & 0x1F800000) == 0x12800000 or (w & 0x7F800000) in (0x52800000, 0x72800000):
        sf = (w >> 31) & 1
        opc = (w >> 29) & 0x3
        hw = (w >> 21) & 0x3
        imm16 = (w >> 5) & 0xFFFF
        rd = w & 0x1F
        val = imm16 << (16 * hw)
        m = {0: "movn", 2: "movz", 3: "movk"}[opc]
        if m == "movn":
            val = ~val & ((1 << 64) - 1) if sf else ~val & 0xFFFFFFFF
        if m == "movz":
            notes["movz"] = (rd, val)
        txt = "%s    %s, %#x" % (m, rt(rd, sf == 1), val)
        if hw:
            txt += ", lsl #%d" % (16 * hw)
        return txt, notes
    # ---- ORR (register) with Rn=31 -> MOV register -----------------------
    if (w & 0x7FE0FFE0) == 0x2A0003E0:
        sf = (w >> 31) & 1
        rm = (w >> 16) & 0x1F
        rd = w & 0x1F
        return "mov     %s, %s" % (rt(rd, sf == 1), rt(rm, sf == 1)), notes
    # ---- LDR / STR unsigned immediate ------------------------------------
    if (w & 0x3B000000) == 0x39000000:
        size = (w >> 30) & 0x3
        v = (w >> 26) & 1
        opc = (w >> 22) & 0x3
        imm12 = (w >> 10) & 0xFFF
        r_n = (w >> 5) & 0x1F
        rtg = w & 0x1F
        if v:
            return ".word   %#010x" % w, notes
        is_ldr = opc in (1, 2, 3) and not (size == 0 and opc == 2)
        scale = size
        off = imm12 << scale
        mnemonic = "ldr" if (opc & 1) else "str"
        if size == 3 and opc == 2:
            mnemonic = "ldrsw"
        if size == 0 and opc == 2:
            mnemonic = "ldrsb"
        if size == 1 and opc == 2:
            mnemonic = "ldrsh"
        if size == 2 and opc == 2:
            mnemonic = "ldr" if False else "ldrsw" if False else "ldr"
        if off:
            notes["mem"] = (mnemonic, r_n, off, 1 << scale)
            return "%-7s %s, [%s, #%#x]" % (mnemonic, rt(rtg, size == 3), rn(r_n, True), off), notes
        notes["mem"] = (mnemonic, r_n, 0, 1 << scale)
        return "%-7s %s, [%s]" % (mnemonic, rt(rtg, size == 3), rn(r_n, True)), notes
    # ---- LDR / STR pre/post index ----------------------------------------
    if (w & 0x3B200C00) == 0x38000400 or (w & 0x3B200C00) == 0x38000800:
        size = (w >> 30) & 0x3
        opc = (w >> 22) & 0x3
        imm9 = (w >> 12) & 0x1FF
        if imm9 & 0x100:
            imm9 -= 0x200
        pre = (w >> 11) & 1
        r_n = (w >> 5) & 0x1F
        rtg = w & 0x1F
        mnemonic = "ldr" if (opc & 1) else "str"
        suffix = "]!" if pre else "], #%#x" % imm9
        txt = "%-7s %s, [%s" % (mnemonic, rt(rtg, size == 3), rn(r_n, True))
        txt += (", #%#x]!" % imm9) if pre else "], #%#x" % imm9
        return txt, notes
    # ---- LDP / STP signed offset -----------------------------------------
    if (w & 0x7FC00000) in (0x29400000, 0x29000000, 0x2D400000, 0x2D000000):
        opc = (w >> 30) & 0x3
        load = (w >> 22) & 1
        imm7 = (w >> 15) & 0x7F
        if imm7 & 0x40:
            imm7 -= 0x80
        rt2 = (w >> 10) & 0x1F
        r_n = (w >> 5) & 0x1F
        rtg = w & 0x1F
        scale = 2 + opc
        off = imm7 << scale
        mnemonic = "ldp" if load else "stp"
        notes["pair"] = (mnemonic, r_n, off, 1 << scale)
        txt = "%-7s %s, %s, [%s" % (mnemonic, rt(rtg), rt(rt2), rn(r_n, True))
        return (txt + ", #%#x]" % off) if off else txt + "]", notes
    # ---- LDP / STP pre/post ----------------------------------------------
    if (w & 0x7E400000) in (0x28400000, 0x28000000, 0x2C400000, 0x2C000000):
        opc = (w >> 30) & 0x3
        load = (w >> 22) & 1
        pre = (w >> 24) & 1
        imm7 = (w >> 15) & 0x7F
        if imm7 & 0x40:
            imm7 -= 0x80
        rt2 = (w >> 10) & 0x1F
        r_n = (w >> 5) & 0x1F
        rtg = w & 0x1F
        scale = 2 + opc
        off = imm7 << scale
        mnemonic = "ldp" if load else "stp"
        if pre:
            return "%-7s %s, %s, [%s, #%#x]!" % (mnemonic, rt(rtg), rt(rt2), rn(r_n, True), off), notes
        return "%-7s %s, %s, [%s], #%#x" % (mnemonic, rt(rtg), rt(rt2), rn(r_n, True), off), notes
    # ---- BL / B -----------------------------------------------------------
    if (w & 0x7C000000) == 0x14000000:
        link = (w >> 31) & 1
        imm26 = w & 0x3FFFFFF
        if imm26 & (1 << 25):
            imm26 -= 1 << 26
        tgt = va + imm26 * 4
        notes["branch"] = tgt
        return "%-7s %#x" % ("bl" if link else "b", tgt), notes
    # ---- CBZ / CBNZ -------------------------------------------------------
    if (w & 0x7E000000) == 0x34000000:
        sf = (w >> 31) & 1
        nz = (w >> 24) & 1
        imm19 = (w >> 5) & 0x7FFFF
        if imm19 & (1 << 18):
            imm19 -= 1 << 19
        rtg = w & 0x1F
        tgt = va + imm19 * 4
        notes["branch"] = tgt
        return "%-7s %s, %#x" % ("cbnz" if nz else "cbz", rt(rtg, sf == 1), tgt), notes
    # ---- TBZ / TBNZ -------------------------------------------------------
    if (w & 0x7E000000) == 0x36000000:
        nz = (w >> 24) & 1
        b5 = (w >> 31) & 1
        b40 = (w >> 19) & 0x1F
        bit = (b5 << 5) | b40
        imm14 = (w >> 5) & 0x3FFF
        if imm14 & (1 << 13):
            imm14 -= 1 << 14
        rtg = w & 0x1F
        tgt = va + imm14 * 4
        notes["branch"] = tgt
        return "%-7s %s, #%d, %#x" % ("tbnz" if nz else "tbz", rt(rtg), bit, tgt), notes
    # ---- B.cond -----------------------------------------------------------
    if (w & 0xFF000010) == 0x54000000:
        imm19 = (w >> 5) & 0x7FFFF
        if imm19 & (1 << 18):
            imm19 -= 1 << 19
        cond = w & 0xF
        names = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                 "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]
        tgt = va + imm19 * 4
        notes["branch"] = tgt
        return "b.%-5s %#x" % (names[cond], tgt), notes
    # ---- RET --------------------------------------------------------------
    if (w & 0xFFFFFC1F) == 0xD65F0000:
        return "ret", notes
    if w == 0xD503201F:
        return "nop", notes
    # ---- CMP immediate (SUBS with Rd=31) ----------------------------------
    if (w & 0x7F800000) == 0x71000000:
        sf = (w >> 31) & 1
        sh = (w >> 22) & 1
        imm12 = (w >> 10) & 0xFFF
        if sh:
            imm12 <<= 12
        r_n = (w >> 5) & 0x1F
        rd = w & 0x1F
        if rd == 31:
            return "cmp     %s, %#x" % (rn(r_n, sf == 1), imm12), notes
        return "subs    %s, %s, %#x" % (rt(rd, sf == 1), rn(r_n, sf == 1), imm12), notes
    return ".word   %#010x" % w, notes


def prologue_before(img, va, limit=0x4000):
    """Walk backwards to the nearest STP x29,x30,[sp,#-N]! frame setup."""
    best = None
    a = va
    while a > va - limit:
        a -= 4
        w = img.word(a)
        if w is None:
            break
        # STP x29, x30, [sp, #imm]!  -> 0xA9B..7FD pattern
        if (w & 0xFFE00000) == 0xA9800000 or (w & 0xFFC003E0) == 0xA98003E0:
            rt2 = (w >> 10) & 0x1F
            r_n = (w >> 5) & 0x1F
            rtg = w & 0x1F
            if r_n == 31 and rtg == 29 and rt2 == 30:
                best = a
                break
        if (w & 0xFFFFFC1F) == 0xD65F0000:   # a RET ends the previous function
            best = a + 4
            break
    return best if best else va - 0x40


def dump(img, start, end, annotate=True):
    va = start
    regs = {}
    while va < end:
        w = img.word(va)
        if w is None:
            print("%#010x: <outside file>" % va)
            break
        txt, notes = decode(w, va, img)
        extra = ""
        if annotate:
            if "adrp" in notes:
                rd, page = notes["adrp"]
                regs[rd] = page
            elif "add" in notes:
                rd, r_n, imm = notes["add"]
                if r_n in regs:
                    tgt = regs[r_n] + imm
                    regs[rd] = tgt
                    s = img.cstr(tgt)
                    sec = section_of(tgt)
                    extra = "   ; -> %#x%s%s" % (tgt, " (%s)" % sec if sec else "",
                                                 ' "%s"' % s if s else "")
                else:
                    regs.pop(rd, None)
            elif "movz" in notes:
                rd, val = notes["movz"]
                regs[rd] = val
            elif "mem" in notes or "pair" in notes:
                key = "mem" if "mem" in notes else "pair"
                items = notes[key]
                base = items[1]
                off = items[2]
                if base in regs and isinstance(regs[base], int):
                    tgt = regs[base] + off
                    s = img.cstr(tgt)
                    if s:
                        extra = '   ; [%s+%#x] -> "%s"' % (rn(base), off, s)
                if key == "mem" and items[0] == "ldr":
                    regs.pop(rd_of_load(w), None)
            elif "branch" in notes:
                pass
            if (w & 0xFFFFFC1F) == 0xD65F0000:
                regs.clear()
        print("%#010x:  %08x  %s%s" % (va, w, txt, extra))
        if (w & 0xFFFFFC1F) == 0xD65F0000:
            break
        va += 4


def rd_of_load(w):
    return w & 0x1F


def find_string_refs(img, needle):
    s = SEC[".rodata"]
    img.fh.seek(s["off"])
    blob = img.fh.read(s["size"])
    out = []
    start = 0
    while True:
        i = blob.find(needle.encode(), start)
        if i < 0:
            break
        if (i == 0 or blob[i - 1] == 0) and blob[i + len(needle)] == 0:
            out.append(s["addr"] + i)
        start = i + 1
    return out


def scan_adrp_sites(img, target):
    """Every ADRP+ADD pair in .text materialising `target`."""
    t = SEC[".text"]
    img.fh.seek(t["off"])
    blob = img.fh.read(t["size"])
    n = len(blob) // 4
    words = struct.unpack("<%dI" % n, blob[: n * 4])
    tgt_page = target & ~0xFFF
    tgt_off = target & 0xFFF
    pend = {}
    sites = []
    for i, w in enumerate(words):
        va = t["addr"] + i * 4
        if (w & 0x9F000000) == 0x90000000:
            rd = w & 0x1F
            immlo = (w >> 29) & 0x3
            immhi = (w >> 5) & 0x7FFFF
            imm = (immhi << 2) | immlo
            if imm & (1 << 20):
                imm -= 1 << 21
            pend[rd] = (va & ~0xFFF) + (imm << 12)
        elif (w & 0xFF800000) == 0x91000000:
            rd = w & 0x1F
            r_n = (w >> 5) & 0x1F
            imm12 = (w >> 10) & 0xFFF
            if ((w >> 22) & 1):
                imm12 <<= 12
            if r_n in pend and pend[r_n] == tgt_page and imm12 == tgt_off:
                sites.append(va)
            pend.pop(rd, None)
        else:
            if (w & 0x7F000000) in (0x0B000000, 0x2A000000, 0x4B000000, 0x52800000,
                                    0x72800000, 0x8B000000, 0x92000000, 0xAA000000,
                                    0xD2000000, 0xF2000000):
                pend.pop(w & 0x1F, None)
            elif (w & 0x3B000000) == 0x39000000 and ((w >> 22) & 1):
                pend.pop(w & 0x1F, None)
    return sites


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("so")
    ap.add_argument("--range", nargs=2, metavar=("START", "END"))
    ap.add_argument("--func", metavar="ADDR", help="back up to the prologue, dump until RET")
    ap.add_argument("--count", type=int, default=96, help="instructions for --func")
    ap.add_argument("--string", metavar="NAME", help="dump functions referencing this string")
    ap.add_argument("--max-sites", type=int, default=12)
    a = ap.parse_args()
    img = Image(a.so)
    if a.string:
        refs = find_string_refs(img, a.string)
        print("bare %r in .rodata: %s" % (a.string, [hex(r) for r in refs] or "none"))
        for target in refs:
            sites = scan_adrp_sites(img, target)
            print("\ntarget %#x: %d ADRP+ADD site(s)" % (target, len(sites)))
            for va in sites[: a.max_sites]:
                fn = prologue_before(img, va)
                print("\n==== site %#x, function starts %#x ====" % (va, fn))
                dump(img, fn, va + a.count * 4)
        return 0
    if a.range:
        dump(img, int(a.range[0], 0), int(a.range[1], 0))
        return 0
    if a.func:
        va = int(a.func, 0)
        fn = prologue_before(img, va)
        print("==== %#x (prologue %#x) ====" % (va, fn))
        dump(img, fn, va + a.count * 4)
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
