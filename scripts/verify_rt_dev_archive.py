#!/usr/bin/env python3
"""Verify that every RT-Dev module in the supplied archive uses the client ABI.

The script is intentionally dependency-free: it reads the ELF64 section and
symbol tables directly from each self-extracting .sh payload.  It proves that
all modules expose the random /dev/[A-GNY]{1,6} char-device path and dispatch ioctl commands
0x801/0x802/0x803 from the ioctl command argument (w1).
"""

from __future__ import annotations

import argparse
import struct
import sys
import zipfile
from pathlib import Path

EM_AARCH64 = 183
ET_REL = 1
SHT_SYMTAB = 2

REQUIRED_SYMBOLS = {
    "init_module",
    "dispatch_open",
    "dispatch_close",
    "dispatch_ioctl",
    "read_process_memory",
    "write_process_memory",
    "devicename",
}
REQUIRED_COMMANDS = {0x801, 0x802, 0x803}
REQUIRED_RANDOM_NAME_POOL = b"BYNBBYNBBYNBBYNBABCDEFG\0"


def c_string(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated ELF string")
    return data[offset:end].decode("ascii", "replace")


def verify_module(name: str, wrapped: bytes) -> None:
    elf_at = wrapped.find(b"\x7fELF")
    if elf_at < 0:
        raise ValueError("ELF payload marker not found")
    data = wrapped[elf_at:]
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("not a little-endian ELF64 payload")

    e_type, e_machine = struct.unpack_from("<HH", data, 0x10)
    if e_type != ET_REL or e_machine != EM_AARCH64:
        raise ValueError("payload is not an AArch64 relocatable module")

    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    if shentsize != 64 or not shnum or shstrndx >= shnum:
        raise ValueError("invalid ELF section-header table")
    if shoff + shentsize * shnum > len(data):
        raise ValueError("truncated ELF section-header table")

    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
                for i in range(shnum)]
    shstr = sections[shstrndx]
    shstr_data = data[shstr[4]:shstr[4] + shstr[5]]
    if len(shstr_data) != shstr[5]:
        raise ValueError("truncated section-name table")

    by_name: dict[str, tuple[int, ...]] = {}
    for section in sections:
        by_name[c_string(shstr_data, section[0])] = section

    text = by_name.get(".text")
    if text is None:
        raise ValueError(".text section missing")
    text_data = data[text[4]:text[4] + text[5]]
    if len(text_data) != text[5]:
        raise ValueError("truncated .text section")

    symbols: set[str] = set()
    for section in sections:
        if section[1] != SHT_SYMTAB or section[9] != 24:
            continue
        strtab_index = section[6]
        if strtab_index >= len(sections):
            raise ValueError("invalid symbol string-table index")
        strtab = sections[strtab_index]
        strtab_data = data[strtab[4]:strtab[4] + strtab[5]]
        if len(strtab_data) != strtab[5]:
            raise ValueError("truncated symbol string table")
        end = section[4] + section[5]
        if end > len(data):
            raise ValueError("truncated symbol table")
        for at in range(section[4], end, 24):
            st_name = struct.unpack_from("<I", data, at)[0]
            if st_name < len(strtab_data):
                symbols.add(c_string(strtab_data, st_name))

    missing_symbols = REQUIRED_SYMBOLS - symbols
    if missing_symbols:
        raise ValueError("missing symbols: " + ", ".join(sorted(missing_symbols)))

    # get_rand_str draws device-name bytes from this literal. The generated
    # /dev name therefore matches [A-GNY]{1,6}; it is not arbitrary.
    if REQUIRED_RANDOM_NAME_POOL not in data:
        raise ValueError("RTkernel random device-name pool is missing")

    # AArch64 SUBS WZR, W1, #imm is the ioctl dispatch range comparison used
    # by RTkernel. Checking the instruction form, not just a byte sequence,
    # prevents a command constant in debug data from producing a false pass.
    commands: set[int] = set()
    for at in range(0, len(text_data) - 3, 4):
        word = struct.unpack_from("<I", text_data, at)[0]
        is_subs_immediate = (word & 0x7F0003FF) == 0x7100003F
        source_is_cmd = ((word >> 5) & 0x1F) == 1
        if is_subs_immediate and source_is_cmd:
            commands.add((word >> 10) & 0xFFF)
    missing_commands = REQUIRED_COMMANDS - commands
    if missing_commands:
        raise ValueError("ioctl dispatch lacks: " + ", ".join(f"0x{x:03x}" for x in sorted(missing_commands)))

    # mov w2, #0x20 is the size passed by dispatch_ioctl to copy_from_user
    # for COPY_MEMOBY. It must be present alongside the read/write handlers.
    words = struct.iter_unpack("<I", text_data[:len(text_data) & ~3])
    if not any(word == 0x52800402 for (word,) in words):
        raise ValueError("dispatch path lacks the 0x20 COPY_MEMOBY request size")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    default_archive = root / "rt dev Драйвера root cheat.zip"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", nargs="?", type=Path, default=default_archive)
    args = parser.parse_args()

    try:
        with zipfile.ZipFile(args.archive) as archive:
            modules = sorted((info for info in archive.infolist()
                              if not info.is_dir() and info.filename.endswith(".sh")),
                             key=lambda info: info.filename)
            if not modules:
                raise ValueError("no .sh modules in archive")
            failures: list[str] = []
            for module in modules:
                try:
                    verify_module(module.filename, archive.read(module))
                except (OSError, ValueError, struct.error) as exc:
                    failures.append(f"{module.filename}: {exc}")
    except (OSError, zipfile.BadZipFile, ValueError) as exc:
        print(f"RT-Dev archive verification failed: {exc}", file=sys.stderr)
        return 1

    if failures:
        print("RT-Dev archive verification failed:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"RT-Dev archive ABI verified: {len(modules)} modules, /dev/[A-GNY]{{1,6}}, ioctl 0x801/0x802/0x803, COPY_MEMOBY char-device family.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
