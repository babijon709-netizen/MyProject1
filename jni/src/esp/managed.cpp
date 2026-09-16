// managed.cpp — Чтение managed-строк и коллекций.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке managed.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/farm_scan.h"
#include "esp/game_patch.h"
#include "esp/il2cpp.h"
#include "esp/marker_labels.h"
#include "esp/markers.h"
#include "esp/melee.h"
#include "esp/mem.h"
#include "esp/names.h"
#include "esp/player_pose.h"
#include "managed.h"

// Чтение C-строки (имени класса, пространства имён) из памяти игры.
//
// Кусками по 8 байт, а не «95 байт на всякий случай», как было раньше. Прежнее
// чтение ломалось на границе отображения: ядро отдаёт отображённые байты, а на
// следующем куске возвращает EIO — чтение считалось неудачным целиком, строка
// терялась, хотя начало прочиталось, и это ещё засчитывалось как потеря доступа.
// Теперь идём вперёд кусками и останавливаемся на нуле; если кусок упёрся в
// границу — добираем по байту и сохраняем прочитанное.
//
// readable (если передан) — прочитался ли первый кусок. По этому признаку видно,
// что строка недоступна вообще (в логе устройства все чтения имён классов падали
// с errno 5: память метаданных там не читается), — и тогда класс опознаётся по
// структуре, см. class_identity.
std::string read_remote_string(uint64_t address, bool* readable) {
    if (readable) *readable = false;
    if (!address) return {};
    const size_t kMax = 95;
    char buffer[kMax + 1] = {};
    size_t got = 0;
    while (got < kMax) {
        const size_t want = (kMax - got < sizeof(uint64_t)) ? (kMax - got) : sizeof(uint64_t);
        if (!rd_buf(address + got, buffer + got, want)) {
            if (got == 0) return {};   // первый кусок не читается: строки здесь нет
            size_t single = 0;
            while (single < want &&
                   g_mem.read_quiet(address + got + single, buffer + got + single, 1)) {
                if (buffer[got + single] == '\0') break;
                ++single;
            }
            got += single;
            break;
        }
        got += want;
        if (memchr(buffer + got - want, '\0', want)) break;
    }
    buffer[kMax] = '\0';
    if (readable) *readable = true;
    return std::string(buffer);
}

static bool remote_string_equals(uint64_t address, const char* expected) {
    if (!address || !expected) return false;
    return read_remote_string(address) == expected;
}

bool read_managed_string_ex(uint64_t str_obj, char* out, size_t cap, int32_t max_chars) {
    if (!str_obj || !out || cap < 2) return false;
    if ((str_obj & 0x1) != 0) return false;
    uint64_t klass = rd_ptr(str_obj);
    if (klass < 0x10000 || klass >= 0x0001000000000000ULL) return false;
    int32_t length = 0;
    if (!rd_exact(str_obj + IL2CPP_STRING_LENGTH, length)) return false;
    if (max_chars > 63) max_chars = 63;
    if (length <= 0 || length > max_chars) return false;
    uint16_t chars[64] = {};
    if (!rd_buf(str_obj + IL2CPP_STRING_CHARS, chars, (size_t)length * sizeof(uint16_t)))
        return false;
    size_t pos = 0;
    for (int32_t i = 0; i < length && pos + 1 < cap; ++i) {
        uint32_t cp = chars[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length) {
            uint32_t lo = chars[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                cp = '?';
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = '?';
        }
        char encoded[4];
        int encoded_len = 0;
        if (cp < 0x80) {
            if (cp < 0x20 || cp == 0x7F) cp = '?';
            encoded[0] = (char)cp; encoded_len = 1;
        } else if (cp < 0x800) {
            encoded[0] = (char)(0xC0 | (cp >> 6));
            encoded[1] = (char)(0x80 | (cp & 0x3F));
            encoded_len = 2;
        } else if (cp < 0x10000) {
            encoded[0] = (char)(0xE0 | (cp >> 12));
            encoded[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            encoded[2] = (char)(0x80 | (cp & 0x3F));
            encoded_len = 3;
        } else if (cp <= 0x10FFFF) {
            encoded[0] = (char)(0xF0 | (cp >> 18));
            encoded[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            encoded[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            encoded[3] = (char)(0x80 | (cp & 0x3F));
            encoded_len = 4;
        } else {
            encoded[0] = '?'; encoded_len = 1;
        }
        if (pos + (size_t)encoded_len >= cap) break;
        for (int k = 0; k < encoded_len; ++k) out[pos++] = encoded[k];
    }
    out[pos] = '\0';
    return pos > 0;
}

bool read_managed_string(uint64_t str_obj, char* out, size_t cap) {
    return read_managed_string_ex(str_obj, out, cap, 31);
}
