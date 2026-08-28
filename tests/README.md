# ESP host tests

`esp_host_test.cpp` runs the real `jni/src/game.cpp` on the host. The reader only
ever touches the game through `process_vm_readv`, so the test builds a fake
il2cpp + Unity object graph in its own process, points the reader at itself
(`pid == getpid()`) and compares the boxes with screen coordinates computed here
by hand. Layouts mirror the shipped binaries — see the comment at the top of the
test for the exact libunity.so functions each offset was taken from.

```sh
./tests/run_host_tests.sh
```

Scenarios: healthy field, field dead for remote players (the regression that
made every box disappear), no model loaded, rig-only, field full of NaN/huge
values, camera transform reachable only through the object scan, and
"nothing readable" (must not crash, must not draw).

## CI

The Android build job lives in `.github/workflows/build.yml`. To run these tests
on every push/PR as well, add this job (the agent cannot push workflow files —
the GitHub App has no `workflows` permission):

```yaml
  host-tests:
    name: ESP host tests
    runs-on: ubuntu-latest
    steps:
      - name: Checkout
        uses: actions/checkout@v4
        with:
          fetch-depth: 1

      - name: Run ESP reader tests
        run: |
          chmod +x tests/run_host_tests.sh
          ./tests/run_host_tests.sh
```
