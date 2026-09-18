// maps_lookup.h — разбор /proc/<pid>/maps: базовый адрес отображённой
// библиотеки по её имени.
//
// Вынесено из game.cpp отдельной функцией, потому что здесь однажды уже была
// ошибка, которая роняла процесс на старте: длина пути считалась как
// «eol - (slash - line)», то есть абсолютный индекс конца строки складывался с
// относительным смещением '/' ВНУТРИ строки. name_len выходил ровно на начало
// строки больше настоящего, и сравнение имени уходило читать память за концом
// строки, а на последней строке карты (без '\n' в конце) — за концом буфера
// целиком. На устройстве это выглядело так: первый запуск (игра ещё не
// запущена) — меню открылось, а как только игра стартовала, процесс падал с
// SIGSEGV в потоке привязки, ещё до первого кадра. Чистая логика без Android —
// такую ошибку ловит хостовая сборка с ASAN, без устройства.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

namespace maps {

// Адрес начала отображения библиотеки lib в карте text.
//
// Строка карты: "start-end perms offset dev inode путь". Имя сверяется по
// последнему сегменту пути: подстрока в чужом имени («/tmp/foo_libil2cpp.so»)
// или « (deleted)» в конце не должны путать. Приоритет — сегмент с нулевым
// смещением в файле: это и есть адрес загрузки образа; если такого нет
// (странная карта), отдаём минимальный load bias, он же адрес начала.
inline uint64_t lookup_library_base(const std::string& text, const char* lib) {
    if (text.empty() || !lib || !lib[0]) return 0;
    const size_t lib_len = strlen(lib);
    uint64_t fallback = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        const size_t line_end = (eol == std::string::npos) ? text.size() : eol;
        const char* line = text.c_str() + pos;
        const size_t line_len = line_end - pos;   // без '\n'
        pos = line_end + 1;

        unsigned long long start = 0, end = 0, file_offset = 0;
        char perms[5] = {};
        if (sscanf(line, "%llx-%llx %4s %llx", &start, &end, perms, &file_offset) != 4)
            continue;
        const char* slash = strchr(line, '/');
        if (!slash) continue;
        const size_t path_off = (size_t)(slash - line);
        if (path_off >= line_len) continue;
        size_t name_len = line_len - path_off;

        // Хвостовые пробелы и « (deleted)» — не часть имени файла.
        while (name_len > 0 && (slash[name_len - 1] == '\r' || slash[name_len - 1] == ' '))
            --name_len;
        static const char kDeleted[] = " (deleted)";
        const size_t deleted_len = sizeof(kDeleted) - 1;
        if (name_len > deleted_len &&
            strncmp(slash + name_len - deleted_len, kDeleted, deleted_len) == 0)
            name_len -= deleted_len;

        if (name_len < lib_len) continue;
        if (strncmp(slash + name_len - lib_len, lib, lib_len) != 0) continue;
        if (name_len > lib_len && slash[name_len - lib_len - 1] != '/') continue;

        const uint64_t load_bias = (uint64_t)start - (uint64_t)file_offset;
        if (!fallback || load_bias < fallback) fallback = load_bias;
        if (file_offset == 0) return (uint64_t)start;
    }
    return fallback;
}

// Все отображения библиотеки, с которых можно начать чтение.
//
// Зачем список, а не один адрес: образов libil2cpp.so в карте бывает несколько.
// После перезапуска игры (пакеты перезапускают себя через exec, pid при этом
// сохраняется) в карте остаётся ещё и старый образ, часто с пометкой «(deleted)»:
// он всё ещё в памяти, читается, но метаданные il2cpp внутри него мёртвые —
// привязка к нему выглядит успешной, а любое чтение «работает» и молча даёт
// мусор. Поэтому базу выбирает вызывающий: ту, на которой резолвится класс игры.
//
// Порядок: сначала сегменты с нулевым смещением в файле (истинный адрес загрузки
// образа), по возрастанию адреса; затем остальные, тоже по возрастанию. Дубли по
// адресу отбрасываются.
inline int lookup_library_bases(const std::string& text, const char* lib,
                                uint64_t* out, int max) {
    if (text.empty() || !lib || !lib[0] || !out || max <= 0) return 0;
    const size_t lib_len = strlen(lib);
    uint64_t primary[8] = {};
    uint64_t secondary[8] = {};
    int primary_count = 0, secondary_count = 0;

    auto add = [](uint64_t* list, int& count, int cap, uint64_t value) {
        if (!value) return;
        for (int i = 0; i < count; ++i) if (list[i] == value) return;
        if (count < cap) list[count++] = value;
    };

    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        const size_t line_end = (eol == std::string::npos) ? text.size() : eol;
        const char* line = text.c_str() + pos;
        const size_t line_len = line_end - pos;
        pos = line_end + 1;

        unsigned long long start = 0, end = 0, file_offset = 0;
        char perms[5] = {};
        if (sscanf(line, "%llx-%llx %4s %llx", &start, &end, perms, &file_offset) != 4)
            continue;
        const char* slash = strchr(line, '/');
        if (!slash) continue;
        const size_t path_off = (size_t)(slash - line);
        if (path_off >= line_len) continue;
        size_t name_len = line_len - path_off;
        while (name_len > 0 && (slash[name_len - 1] == '\r' || slash[name_len - 1] == ' '))
            --name_len;
        static const char kDeleted[] = " (deleted)";
        const size_t deleted_len = sizeof(kDeleted) - 1;
        bool deleted = false;
        if (name_len > deleted_len &&
            strncmp(slash + name_len - deleted_len, kDeleted, deleted_len) == 0) {
            name_len -= deleted_len;
            deleted = true;
        }
        if (name_len < lib_len) continue;
        if (strncmp(slash + name_len - lib_len, lib, lib_len) != 0) continue;
        if (name_len > lib_len && slash[name_len - lib_len - 1] != '/') continue;

        const uint64_t load_bias = (uint64_t)start - (uint64_t)file_offset;
        // Живой образ (не «(deleted)») и с нулевым смещением — лучший кандидат.
        if (file_offset == 0 && !deleted) add(primary, primary_count, 8, (uint64_t)start);
        else add(secondary, secondary_count, 8, load_bias);
    }

    // Сортировка по возрастанию адреса внутри каждой группы: порядок карты не
    // гарантирован, а он у нас раньше и решал, какой образ достанется.
    auto sort_asc = [](uint64_t* list, int count) {
        for (int i = 1; i < count; ++i) {
            const uint64_t value = list[i];
            int j = i - 1;
            while (j >= 0 && list[j] > value) { list[j + 1] = list[j]; --j; }
            list[j + 1] = value;
        }
    };
    sort_asc(primary, primary_count);
    sort_asc(secondary, secondary_count);

    int written = 0;
    auto push = [&](uint64_t value) {
        for (int i = 0; i < written; ++i) if (out[i] == value) return;
        if (written < max) out[written++] = value;
    };
    for (int i = 0; i < primary_count; ++i) push(primary[i]);
    for (int i = 0; i < secondary_count; ++i) push(secondary[i]);
    return written;
}

}  // namespace maps
