#pragma once
// skeleton_diag.h — CLI-команда skel: прогон конвейера костей на живом
// процессе игры и печать того, на каком шаге он рассыпается.
//
//     su -c /data/local/tmp/xvcen skel                  // режим из конфига
//     su -c /data/local/tmp/xvcen skel external         // режим аргументом
//     su -c /data/local/tmp/xvcen skel rt-dev 12345     // режим + конкретный pid
//
// Зачем: скелеты — самый офсетозависимый визуал. characterModel, имя
// GameObject, раскладка массивов иерархии и слоты KCC/Ragdoll — всё это
// смещения, которые обфускатор перекатывает на каждом обновлении игры, а
// часть из них (раскладка, имя) чит находит сам, на живой памяти. Когда
// пользователь пишет «боксы есть, скелетов нет», лога на устройстве нет
// (logfile.h — заглушка), и единственный способ понять причину — прогнать
// конвейер вручную и посмотреть, какой шаг не сошёлся.
//
// Печатает: транспорт и привязку, счётчики чтений (в том числе метки TBI и
// мусорные адреса), найденные раскладки и смещения, а по каждому игроку —
// трансформ, корень модели, сколько имён прочиталось и КАК они выглядят
// (по именам видно, тот ли это риг), сколько костей дали мировую позицию и
// сколько доехало до экрана.
//
// Как и dump, это отдельный короткоживущий процесс: без GUI и маскировки,
// транспорт — из конфига или аргументом, без фоллбэков.
//
// Коды выхода: 0 — скелеты строятся, 1 — игра не найдена или транспорт не
// привязался, 2 — привязались, но ни у одного игрока скелет не собрался,
// 3 — привязка к игре не подтвердила класс (esp_init отказал).

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"          // CfgDir()
#include "game.h"            // esp_init, esp_skeleton_diag, ESP_MEM_*
#include "game_internal.h"   // g_mem
#include "maps_lookup.h"     // maps::lookup_library_bases
#include "metadata_dump.h"   // metadump::Log, read_proc_maps, saved_backend
#include "process.h"         // process_find_game_pid, process_find_unity_candidates

namespace skeldiag {

static int Run(int argc, char* argv[]) {
    // argv[0] — само слово «skel»; дальше опционально режим и/или pid.
    const char* mode_arg = nullptr;
    pid_t want_pid = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!a || !a[0]) continue;
        if (a[0] >= '0' && a[0] <= '9') {
            want_pid = (pid_t)atoi(a);
        } else if (!mode_arg) {
            mode_arg = a;
        } else {
            printf("не понимаю аргумент: %s\n", a);
            printf("использование: xvcen skel [external|ft|rt-dev|rt-hook] [pid]\n");
            return 1;
        }
    }
    int backend = -1;
    if (mode_arg) {
        if (!strcmp(mode_arg, "external") || !strcmp(mode_arg, "ext"))
            backend = ESP_MEM_EXTERNAL;
        else if (!strcmp(mode_arg, "ft") || !strcmp(mode_arg, "kernel"))
            backend = ESP_MEM_FT;
        else if (!strcmp(mode_arg, "rt-dev") || !strcmp(mode_arg, "rt"))
            backend = ESP_MEM_RT_DEV;
        else if (!strcmp(mode_arg, "rt-hook"))
            backend = ESP_MEM_RT_HOOK;
        if (backend < 0) {
            printf("неизвестный режим: %s (external | ft | rt-dev | rt-hook)\n", mode_arg);
            return 1;
        }
    } else {
        backend = metadump::saved_backend();
    }

    const std::string dir = std::string(CfgDir());
    mkdir(CfgDir(), 0777);

    metadump::Log log;
    log.open((dir + "/skel.log").c_str());
    log.say("== xvcen skel: диагностика скелетов ==");
    log.say("uid/euid : %d / %d%s", (int)getuid(), (int)geteuid(),
            geteuid() == 0 ? " (root)" : "  <-- НЕ ROOT");
    if (geteuid() != 0)
        log.say("!! нужен root: su -c <путь>/xvcen skel");
    log.say("режим    : %s%s", metadump::backend_name(backend),
            mode_arg ? " (аргументом)" : " (из конфига)");

    // Транспорт назначается один раз на жизнь процесса (§2 KERNEL.md), до
    // первого bind и без фоллбэков — как у команды dump.
    esp_set_mem_backend(backend);

    std::vector<pid_t> pids;
    if (want_pid > 0) {
        pids.push_back(want_pid);
    } else {
        const pid_t official = process_find_game_pid();
        if (official > 0) pids.push_back(official);
        else {
            pid_t cand[16] = {};
            const int n = process_find_unity_candidates(cand, 16);
            for (int i = 0; i < n; ++i) pids.push_back(cand[i]);
        }
    }
    if (pids.empty()) {
        log.say("!! игра не найдена — запусти её и зайди на сервер "
                "(нужен живой список игроков)");
        return 1;
    }

    bool bound = false;
    pid_t used_pid = 0;
    for (const pid_t pid : pids) {
        std::string text;
        if (!metadump::read_proc_maps(pid, &text)) continue;
        log.say("процесс  : pid %d (%s)", (int)pid,
                metadump::proc_cmdline(pid).c_str());
        std::uint64_t bases[8] = {};
        const int nb = maps::lookup_library_bases(text, "libil2cpp.so", bases, 8);
        for (int i = 0; i < nb; ++i)
            if (g_mem.bind(pid, bases[i])) { bound = true; break; }
        if (bound) { used_pid = pid; break; }
        log.say("   не привязалось (errno %d)", esp_mem_last_errno());
    }
    if (!bound) {
        log.say("!! транспорт не читает память игры: errno %d.", esp_mem_last_errno());
        log.say("   Проверь драйвер:  xvcen --check-driver");
        log.say("   Или задай режим явно:  xvcen skel external");
        return 1;
    }
    log.say("привязка : pid %d, %s", (int)used_pid, g_mem.backend_name());

    // Полная инициализация чита: она же резолвит классы il2cpp. Отказ здесь
    // сам по себе диагноз — офсеты не соответствуют сборке игры.
    if (!esp_init(used_pid)) {
        log.say("!! esp_init не подтвердил игру: состояние %d. "
                "Сборка игры не совпадает с офсетами, или игра ещё грузится.",
                (int)esp_attach_state());
        return 3;
    }

    static char report[16384];
    const int rc = esp_skeleton_diag(report, sizeof(report), 4);
    // Построчно: Log сам дублирует вывод в stdout.
    char* cursor = report;
    while (cursor && *cursor) {
        char* nl = strchr(cursor, '\n');
        if (nl) *nl = '\0';
        log.say("%s", cursor);
        cursor = nl ? nl + 1 : nullptr;
    }
    log.say("== конец диагностики (код %d) ==", rc);
    log.say("отправь этот вывод (или %s/skel.log) разработчику", CfgDir());
    return rc;
}

}  // namespace skeldiag
