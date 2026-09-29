#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 /path/to/android-kernel" >&2
  exit 2
}

[[ $# -eq 1 ]] || usage
kernel_root=$(realpath "$1")
script_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir="$script_root/kernel/rwmem"
dest_dir="$kernel_root/drivers/rwmem"

[[ -f "$kernel_root/Makefile" && -d "$kernel_root/drivers" ]] || {
  echo "not a kernel source tree: $kernel_root" >&2
  exit 1
}

mkdir -p "$dest_dir"
cp "$source_dir/rwmem.c" "$source_dir/rwmem_uapi.h" \
   "$source_dir/Makefile" "$source_dir/Kconfig" "$dest_dir/"

make_line='obj-$(CONFIG_RW_MEM) += rwmem/'
kconfig_line='source "drivers/rwmem/Kconfig"'

grep -Fqx "$make_line" "$kernel_root/drivers/Makefile" || \
  printf '\n%s\n' "$make_line" >> "$kernel_root/drivers/Makefile"
grep -Fqx "$kconfig_line" "$kernel_root/drivers/Kconfig" || \
  printf '\n%s\n' "$kconfig_line" >> "$kernel_root/drivers/Kconfig"

cat <<EOF
rwmem installed into: $dest_dir
Enabled hooks in drivers/Makefile and drivers/Kconfig.
Set CONFIG_RW_MEM=y, rebuild the kernel, then verify /dev/rwmem exists.
EOF
