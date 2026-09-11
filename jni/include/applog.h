#pragma once

// ---- Лог диагностики в «Загрузках» -----------------------------------------
// Все важные события софта (root, выбор режима, драйвер, подключение к игре)
// пишутся в /sdcard/Download/benzware.log, чтобы лог можно было забрать из
// «Загрузок» без root и adb. Туда же зеркалируется лог драйвера
// /data/local/tmp/ftdrv.log (сам файл в /data без root не читается).
namespace applog {

// Открывает лог-файл: /sdcard/Download/benzware.log, при неудаче
// /storage/emulated/0/Download/benzware.log, затем /data/local/tmp/benzware.log.
// Вызывать как можно раньше при старте (до root-эскалации — оба процесса
// пишут в один и тот же файл в режиме append).
void init();

// Дописать строку с таймстампом (потокобезопасно; no-op если лог не открыт).
void write(const char* fmt, ...);

// Дописать новые данные из /data/local/tmp/ftdrv.log (инкрементально,
// по смещению; вызывать из фонового потока ожидания драйвера).
void mirror_ftdrv();

// Фактический путь лога ("" если лог не открыт).
const char* path();

} // namespace applog
