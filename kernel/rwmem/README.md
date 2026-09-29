# rwmem kernel driver

Драйвер реализует ABI проекта [`ri-char/rwMem`](https://github.com/ri-char/rwMem) для чтения/записи памяти и получения карты процесса. В отличие от исходной реализации он использует `access_process_vm()`, поэтому не хранит PTE после снятия блокировки и корректно работает со swapped/COW-страницами. Патч ядра для hardware breakpoints приложению не нужен.

## Встраивание в Android kernel

Из корня этого репозитория:

```bash
./scripts/install-rwmem-driver.sh /path/to/android-kernel
```

Затем включить:

```text
CONFIG_RW_MEM=y
```

и собрать/прошить ядро обычным способом для конкретного устройства. После загрузки:

```bash
ls -l /dev/rwmem
cat /sys/dev/char/$(cat /sys/class/rwmem/rwmem/dev)/uevent 2>/dev/null || true
```

Для Android рекомендуется `=y`: GKI может не экспортировать `access_process_vm` vendor-модулям. Вариант `=m` можно собрать так:

```bash
make -C /path/to/kernel \
  M="$PWD/kernel/rwmem" CONFIG_RW_MEM=m \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```

`.ko` должен собираться ровно против дерева и конфигурации запущенного ядра. Универсального бинарного драйвера для разных устройств/версий ядра нет.

## Android permissions / SELinux

Dev-узел создаётся как `/dev/rwmem`. Дайте доступ домену, из которого запускается `xvcen`, через `ueventd.rc` и sepolicy конкретной прошивки. Минимальная схема:

```text
# ueventd.rc
/dev/rwmem  0600  root  root
```

В `file_contexts`/`device_contexts` назначьте узлу отдельный `dev_type`, затем разрешите этому типу `open/read/write/ioctl/getattr` для нужного домена. Имя домена root-shell/KernelSU зависит от прошивки, поэтому готовое правило нельзя сделать переносимым.

## Userspace selection

`xvcen` по умолчанию выбирает `/dev/rwmem`, проверяет ELF-заголовок и при неудаче откатывается на `/proc/<pid>/mem`.

```bash
RWMEM_BACKEND=auto    ./xvcen   # default: driver, then proc fallback
RWMEM_BACKEND=kernel  ./xvcen   # require the driver
RWMEM_BACKEND=proc    ./xvcen   # force the old procfs path
RWMEM_DEVICE=/dev/custom-rwmem RWMEM_BACKEND=kernel ./xvcen
```

Поддерживается и legacy-имя `/dev/rwMem`.
