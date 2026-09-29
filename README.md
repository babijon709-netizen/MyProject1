# Memory access modes

The native client now has two separate memory transports. On every start it first shows a transport-selection screen:

- **External** — the existing `/proc/<pid>/mem` path.
- **Kernel** — the FTDriver ioctl transport; it never falls back to External.

The attach thread and game-memory pipeline are not started until a choice is made. `jni/src/mem_io.h` keeps the frame cache and delegates only the actual read/write operation to the selected transport.

## Build

```sh
ndk-build APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk
```

The unpacked `jni/` tree is kept alongside the original `benzwaresc.7z`; the archive has also been refreshed with the new files.

## Kernel mode deployment

The supplied kernel payload launchers are in `driver/` and are selected by the kernel release. Deploy them as `/data/local/tmp/FTDriver/` or set `FTDRIVER_DIR`. If the driver is already running, set `MEMIO_KERNEL_DEVICE` to its device node. The launcher waits for one of the supported FTDriver endpoints before enabling Kernel mode.

The original payloads are opaque ELF launchers and do not contain a kernel-side header or documented ioctl ABI. The app-side contract is documented in `driver/README.md`; if the payload exposes a different device or request layout, add a driver-side shim or adapt `jni/src/memory_backend.cpp`. Kernel mode deliberately reports that failure instead of silently using `/proc/<pid>/mem`.
