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
// значит её можно проверять на хосте (см. tools/maps).

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

}  // namespace maps
