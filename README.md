# xvcen

Android arm64 overlay with two selectable memory transports:

- **External** — the original `/proc/<pid>/mem` reader.
- **Kernel** — the QX/Qinghuan-compatible driver protocol used by the reference source in `kernal-based-Esp.zip`.

## Selecting the mode

When the binary is started from an interactive shell it shows a menu before EGL, touch, or the attach threads are initialized:

```text
[1] External  - /proc/<pid>/mem
[2] Kernel    - installed Android kernel driver
```

For launchers without a TTY, pass an explicit option instead:

```sh
./libs/arm64-v8a/xvcen --kernel
./libs/arm64-v8a/xvcen --proc-mem
# or: MEMORY_BACKEND=kernel ./libs/arm64-v8a/xvcen
```

The selected backend is process-wide and cannot change while a frame is being
read. The game/ESP code uses the same `Reader` API in both modes, so reads,
writes, frame caching, reconnects, and liveness checks stay consistent.

## Kernel driver setup

Install the module matching the device's Android kernel version from
`drivers.7z` (or the matching script in `kernal-based-Esp.zip`) before starting
the executable. The scripts unpack and load the module, which creates the
character node under `/dev`. The application discovers both the stable names
used by older modules (`wanbai`, `CheckMe`, `Ckanri`, `lanran`, `video188`) and
the lowercase dynamically named nodes used by newer modules. It validates a
candidate by reading the target process's ELF header through the driver before
accepting it.

The userspace protocol is kept in `jni/include/kernel_driver.h` and
`jni/src/kernel_driver.cpp`:

- `0x801` — read target-process memory;
- `0x802` — write target-process memory;
- `0x803` — query a module base.

The layout matches `Tools/wanbai.hpp` from the reference source. No driver
binary is rebuilt as part of the Android NDK build; the supplied per-kernel
module scripts remain the runtime dependency.

## Build

The existing GitHub Actions workflow builds `jni/Android.mk` for
`arm64-v8a`. Locally, use the same NDK command as the workflow:

```sh
$ANDROID_NDK_HOME/ndk-build \
  NDK_PROJECT_PATH="$PWD" \
  APP_BUILD_SCRIPT="$PWD/jni/Android.mk" \
  NDK_APPLICATION_MK="$PWD/jni/Application.mk"
```
