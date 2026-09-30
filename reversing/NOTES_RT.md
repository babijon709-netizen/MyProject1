# RT-hook драйвер (класс hookdd, архив RT-hook.7z) — протокол

Источник: RT-hook.7z → RT-hook/*.sh (драйверы .sh под 4.14–6.12, канал
@RTdrivers). Клиентского кода в архиве НЕТ — протокол снят статическим
реверсом модуля. Разобран 5.10.sh (954989 байт → .ko 952064):
module name=hookdd, license=GPL, author=waangchuan, description=
«Custom syscall module», исходники /root/yijian/gynhdd/ (entry.c hook.c
memory.h hw_breakpoint.h comm.h cvector.c, hookdd.mod.c), vermagic
5.10.226LZ离@gnmtl-00002-g7d7b7d0205d2-dirty SMP preempt mod_unload
modversions aarch64. Символы не стрипнуты, clang/LTO — релокации .text
лежат в .rela.text.__cfi_check (как у QX).

## Транспорт: ХУК SYSCALL IOCTL (не сокет и не /dev!)

init_module → install_hook(29): 29 = __NR_ioctl на arm64.
- util_find_kallsyms/util_kallsyms_lookup_name находят kallsyms (через
  kprobe kp_kallsyms на kallsyms_lookup_name, для 5.10+), init_page_util
  выучивает pgd/init_mm/уровни страницы/memstart_addr;
- «sys_call_table» → root_table; original_nr=29, orginal_func=table[29];
- sys_hook(29, hook_ioctl) — САМ делает страницу sys_call_table writable:
  inline PTE-walk (pgd→pud→pmd→pte, поддержка больших страниц), ставит
  бит writable/DBM (orr #0x8000000000000), dsb ishst + tlbi vmalle1is +
  dsb ish + isb, пишет table[29] = hook_ioctl (через CFI-трамплин
  hook_ioctl.cfi_jt), возвращает pte в исходное состояние;
- самоскрытие: list_del(&this_module.list) + kobject_del(&module.mkobj)
  + вычистка из vmap_area_root (rb_erase) и vmap_area_list — модуля нет
  в /proc/modules и /sys/module.
- khack_hw_bp_module_init: резолвит register_user_hw_breakpoint /
  unregister_hw_breakpoint (для команд 0x260/0x261).

**Обращение — обычный syscall ioctl(fd, cmd, &req) с ЛЮБЫМ fd** (fd
диспетчером НЕ читается вовсе). hook_ioctl имеет сигнатуру syscall-
обработчика (const struct pt_regs*): cmd = regs->x1, arg = regs->x2.
Чужие cmd (вне 0x259..0x261) уходят в orginal_func(т regs) — для
остальных процессов системы всё выглядит как обычный ioctl.
=> **след в fd-листе чита: НИКАКОЙ** (ни сокета, ни /dev-узла) — самый
скрытный класс из известных; хук виден только из ядра.

## Диспетчер hook_ioctl (.text+0xc5c), jumptable .rodata+0x714

| код | действие | root-гейт |
|---|---|---|
| 0x259 | **чтение**: copy_from_user 0x20 → read_process_memory(pid, addr, buffer, size) | ДА |
| 0x25a | **запись**: copy_from_user 0x20 → write_process_memory(...) | нет (!) |
| 0x25b | база модуля: {u32 pid; char* name @+0x08; u16 ? @+0x18} → strncpy_from_user(0xff) → get_module_base(pid, name, ?, &out) → u64 base в struct+0x10, копия 0x20 назад | ДА |
| 0x25c | проход в оригинальный ioctl (NOP) | — |
| 0x25d | показать процесс: восстановить list-связи, сохранённые 0x25f | нет |
| 0x25e | снять хук: sys_hook(original_nr, orginal_func) + khack_hw_bp_module_exit | нет |
| 0x25f | скрыть процесс: list_del_rcu списка task+0x630 (links пишутся в task+0x638/0x640) | нет |
| 0x260 | hw-breakpoint: счётчик попаданий (kmalloc 0x10-структура, handle_hw_breakpoint_get_hits) | нет |
| 0x261 | hw-breakpoint: управление (handle_hw_breakpoint_control, perf_event HW_BREAKPOINT_X/LEN_4) | нет |
| прочее | orginal_func — прозрачный проход | — |

Root-гейт 0x259/0x25b: cred=current+0x780; uid(cred+4)==0 || euid(cred+0x14)==0
|| capable(CAP_SYS_ADMIN); отказ → printk(«noroot») и **возврат -1**.

Структура COPY (0x20, natural align arm64; ldp x1,x2,[sp,#0x30] в
диспетчере): `u32 pid +0x00; pad +0x04; u64 addr +0x08; u64 buffer +0x10;
u64 size +0x18` — РАСКЛАДКА СОВПАДАЕТ с QX wanbai. Возврат hook_ioctl:
успех → 0; отказ → -1 (-EPERM, errno 1).

## read/write_process_memory (.text+0x2618 / +0x2ae4)

find_get_pid(pid) → get_pid_task → get_task_mm (с правильными put_pid/
mmput/put_task_struct) → **цикл по страницам**: chunk = min(0x1000 −
(addr & 0xfff), size); свой PTE-walk по 4 уровням (сдвиги 0x1e/0x15/0xc,
поддержка pmd-огромных страниц — маска 0x400...000); pfn_valid; vmap()
физической страницы (pgprot с учётом arm64_use_ng_mappings), dmb ish +
dc civac по строкам кэша, copy_to_user/copy_from_user, vunmap; продвижение
addr/buffer/size, пока size != 0. Статус: true(1) — успех, 0 — отказ.
**Ключевое отличие от QX: чанкинг по 4К делает САМ драйвер** — клиенту
нарезать не обязательно (клиент всё равно режет — страховка от других
сборок, пов идентична QX).

## Проба наличия (без побочных эффектов)

`ioctl(-1, 0x259, &req)`:
- драйвер НЕ загружен → обычное ядро: fd=-1 → **EBADF** (9);
- драйвер загружен, мы root → hook_ioctl → 0 (проба чтения своей памяти);
- драйвер загружен, не root → printk «noroot» в dmesg + -1/EPERM.
Классификация как у QX: читаем паттерн из своей памяти через 0x259,
пишем scratch через 0x25a, сверяем.

## Следы на устройстве / скрытность

- fd-лист чита в RT-режиме: НЕТ новых записей (используем fd=-1).
- /proc/modules, /sys/module: модуля нет (self-hide после install_hook).
- dmesg: прошивочный .sh делает dmesg -C после insmod;	printk'и драйвера
  («Custom syscall module loaded» и пр.) при загрузке уже стёрты. Наши
  пробы root'а НЕ пишут (printk только на отказе гейта).
- Хук sys_call_table[29] теоретически детектится из ядра (сравнение адреса
  с kallsyms), из юзерспейса — только по поведению ioctl с cmd 0x259-0x261.

## Поколения драйверов (ПРОВЕРЕНО АВТОМАТОМ ПО ВСЕМ 22 .ko)

Чит-клиент использует только 0x259/0x25a со структурой 0x20 — это
совместимо со ВСЕМИ поколениями. Различия — в способе установки хука и
наборе дополнительных команд:

| поколение | .sh | module | установка хука NR 29 | команды | root-гейт |
|---|---|---|---|---|---|
| 4.14 | 4.14.117/141/180/186/b/c | djbejsvwhjad | kallsyms «__sys_call_table», иначе СКАН между start_address..finish_address (ищут массив, где [57]==sys_close), запись в table+0xe8 (29) | 0x259..0x25c (cmp #3) | НЕТ (только access_ok) |
| 4.19 | 4.19.81/113/157/b/c/191/b/c | djbejsvwhjad | hook_func-трамплин: сам патчит table[29] (x1+0xe8) через inline PTE-walk + DBM + tlbi | 0x259..0x25c | не замечен |
| 5.4/5.10/5.15/6.1/6.6 | 8 шт | hookdd | install_hook(29): kallsyms «sys_call_table» + PTE-walk + самоскрытие | 0x259..0x261 (cmp #8) | 0x259/0x25b (uid/euid/capable) |
| 6.12 | 6.12 | gynhdd (имя ДРУГОЕ!) | install_hook(0x1d) — подтверждено в init.text | 0x259.. | capable() в hook_ioctl |

Структура 0x20 {pid,addr,buffer,size} и возврат «успех 0 / отказ -1»
подтверждены дизассемблом hook_ioctl/new_hook_ioctl во всех поколениях
(4.14.117, 4.19.157, 5.10, 6.12 — вручную, остальные — автопроверкой:
символы hook_ioctl/new_hook_ioctl + read/write_process_memory, копия
0x20, база 0x259, вызовы rw из диспетчера). 4.14/4.19 доп. команд
hide/unhide/hw-bp НЕ имеют (диапазон кончается на 0x25c: 0x25b — база
модуля, 0x25c — проход в оригинал).

## Версии драйверов в архиве (vermagic → uname -r устройства)

| .sh | vermagic |
|---|---|
| 4.14.117 | 4.14.117-perf+ |
| 4.14.141 | 4.14.141-g4e3bb1fab-dirty |
| 4.14.180 | 4.14.180-perf-g758bb7ef5-dirty |
| 4.14.186/b/c | 4.14.186-g49c02c2de / -perf-g30fa96c92 / -g2af31af37 |
| 4.19.81 | 4.19.81-perf-g236f9eb09-dirty |
| 4.19.113 | 4.19.113-perf-g2fdca3872-dirty |
| 4.19.157/b/c | 4.19.157-g9aba441cf6cd / 4.19.157 / 4.19.157-perf+ |
| 4.19.191/b/c | 4.19.191-g42825e29c289 / + / -g914ba8403bb3 |
| 5.4/b/c | 5.4.61-qgki-ga9244def8 / 5.4.191-g7aac66291bd5 / -qgki- |
| 5.10 | 5.10.226LZ离@gnmtl-00002-g7d7b7d0205d2-dirty |
| 5.15 | 5.15.167tg@gnmtl离-00023-g952e16e5fc48-dirty |
| 6.1 | 6.1.112-00034-g226e9f92a06c-dirty |
| 6.6 | 6.6.57-4k-gd66e9166bfa1-dirty |
| 6.12 | 6.12.23-android16-5-maybe-dirty-4k |

Прошивка: su -c sh <файл>.sh — скрипт проверяет файлы Telegram-каналов
(обход: положить cache/-6201945136796122720_99.jpg в каталог Telegram),
sed "1,/^#离/d" отрезает шелл-часть, insmod /data/tmpf, dmesg -C. На
несовпадении vermagic insmod откажет — .sh подбирается под uname -r.

## Реализация в чите

jni/src/kernel_io.h, класс RtTransport: команды kRtCmdRead=0x259 /
kRtCmdWrite=0x25a, структура RtCopyMem (0x20), fd=-1 (kRtAnyFd),
чанкинг 4К (страховка), classify() — пробы по своей памяти.
UI: четвёртая кнопка «RT-Драйвер» (mode_select.h, сетка 2х2),
ESP_MEM_RT=3 (game.h/game.cpp), блок в --check-driver (main.cpp).
