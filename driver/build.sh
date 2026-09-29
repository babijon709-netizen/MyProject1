#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
KDIR="${KDIR:-${1:-}}"
OUT="${OUT:-$ROOT/out}"
GC_HIDE="${GC_HIDE:-1}"

if [[ -z "$KDIR" || ! -f "$KDIR/Makefile" ]]; then
    echo "usage: KDIR=/path/to/configured/kernel ./driver/build.sh" >&2
    echo "The tree must match the target device's kernel and Module.symvers." >&2
    exit 2
fi

mkdir -p "$OUT"
make -C "$KDIR" M="$ROOT" ARCH=arm64 \
    LLVM="${LLVM:-1}" LLVM_IAS="${LLVM_IAS:-1}" \
    GC_HIDE="$GC_HIDE" modules -j"${JOBS:-$(nproc)}"

release="$(make -s -C "$KDIR" kernelrelease 2>/dev/null || echo unknown)"
cp -f "$ROOT/gc_memory.ko" "$OUT/gc-memory-${release}.ko"
printf 'built: %s\n' "$OUT/gc-memory-${release}.ko"
