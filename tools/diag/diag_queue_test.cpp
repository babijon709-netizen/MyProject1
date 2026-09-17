// tools/diag/diag_queue_test.cpp — журнал здоровья не должен тормозить кадр.
//
// Проверяется ровно то, из-за чего переписан app/diag_log.cpp:
//   * запись идёт в отдельном потоке, поэтому зовущий diag_log() только кладёт
//     строку в буфер;
//   * при переполнении буфера событие отбрасывается СО СЧЁТЧИКОМ (в файле
//     появляется «потеряно событий»), а не молча;
//   * повтор одной и той же строки схлопывается в счётчик;
//   * всё, что попало в буфер, доходит до файла после diag_flush.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <vector>

#include "app/diag_log.h"

static int g_fail = 0;

static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "OK  " : "ПРОВАЛ", what);
    if (!ok) ++g_fail;
}

static double mono() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static std::string read_all(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return std::string();
    std::string out;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static int count_lines_with(const std::string& text, const char* needle) {
    int count = 0;
    size_t pos = 0;
    const size_t len = strlen(needle);
    while ((pos = text.find(needle, pos)) != std::string::npos) { ++count; pos += len; }
    return count;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("нужен каталог журнала\n"); return 2; }
    diag_init(argv[1]);
    if (!diag_enabled()) { printf("  ПРОВАЛ журнал не открылся\n"); return 1; }

    const std::string path = diag_path();
    check(path.find("xvcen_health.log") != std::string::npos, "путь журнала — xvcen_health.log");

    // 1. Обычная нагрузка: 300 разных событий — все должны дойти.
    const double t0 = mono();
    for (int i = 0; i < 300; ++i) diag_log("stand", "событие номер %d", i);
    const double elapsed = mono() - t0;
    diag_flush(2000);
    std::string text = read_all(path.c_str());
    int found = 0;
    for (int i = 0; i < 300; ++i) {
        char needle[64];
        snprintf(needle, sizeof(needle), "событие номер %d\n", i);
        if (text.find(needle) != std::string::npos) ++found;
    }
    check(found == 300, "все 300 событий дошли до файла (после flush)");
    check(elapsed < 0.25, "300 вызовов diag_log заняли меньше 0,25 с (диск не на этом потоке)");

    // 2. Повтор: та же строка 50 раз — одна строка плюс счётчик подавленных.
    for (int i = 0; i < 50; ++i) diag_log("stand", "одно и то же");
    diag_log("stand", "разное");
    diag_flush(2000);
    text = read_all(path.c_str());
    check(count_lines_with(text, "одно и то же\n") == 1, "повтор схлопнулся в одну строку");
    check(text.find("повторилось") != std::string::npos, "число подавленных повторов сохранено");

    // 3. Буря событий: буфер заведомо переполнится. Главное — не зависнуть и не
    //    потерять молча.
    const double burst_start = mono();
    for (int i = 0; i < 20000; ++i) diag_log("stand", "буря номер %d", i);
    const double burst_elapsed = mono() - burst_start;
    diag_flush(4000);
    text = read_all(path.c_str());
    const int last_events = count_lines_with(text, "буря номер ");
    check(burst_elapsed < 3.0, "20000 вызовов вернулись быстро (зовущий не ждёт диск)");
    check(last_events > 0 && (last_events == 20000 || text.find("потеряно событий") != std::string::npos),
          "потеря при переполнении видна строкой «потеряно событий»");

    // 4. Формат строки: «секунды тег текст».
    const bool has_prefix = text.find(" stand ") != std::string::npos;
    check(has_prefix, "в строке есть секунды, тег и текст");

    printf("%s\n", g_fail == 0 ? "стенд журнала: ПРОЙДЕН" : "стенд журнала: ПРОВАЛ");
    return g_fail == 0 ? 0 : 1;
}
