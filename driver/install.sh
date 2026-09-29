#!/system/bin/sh
set -u

KO="${1:-}"
if [ "$(id -u)" != "0" ]; then
    echo "run as root" >&2
    exit 1
fi
if [ -z "$KO" ]; then
    release="$(uname -r)"
    KO="./gc-memory-${release}.ko"
fi
if [ ! -f "$KO" ]; then
    echo "module not found: $KO" >&2
    exit 2
fi

if insmod "$KO"; then
    echo "kernel memory transport loaded for $(uname -r)"
    echo "start xvcen normally (auto mode) or pass --kernel to require it"
    exit 0
fi

echo "insmod failed; the module must be built against this device's exact kernel config/ABI" >&2
exit 3
