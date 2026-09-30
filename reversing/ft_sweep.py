#!/usr/bin/env python3
# ft_sweep.py — проверка всех FT-сборок из drivers.7z (NOTES_FT_SWEEP.md).
# Пайплайн: .sh -> демон (gzip) -> поиск блоба .ko -> расшифровка кейстримом
# K510 (известная пара 5.10.bin@15157 <-> driver510.ko) -> карта расхождений.
import gzip, os, re, struct, sys, glob

# 1) демоны: из .sh отрезать gzip-поток
def extract_daemon(sh_path):
    data = open(sh_path, 'rb').read()
    i = data.find(b'\x1f\x8b\x08')
    if i < 0: return None
    try: return gzip.decompress(data[i:])
    except OSError: return None

# 2) блоб .ko: позиция, где C[i:i+8]^K[0:8] == magic ELF64 LE
MAGIC = bytes.fromhex('7f454c4602010100')

def find_blob_off(daemon, K):
    pat = bytes(a ^ b for a, b in zip(MAGIC, K[:8]))
    return daemon.find(pat, 0x1000)

# 3) карта расхождений с эталонным открытым текстом
def divergence(dec, P):
    for i in range(min(len(dec), len(P))):
        if dec[i] != P[i]: return i
    return -1

def main(ft_sh_dir='/tmp/ft/sh', ko_dir='/tmp/ft/ko',
         ref_bin='/tmp/ft/ko/5.10.bin', ref_off=15157,
         ref_ko='driver510.ko', out_dir='/tmp/ft/out'):
    os.makedirs(ko_dir, exist_ok=True); os.makedirs(out_dir, exist_ok=True)
    P = open(os.path.join(os.path.dirname(__file__), ref_ko), 'rb').read()
    # извлекаем демоны
    for sh in sorted(glob.glob(os.path.join(ft_sh_dir, '*.sh'))):
        name = os.path.splitext(os.path.basename(sh))[0]
        d = extract_daemon(sh)
        if d: open(os.path.join(ko_dir, name + '.bin'), 'wb').write(d)
    # кейстрим эталона
    C = open(ref_bin, 'rb').read()
    K = bytes(a ^ b for a, b in zip(C[ref_off:ref_off+len(P)], P))
    # свип
    print(f"{'сборка':24} {'off':>6} {'расх':>7}  комментарий")
    for b in sorted(glob.glob(os.path.join(ko_dir, '*.bin'))):
        name = os.path.splitext(os.path.basename(b))[0]
        d = open(b, 'rb').read()
        off = find_blob_off(d, K)
        if off < 0:
            print(f"{name:24} {'—':>6} {'—':>7}  другой кейстрим (группа 3)")
            continue
        dec = bytes(a ^ b for a, b in zip(d[off:off+len(P)], K))
        dv = divergence(dec, P)
        note = 'ЭТАЛОН' if name == '5.10' else (
               f'глубоко: валидный ELF-заголовок' if dv >= 0xa8 else
               ('полосная группа: e_shoff проверять отдельно' if dv <= 0x20 else 'ok'))
        shoff, = struct.unpack_from('<Q', dec, 0x28)
        note += f' e_shoff={shoff:#x}' + (' (совпал!)' if shoff == 0x97f0 else ' (мусор)')
        print(f"{name:24} {off:6} {dv:#7x}  {note}")
        open(os.path.join(out_dir, name + '.ko'), 'wb').write(dec)

if __name__ == '__main__':
    main(*sys.argv[1:])
