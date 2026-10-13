#pragma once
// vis_diag.h — CLI-команда mesh: прогнать сбор геометрии мира на живом
// процессе игры и напечатать, что из этого вышло.
//
//     su -c /data/local/tmp/xvcen mesh                  // режим из .mode
//     su -c /data/local/tmp/xvcen mesh external         // режим аргументом
//     su -c /data/local/tmp/xvcen mesh rt-dev 12345     // режим + конкретный pid
//
// Зачем: сбор мешей — самая офсетозависимая часть визибл-чека и единственная,
// которая молча ничего не делает. Если раскладка нативного Mesh не нашлась
// (другой Unity или CPU-копии мешей выгружены), тумблер «Учёт стен» просто не
// меняет поведение, и по логу это неотличимо от «фича выключена». Эта команда
// печатает выбранную раскладку, число собранных треугольников и два
// контрольных луча — по ним за десять секунд видно, работает ли сбор вообще.
//
// Как и dump/skel, это отдельный короткоживущий процесс: без GUI и маскировки,
// транспорт — аргумент → .mode → трейлер конфига.
//
// Коды выхода: 0 — геометрия собрана, 1 — игра не найдена или транспорт не
// привязался, 3 — привязка не подтвердила класс (esp_init отказал),
// 4 — игра жива, но геометрии нет (см. отчёт: раскладка / CPU-копии).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "config.h"          // CfgDir()
#include "game.h"            // esp_init, esp_vis_mesh_diag, ESP_MEM_*
#include "game_internal.h"   // g_mem
#include "maps_lookup.h"     // maps::lookup_library_bases
#include "metadata_dump.h"   // metadump::Log, read_proc_maps, saved_backend
#include "process.h"         // process_find_game_pid, process_find_unity_candidates

namespace visdiag {

static int Run(int argc, char* argv[]) {
    // argv[0] — само слово «mesh»; дальше опционально режим и/или pid.
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
            printf("использование: xvcen mesh [external|ft|rt-dev|rt-hook] [pid]\n");
            return 1;
        }
    }
    int backend = -1;
    if (mode_arg) {
        backend = metadump::ParseBackendArg(mode_arg);
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
    log.open((dir + "/mesh.log").c_str());
    log.say("== xvcen mesh: сбор геометрии мира ==");
    log.say("uid/euid : %d / %d%s", (int)getuid(), (int)geteuid(),
            geteuid() == 0 ? " (root)" : "  <-- НЕ ROOT");
    if (geteuid() != 0) log.say("!! нужен root: su -c <путь>/xvcen mesh");
    log.say("режим    : %s%s", metadump::backend_name(backend),
            mode_arg ? " (аргументом)" : " (сохранённый)");

    esp_set_mem_backend(backend);
    metadump::PrepareOneShotBackend(backend);

    std::vector<pid_t> pids;
    if (want_pid > 0) {
        pids.push_back(want_pid);
    } else {
        const pid_t official = process_find_game_pid();
        if (official > 0) {
            pids.push_back(official);
        } else {
            pid_t cand[16] = {};
            const int n = process_find_unity_candidates(cand, 16);
            for (int i = 0; i < n; ++i) pids.push_back(cand[i]);
        }
    }
    if (pids.empty()) {
        log.say("!! игра не найдена — запусти её и зайди на сервер");
        return 1;
    }

    bool bound = false;
    pid_t used_pid = 0;
    for (const pid_t pid : pids) {
        std::string text;
        if (!metadump::read_proc_maps(pid, &text)) continue;
        log.say("процесс  : pid %d (%s)", (int)pid, metadump::proc_cmdline(pid).c_str());
        std::uint64_t bases[8] = {};
        const int nb = maps::lookup_library_bases(text, "libil2cpp.so", bases, 8);
        for (int i = 0; i < nb; ++i)
            if (g_mem.bind(pid, bases[i])) {
                bound = true;
                break;
            }
        if (bound) {
            used_pid = pid;
            break;
        }
        log.say("   не привязалось (errno %d)", esp_mem_last_errno());
    }
    if (!bound) {
        log.say("!! транспорт не читает память игры: errno %d.", esp_mem_last_errno());
        log.say("   Проверь драйвер:  xvcen --check-driver");
        log.say("   Или задай режим явно:  xvcen mesh external");
        return 1;
    }
    log.say("привязка : pid %d, %s", (int)used_pid, g_mem.backend_name());

    if (!esp_init(used_pid)) {
        log.say("!! esp_init не подтвердил игру: состояние %d.", (int)esp_attach_state());
        return 3;
    }

    static char report[8192];
    const int written = esp_vis_mesh_diag(report, sizeof(report));
    char* cursor = report;
    while (cursor && *cursor) {
        char* nl = strchr(cursor, '\n');
        if (nl) *nl = '\0';
        log.say("%s", cursor);
        cursor = nl ? nl + 1 : nullptr;
    }
    if (written <= 0) {
        log.say("!! отчёт пуст — буфер не задан");
        return 4;
    }
    // Код 4 — это «геометрии нет»: причина всегда видна в самом отчёте
    // (состояние 5 = раскладка не найдена, 6 = нет входа в сцену).
    const int rc = (strstr(report, "состояние : готово") != nullptr) ? 0 : 4;
    log.say("== конец диагностики (код %d) ==", rc);
    log.say("отправь этот вывод (или %s/mesh.log) разработчику", CfgDir());
    return rc;
}

}  // namespace visdiag
