# RT-Dev драйвер (пакет "RT【Dev】Driver", rt dev Драйвера root cheat.zip) — протокол

Источник: "rt dev Драйвера root cheat.zip" (23 файла .sh под ядра 4.9–6.6).
Канал/автор: @RTdrivers / RT-LZ / BYNB.

## Механизм
- Char-устройство `/dev/<случайное_имя>` (6 символов, алфавит "BYNBBYNBBYNBBYNBABCDEFG", например `/dev/BYBNCG`, `/dev/wanbai`, `/dev/RTkernel` и др.).
- Регистрация в ядре: `alloc_chrdev_region`, `cdev_init`, `cdev_add`, `class_create` (mem_tool_class), `device_create`.
- Команды ioctl на открытом файловом дескрипторе `/dev/...`:
  * `0x801` — чтение памяти процесса: `QxCopyMem` (`RtDevCopyMem`) {u32 pid; u32 pad; u64 addr; u64 buffer; u64 size} (0x20 байт).
  * `0x802` — запись памяти процесса: та же структура 0x20 байт.
  * `0x803` — получение базы модуля (нами не используется, базы из /proc/<pid>/maps).
- Важно: драйвер транслирует виртуальный адрес постранично (4К) без перехода через границу страницы -> запрос бьётся по границам 4К.
- Проба по своей памяти: чтение/запись паттерна через ioctl(fd, 0x801/0x802).
