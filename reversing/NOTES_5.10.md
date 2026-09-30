# Реверс 5.10.sh — протокол драйвера (сессия 2026-09-29)

## Драйвер (извлечён: driver510.ko, 41008 байт, ET_REL aarch64)
- name=bing_rw, author=NULL1337 (t.me/NULLkawaii), clang 22.1.5, Goron-обфускация
- vermagic=5.10.223-android12-9-00008-g4c7b897f395f-dirty SMP preempt mod_unload modversions aarch64
- import_ns=VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver (маскировка), kobject_del (самоскрытие)
- proto_register "BINGTCP", sock_register(family из .data)

## КАК ИЗВЛЕЧЁН
Демон (stage2, OLLVM) расшифровывает .ko в heap при старте → мемдамп арены
эмулятора при kill(getpid(),9) → поиск \x7fELF → driver510.ko.
(daemon_emu510.py: absent/present режимы, кооперативные потоки, va_list printf.)

## ПРОТОКОЛ (главное!)
- socket(fam=12.., type=6, ANY proto) -> errno 1338 (маячок; протокол игнорируется)
- socket(fam, type=4, 0) -> fd (рабочий сокет)
- **getsockopt И setsockopt — заглушки: ЛЮБОЙ код -> ENOPROTOOPT(92)**
  → поэтому у пользователя все коды дали 92 и скан 0..0x7FF пуст!
- **Команды через ioctl(fd, код, &req)**: 0x300, 0x301, 0x302, 0x303
  (сравнения cmd в обработчике .text+0x105c/0x10a4/0x10e8/0x18d4 после init)
- Запрос тот же, что у 6.6: 0x28 байт {u32 pid, u32 pad, u64 addr, u64 buf,
  u64 size, u32 magic=0x1337 @+0x20, u32 pad2}; copy_from_user(0x28),
  проверка magic==0x1337 (обработчик 0x21fc: ldur w8,[x29,-0x88]; cmp 0x1337)
- Второй 0x28-обработчик на 0x2260 (вероятно write/вторая команда)
- Чтение/запись классифицируются пробами pat/mirror (как в kernel_io.h)

## Эмуляция модуля (ko_emu.py + ko_sweep.py)
Unicorn: секции+релокации (257/274/275/276/277/278/282/283/284/285/286),
импорты-функции → стабы svc, импорты-ДАННЫЕ (cpu_hwcaps и пр.) → своя память,
sp_el0 = fake task (addr_limit 48бит). init_module прогоняется, патчит
thunks → актуальные адреса. create(4,0)->0, create(6,любой)->-1338 ✓.
ioctl(0x300) идёт до copy_from_user + state machine (глубже не прошли:
5.10-стейтмашина требует полного ядра).

## Статус приложения
kernel_io.h/f772ebb: автодетект транспорта — getsockopt ENOPROTOOPT ⇒ ioctl.

---

## 2026-09-30: ПРОТОКОЛ ПОЛНОСТЬЮ РАСШИФРОВАН (эмуляция, все проверки пройдены)

Эмулятор (ko_emu.py, фикс: стабы `movz x16`, реальный copy_from/to_user, fake
thread_info с addr_limit=0x0000FFFFFFFFFFFF — ВАЖНО: не 2^48, иначе uaccess-маска
`csel x8,x1,xzr` обнуляет src) дошёл до живого чтения/записи в фейковой памяти.

### Протокол bing_rw 5.10 (полный)

1. **Сокет**: `socket(fam, 4, 0)` (маячок 1338 на probe type 6 — тот же).
2. **АВТОРИЗАЦИЯ**: `ioctl(fd, 0x300, buf49)` — buf49 (0x31 байт) =
   `"C61StIeKasRIz89g3c6xLzIc9L0LaRbI\0"` + нули. Драйвер: при глобальном флаге
   .data+0x290==0 сверяет первые 0x21 байта с KEY[i]^state_i (33 итерации,
   rolling state через helper .text+0x36c, ключ-блоб .text+0x5e8) и ставит
   флаг=1. Возврат ioctl бывает -1 даже при успехе — игнорировать.
   Без флага рабочие команды → -EPERM (errno 1, что и видел пользователь).
3. **ЧТЕНИЕ**: `ioctl(fd, 0x301, req28)` → 0 при успехе:
   `{u32 pid +0x00; u32 pad +0x04; u64 addr +0x08; u64 size +0x10;
     u64 local_buf +0x18; u32 magic=0x1337 +0x20; u32 pad +0x24}`
   magic сверяется ДО работы (handler .text+0x21fc: `ldur w8,[x29,-0x88];
   mov w9,#0x1337; cmp`). Путь: find_get_pid→get_pid_task→get_task_mm→
   down_read(mm+0x70)→свой PTE-walk (pgd[addr>>30&0x1ff] → pmd[addr>>21&0x1ff]
   → pte[addr>>12&0x1ff], mm->pgd = mm+0x48; phys→kva через
   0xffffff8000000000|phys - [0x20000a0]) → pfn_valid → __check_object_size →
   `__arch_copy_to_user(local_buf, kva|offset, min(0x1000-off, size))`,
   цикл по страницам → up_read → mmput.
4. **ЗАПИСЬ**: `ioctl(fd, 0x302, req28)` — та же структура (magic не
   сверяется), функция .text+0x273c: тот же walk, копия
   `__arch_copy_from_user(kva|offset, local_buf, chunk)`.
5. **0x303** — сырая запись 0x31 байт по адресу из req+0x08 (не используется).

### Ключевые смещения (координаты BASE=0x40000000, .text=BASE+0x680!)
- 0x40003980 — НЕ функция, а vtable proto_ops (bti c; b target)!
- dispatcher/ioctl = .text+0xffc (0x4000167c), create = 0x40004390.
- auth-флаг .data+0x290 (adrp 0x40004000+0x290), глобал ключа .data+0x1e8.
- Хендшейк-цикл: сравнение .text+0x1c44 (блок +0x1c28), счётчик [sp+0x68],
  предел 0x21 из глобала.
- READ handler +0x21fc (magic 0x1337!), WRITE handler +0x1e6c → ф-я +0x273c.
- Размеры запросов: auth 0x31, read/write 0x28. Копия запроса — wrapper
  +0x35d8 (в .text координатах +0x2f58).

### Результаты эмуляции (hs15.txt в /tmp/drv — воспроизвести при сбросе)
```
create -> 0x0
auth -> 0xffffffff (flag: 1)
READ  (0x301, {pid=1234, addr, size=16, dst}) -> 0x0, dst='HERO_SECRET_DATA'
WRITE (0x302, {pid, addr, size=16, src})      -> 0x0, память='WRITEDATA_INJECT'
READ#2 -> 0x0 | dst='WRITEDATA_INJECT'        # читаем записанное
```

### Реализовано в jni/src/kernel_io.h
- kDriverCmdAuth=0x300 (send_auth: ioctl паролем, retval игнорируем),
  kDriverCmdRead=0x301, kDriverCmdWrite=0x302.
- DriverRwReq: size@0x10, user_buf@0x18 (ПОМЕНЯЛИСЬ местами со старой гипотезы!).
- send_auth() вызывается: при переходе на ioctl-транспорт (ENOPROTOOPT от
  getsockopt), в open(), в rw(). classify_cmd шлёт auth при use_ioctl_.
