# FTDriver payloads

The `*.sh` files are opaque, self-extracting ARM64 launchers supplied with the project. Each file must be selected and started manually for the exact Android kernel build. The application does **not** choose, start, or stop a payload.

Example deployment:

```sh
adb push driver /data/local/tmp/FTDriver
adb shell su -c 'chmod 700 /data/local/tmp/FTDriver/*.sh'
# Select the exact payload for the device, then run it manually as root.
```

## Important integration limitation

The supplied payloads do not contain source, a public header, a documented device-node name, or a documented read/write protocol. Therefore the application must not claim a particular `/dev` path or ioctl layout as the real FTDriver ABI.

Kernel mode in the client is consequently only an integration shell: it can be pointed at an already-running endpoint with `MEMIO_KERNEL_DEVICE`, but the transport adapter must be replaced or completed after the actual FTDriver endpoint and read/write protocol are confirmed for the selected payload. It never falls back to `/proc/<pid>/mem`.

To complete the adapter, provide the output of the manual launch and the resulting endpoint/protocol, for example the created device or socket path and the request format used for attach, read, and write. A driver compiled for a different kernel must not be selected by filename prefix alone.
