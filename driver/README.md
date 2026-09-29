# FTDriver payloads

The `*.sh` files are opaque, self-extracting ARM64 launchers supplied with the project. Install the payload set once, keeping the filenames unchanged:

```sh
adb push driver /data/local/tmp/FTDriver
adb shell su -c 'chmod 700 /data/local/tmp/FTDriver/*.sh'
```

After installation the application detects `uname -r`, selects the exact matching payload (then the non-vendor release, then an explicit major.minor payload), and launches it with root permissions when Kernel mode is selected. It never stops a payload on exit. `FTDRIVER_PAYLOAD` can override the selection and `FTDRIVER_DIR` can change the install directory.

## Important integration limitation

The supplied payloads do not contain source, a public header, a documented device-node name, or a documented read/write protocol. Therefore the application must not claim a particular `/dev` path or ioctl layout as the real FTDriver ABI.

The launcher now automates installation-time selection and execution, then waits for an already-running endpoint. The transport adapter still has to be completed or replaced after the actual FTDriver endpoint and read/write protocol are confirmed for the selected payload. It never falls back to `/proc/<pid>/mem`.
