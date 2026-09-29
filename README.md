# MyProject1

Android arm64 overlay (`xvcen`) с двумя backend доступа к памяти процесса:

1. **Kernel mode** — `/dev/rwmem`, ABI-совместимый с [`ri-char/rwMem`](https://github.com/ri-char/rwMem).
2. **Procfs fallback** — `/proc/<pid>/mem`.

В режиме по умолчанию приложение сначала проверяет драйвер по ELF-заголовку целевого `libil2cpp.so`, затем автоматически откатывается на procfs. Карта процесса также читается через driver ioctl, если `/proc/<pid>/maps` закрыт.

## Сборка userspace

Нужен Android NDK r26d (`26.3.11579264`):

```bash
$ANDROID_NDK_HOME/ndk-build \
  NDK_PROJECT_PATH="$PWD" \
  APP_BUILD_SCRIPT="$PWD/jni/Android.mk" \
  NDK_APPLICATION_MK="$PWD/jni/Application.mk" \
  V=1
```

Результат:

```text
libs/arm64-v8a/xvcen
```

GitHub Actions workflow `.github/workflows/build.yml` выполняет ту же сборку и публикует artifact `xvcen-arm64-v8a`.

## Kernel driver

Исходники находятся в [`kernel/rwmem`](kernel/rwmem). Установка в дерево Android kernel:

```bash
./scripts/install-rwmem-driver.sh /path/to/android-kernel
```

После этого включить `CONFIG_RW_MEM=y`, собрать и прошить ядро. Подробности, module-вариант и Android SELinux/ueventd — в [`kernel/rwmem/README.md`](kernel/rwmem/README.md).

Универсальный `.ko` не поставляется: модуль должен быть собран под точные исходники, config и toolchain ядра конкретного устройства.

## Выбор backend

```bash
RWMEM_BACKEND=auto    ./xvcen   # default: kernel -> proc
RWMEM_BACKEND=kernel  ./xvcen   # только /dev/rwmem
RWMEM_BACKEND=proc    ./xvcen   # только /proc/<pid>/mem
```

Для нестандартного имени узла:

```bash
RWMEM_DEVICE=/dev/custom-rwmem RWMEM_BACKEND=kernel ./xvcen
```

Поддерживается legacy-узел `/dev/rwMem`.

## Проверка транспорта на Linux host

```bash
./tools/memio/run.sh
```

Тест поднимает дочерний процесс, привязывает `memio::Reader`, проверяет remote read/write, инвалидирование кэша после записи и чтение карты процесса.
