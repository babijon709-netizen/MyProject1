# xvcen: user mode + Android kernel mode

В проект добавлен kernel-backend памяти на базе архитектуры
[`android_kerneldriver_gongchuang`](https://github.com/gongchuang1089/android_kerneldriver_gongchuang).
`drivers.7z` не использовался.

## Что реализовано

- Android arm64 клиент собирается из `jni/` как раньше.
- Новый backend `jni/src/kernel_memory.*` общается с модулем через динамически
  зарегистрированное семейство raw-сокетов.
- Нет постоянного `/dev/...` узла, sysfs-класса или ioctl character device.
- Драйвер допускает только UID 0, проверяет ABI, PID, размеры запросов и
  сериализует обращения одного socket-сеанса.
- Чтение/запись основного ESP, viewmodel, freecam и фонового silent-aim writer
  используют один и тот же выбранный режим.
- Адреса ARM TBI/MTE очищаются от тега и в основном, и в silent-aim backend.
- В режиме `auto` сначала используется драйвер, а если он не загружен —
  `/proc/<pid>/mem`.
- Исходник модуля находится в `driver/`. По умолчанию после загрузки он удаляет
  себя из `/proc/modules` и `/sys/module`; можно собрать без этого через
  `GC_HIDE=0`.

## Сборка клиента

Нужен Android NDK r26c или совместимый:

```bash
$ANDROID_NDK_HOME/ndk-build \
  NDK_PROJECT_PATH="$PWD" \
  APP_BUILD_SCRIPT="$PWD/jni/Android.mk" \
  NDK_APPLICATION_MK="$PWD/jni/Application.mk"
```

Результат: `libs/arm64-v8a/xvcen`.

## Режимы запуска

```bash
# Авто: kernel, затем fallback на /proc/<pid>/mem
su -c '/data/local/tmp/xvcen'

# Обязательно драйвер; fallback запрещён
su -c '/data/local/tmp/xvcen --kernel'

# Старый userspace backend
su -c '/data/local/tmp/xvcen --proc'
```

То же можно задать переменной окружения:

```bash
XVCEN_MEMORY=kernel /data/local/tmp/xvcen
# значения: auto, kernel, proc
```

## Сборка драйвера

`.ko` жёстко привязан к ABI, конфигурации, `vermagic` и часто к CRC символов
конкретного ядра устройства. Поэтому универсальный модуль для всех телефонов
с одним номером Android невозможен: его надо собирать против подходящего дерева
ядра и `Module.symvers`.

```bash
KDIR=/path/to/configured/device-kernel \
  ./driver/build.sh
```

Результат появится как:

```text
driver/out/gc-memory-<kernel-release>.ko
```

Если скрытие списков модуля при отладке не нужно:

```bash
GC_HIDE=0 KDIR=/path/to/kernel ./driver/build.sh
```

Для GKI дерево/выходной каталог должны соответствовать целевому kernel release,
KMI и Android security patch level. Для OEM-ядра используйте опубликованный
производителем source/config и его `Module.symvers`.

## Установка

```bash
adb push driver/out/gc-memory-$(adb shell uname -r).ko /data/local/tmp/
adb push driver/install.sh /data/local/tmp/
adb push libs/arm64-v8a/xvcen /data/local/tmp/

adb shell su -c 'chmod 700 /data/local/tmp/install.sh /data/local/tmp/xvcen'
adb shell su -c '/data/local/tmp/install.sh /data/local/tmp/gc-memory-<release>.ko'
adb shell su -c '/data/local/tmp/xvcen --kernel'
```

При `GC_HIDE=1` модуль штатно не выгружается по имени после удаления из списков;
для цикла разработки собирайте с `GC_HIDE=0` или перезагружайте устройство.

## Структура

- `jni/src/kernel_memory.cpp` — userspace socket-клиент и выбор backend.
- `jni/src/mem_io.h` — общий кэшированный Reader с kernel/proc транспортом.
- `jni/src/silent_aim.cpp` — отдельный высокочастотный writer с тем же режимом.
- `driver/gc_memory.c` — root-only kernel transport.
- `driver/build.sh`, `driver/install.sh` — сборка и загрузка модуля.

Driver-код распространяется по GPL-2.0 и содержит атрибуцию исходного проекта.
