#!/usr/bin/env python3
# Вшивает CRC32 самопроверки в собранный ELF (пара к license.cpp / SelfCheck).
#
# В бинаре лежит 16-байтовый слот: 4 байта магии DE AD BE EF + тег
# "XVCRCPATCH42". Скрипт заменяет магию на CRC32 файла, посчитанный так,
# будто все 16 байт слота — нули. Рантайм в license.cpp повторяет подсчёт
# из /proc/self/exe: пропатченный байт -> CRC не сойдётся -> отказ.
#
# Использование: patch_crc.py <elf>      (код 0 = ок, в т.ч. «слот не найден»)

import sys
import binascii

MAGIC = b"\xDE\xAD\xBE\xEFXVCRCPATCH42"  # ровно 16 байт, как kCrcSlot в license.cpp


def main(path: str) -> int:
    with open(path, "rb") as f:
        data = bytearray(f.read())

    idx = data.find(MAGIC)
    if idx < 0:
        print("patch_crc: слот не найден — самопроверка будет пропускаться (dev)")
        return 0
    if data.find(MAGIC, idx + 1) >= 0:
        print("patch_crc: слот встречается более одного раза — неоднозначность", file=sys.stderr)
        return 2

    tag = MAGIC[4:]                    # 12 байт тега — сначала сохранить
    for i in range(16):
        data[idx + i] = 0
    crc = binascii.crc32(bytes(data)) & 0xFFFFFFFF
    data[idx:idx + 4] = crc.to_bytes(4, "little")
    data[idx + 4:idx + 16] = tag       # и вернуть тег (рантайм ищет по нему)

    with open(path, "wb") as f:
        f.write(data)
    print(f"patch_crc: CRC {crc:08x} вшит @0x{idx:x} ({path})")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <elf>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
