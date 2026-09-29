# Memory access modes

`xvcen` now chooses its memory backend **before** initializing graphics, touch,
audio, or the process-attach thread.

## Interactive launch

When stdin is a terminal the executable displays:

1. **Ordinary external mode** — the existing `/proc/<pid>/mem` backend.
2. **Kernel mode** — loads the supplied script matching `uname -r`, then uses
   the arm64 `process_vm_readv`/`process_vm_writev` route serviced by that
   kernel loader.

Kernel mode requires an effective UID of 0. A missing/mismatched/failing loader
is a hard startup error; the program does not silently fall back to ordinary
mode.

## Non-interactive launch

Non-interactive launches default safely to ordinary mode. Explicit options are:

```sh
./xvcen --mode=external
./xvcen --mode=kernel --driver=auto --drivers-dir=/data/local/tmp/drivers
./xvcen --mode=kernel --driver=/data/local/tmp/drivers/5.10.sh
```

`XVCEN_DRIVERS_DIR` can be used instead of `--drivers-dir`. Automatic lookup
also searches `drivers` and `drivers_extracted` beside the executable and in
`/data/local/tmp`.

## Deploying the supplied loaders

Copy the relevant scripts from `drivers_extracted/` to the device along with
the executable. The scripts are self-extracting arm64 loaders and must remain
byte-for-byte intact; do not open and re-save them in a text editor. Example:

```sh
adb push libs/arm64-v8a/xvcen /data/local/tmp/xvcen
adb push drivers_extracted /data/local/tmp/drivers
adb shell su -c 'chmod 700 /data/local/tmp/xvcen; chmod 600 /data/local/tmp/drivers/*.sh; /data/local/tmp/xvcen'
```

The automatic matcher uses the kernel family and prefers an exact patch/vendor
variant when `uname -r` identifies one. `b`/`c` variants cannot be inferred
reliably from `uname`; select those explicitly with `--driver=PATH`.

## Diagnostics

The backend name reported by memory diagnostics is now one of:

- `/proc/<pid>/mem`
- `kernel/process_vm`
- `нет доступа` (not attached/no access)

Kernel mode still verifies the ELF header at the selected `libil2cpp.so` base
when attaching. This catches a loader that exits successfully but does not
provide memory access on that device/kernel.
