# FTDriver payloads

The `*.sh` files are the kernel payloads supplied with the project. Each file is a self-extracting shell launcher and is selected by the Android kernel release (`uname -r`). Keep the launcher and its filename unchanged when deploying it:

```sh
adb push driver /data/local/tmp/FTDriver
adb shell su -c 'chmod 700 /data/local/tmp/FTDriver/*.sh'
```

The app looks for the payload directory in `FTDRIVER_DIR` first and then in `/data/local/tmp/FTDriver`, `/data/local/tmp/ftdriver`, and `/sdcard/FTDriver`. An already-running driver can also be selected by setting `MEMIO_KERNEL_DEVICE` to its device node.

## Client ABI

Kernel mode is intentionally separate from External mode. The new client uses the following ioctl requests on the driver device. The supplied payloads are opaque ELF launchers and do not include a kernel-side header or ABI description, so a driver-side shim must expose this contract (or the client transport must be adapted to the real contract):

- `_IOW('F', 0x00, int32_t)` — attach to a target PID
- `_IOWR('F', 0x01, KernelMemoryRequest)` — read target memory
- `_IOWR('F', 0x02, KernelMemoryRequest)` — write target memory
- `_IOW('F', 0x03, int32_t)` — detach from a target PID

`FTDriverMemoryRequest` is the 32-byte structure `{ int32_t pid; uint32_t flags; uint64_t address; uint64_t buffer; uint64_t size; }`, defined in `jni/include/ftdriver_abi.h`. `buffer` is a pointer in the calling app and `address` is an address in the target process. The driver must validate both the PID and the user buffer before copying.

If a payload exposes a different device node or ABI, configure the node with `MEMIO_KERNEL_DEVICE` and adapt the driver-side shim; the app will not silently fall back to `/proc/<pid>/mem`.
