#pragma once
// Выбор стартового конфига. Один селектор на всех: и загрузка при старте,
// и восстановление языка/акцента/раскладки/режима читают ровно один файл.
// Не ограничен списком UI (kMaxConfigs) — иначе при большом числе конфигов
// загрузка и восстановление брали разные файлы.
#include <dirent.h>
#include <sys/stat.h>
#include <ctime>
#include <string>

inline bool CfgTimeNewer(const struct timespec& a, const struct timespec& b) {
    if (a.tv_sec != b.tv_sec) return a.tv_sec > b.tv_sec;
    return a.tv_nsec > b.tv_nsec;
}

// dir с завершающим '/'. outName — имя без ".cfg". Ничья по mtime — по имени.
inline bool CfgPickNewest(const char* dir, std::string& outName) {
    DIR* d = opendir(dir);
    if (!d) return false;
    bool found = false;
    struct timespec best{};
    std::string bestName;
    while (struct dirent* e = readdir(d)) {
        const std::string fn = e->d_name;
        if (fn.size() <= 4 || fn.compare(fn.size() - 4, 4, ".cfg") != 0) continue;
        struct stat st{};
        if (stat((std::string(dir) + fn).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        const std::string stem = fn.substr(0, fn.size() - 4);
        const bool take = !found
            || CfgTimeNewer(st.st_mtim, best)
            || (st.st_mtim.tv_sec == best.tv_sec && st.st_mtim.tv_nsec == best.tv_nsec && stem > bestName);
        if (take) {
            found = true;
            best = st.st_mtim;
            bestName = stem;
        }
    }
    closedir(d);
    if (found) outName = bestName;
    return found;
}
