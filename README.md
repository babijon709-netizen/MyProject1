# Memory access modes

The native client has two separate memory transports. On every start it first shows a transport-selection screen:

- **External** — the existing `/proc/<pid>/mem` path.
- **Kernel** — a separate FTDriver transport; it never falls back to External.

The attach thread and game-memory pipeline are not started until a choice is made. `jni/src/mem_io.h` keeps the frame cache and delegates only the actual read/write operation to the selected transport.

## Build

```sh
ndk-build APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk
```

The unpacked `jni/` tree is kept alongside the original `benzwaresc.7z`; the archive is refreshed with the new files.

## Kernel mode deployment

Install the supplied payload launchers once as `/data/local/tmp/FTDriver/`. When Kernel mode is selected, the app detects the device kernel release, selects the matching installed `.sh`, starts it through `su`, and waits for its endpoint. `FTDRIVER_PAYLOAD` can provide an explicit payload and `FTDRIVER_DIR` can provide another install directory.

The original payloads are opaque ELF launchers and do not contain a kernel-side header or documented device/read/write ABI. The app does not present its provisional adapter constants as the native FTDriver ABI. The transport must be adapted to the actual endpoint and protocol for the selected payload; Kernel mode deliberately reports that failure instead of silently using `/proc/<pid>/mem`.
