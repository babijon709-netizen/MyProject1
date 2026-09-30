# QX-драйвер (класс wanbai, «QX Perfect core v5») — протокол

Источник: kernal-based-Esp.zip → QX Perfect core v5 (драйверы .ko.sh) +
Qinghuan kernel 2.9/General3.0 (эталонный чит, jni/include/Tools/wanbai.hpp).
Драйвер разобран статически (символы не стрипнуты, Goron-обфускации нет):
reversing/qx510.ko — 5.10.ko.sh, module name=entryi, vermagic 5.10.189-android12.

## Транспорт
- **char-устройство /dev/<случайное имя>**: init_module сам генерирует имя
  (5 строчных букв из get_random_u32) и имя класса (7-16 букв); после insmod
  модуль СКРЫВАЕТСЯ (list_del(&this_module.list) + kobject_del → нет в
  /proc/modules и /sys/module). Прошивочный .sh дополнительно патчит все
  строки "wanbai" в бинарнике на случайные 6 букв.
- Поиск узла (как в эталоне): известные имена {wanbai, CheckMe, Ckanri,
  lanran, video188} + эвристика: char-dev, имя только из строчных букв,
  длина ≤7, без '_','-',';', не gpiochip/std*, uid=gid=0, size 0,
  atime==ctime, год создания ≠ 1970.
- Открытие: open("/dev/<имя>", O_RDWR) — права узла root-only.

## ioctl (dispatch_ioctl = .text+0x5ac)
Валидны коды 0x801..0x805 (`sub w8,w1,#0x801; cmp w8,#4; b.hi → ret 0`),
jumptable в .rodata+0x23 = [00,0e,1c,35,38]:

| код | смещение | действие |
|---|---|---|
| 0x801 | +0x5ec | **чтение**: copy_from_user 0x20 → read_process_memory(pid, addr, buffer, size) |
| 0x802 | +0x620 | **запись**: copy_from_user 0x20 → write_process_memory(...) |
| 0x803 | +0x658 | база модуля: {pid, char* name, u64 base} — strncpy имя (0xff), get_module_base по vma (file_path/strrchr/strcmp) |
| 0x804 | +0x6bc | пустышка: возврат 0 |
| 0x805 | +0x6c8 | «печать»: copy_from_user 0x20 (контент игнорируется!), пишет 0x29a в .text+0 (__cfi_check), возврат 2 |

**Ключа НЕТ**: эталонный чит init_key не вызывает вообще; 0x800 вне
валидного диапазона (возврат 0). 0x805 в эталоне тоже не используется.

Структура COPY_MEMORY (0x20, natural align arm64): `u32 pid +0x00; pad +0x04;
u64 addr +0x08; u64 buffer +0x10; u64 size +0x18` (подтверждено ldp/ldr в
диспетчере). Успех → 0; ошибка → **-1 (= -EPERM, errno 1)**, как у bing 5.10.

## ВАЖНО: без нарезки по страницам
read/write_process_memory делают ОДНУ трансляцию (pgd[30:39]→pmd[21:30]→
pte[12:21], mm->pgd = mm+0x48, phys через memstart_addr + 0xffffff8000000000)
и копируют size байт с одной физической страницы БЕЗ проверки границы.
Клиент ОБЯЗАН резать запросы по границам 4К (QxTransport::rw делает это).
Физический доступ: ioremap_cache + __check_object_size + __arch_copy_to/from_user.

## Реализация
- jni/src/kernel_io.h: class QxTransport (0x801/0x802, чанкинг 4К,
  qx_available() = найти узел + self-probe чтение/запись своей памяти).
- esp_set_mem_backend(int): ESP_MEM_EXTERNAL / ESP_MEM_FT / ESP_MEM_QX.
- Экран «Способ чтения»: три кнопки — Прямое чтение / FT-Драйвер / QX-Драйвер.
- .mode: "external" / "kernel" / "qx".

## Следы на устройстве (стелс)
- /dev/<6 случайных букв> — виден только root (ls /dev).
- fd чита показывает открытый узел: `ls -l /proc/<pid>/fd` → `/dev/abcde`.
- Модуль скрыт: /proc/modules, /sys/module чисты.
- /proc/<pid>/mem НЕ используется; но /proc/<pid>/maps читается для баз
  (как и в FT-режиме) — мгновенные open/read/close.
