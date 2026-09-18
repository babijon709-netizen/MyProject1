// Стенд доступа к памяти чужого процесса (jni/src/mem_io.h).
//
//   sh tools/memio/run.sh
//
// Почему он есть: слой чтения памяти игры ходит в /proc/<pid>/mem и кэширует
// блоки, а проверить это на телефоне из песочницы нельзя. Здесь то же самое, но
// на хосте: стенд порождает процесс-ребёнка со страницей узора и читает его
// память ровно теми же функциями, которыми чит читает игру.
//
// Проверки:
//   1) привязка открывает /proc/<pid>/mem и видит ELF-заголовок (здесь — метку,
//      подложенную ребёнком) — то, чем bind() проверяет доступ;
//   2) чтения через кэш отдают данные ребёнка байт в байт (идут тем же pread,
//      что и на устройстве);
//   3) кэш блоков: 200 мелких чтений внутри блока = 1 syscall, соседний блок = +1;
//   4) начало кадра (frame_begin) сбрасывает кэш — данные перечитываются;
//   5) запись доходит до процесса-ребёнка и не отдаётся из кэша;
//   6) «привязка ещё жива» (verify) истинна для живого процесса и ложна после
//      его смерти или когда доступ отобран (ребёнок помечен неdumpable);
//   7) обрыв на границе отображения и нечитаемая страница — разные вещи: обрыв
//      (начало адреса читается, дальше памяти нет) не считается потерей доступа,
//      а нечитаемая страница (как память имён классов на устройстве из лога) —
//      считается, с errno EIO;
//   8) привязка к несуществующему pid честно отказывает, а не «удаётся» молча.

#include "mem_io.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>

static int g_failed = 0;

static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "OK  " : "ПРОВАЛ", what);
    if (!ok) ++g_failed;
}

// ---- процесс-ребёнок -------------------------------------------------------
// Две страницы с узором: байт i = (uint8_t)(i * 7 + 13). Первые 4 байта — метка,
// которую bind() читает как «ELF-заголовок». Через pipe отдаёт базовый адрес и
// умеет по команде вернуть свой байт по смещению 0x40 (проверка записи).
struct Child {
    pid_t pid = -1;
    int   to_child = -1;     // родитель -> ребёнок: команды
    int   from_child = -1;   // ребёнок -> родитель: данные
    uint64_t base = 0;
    size_t   length = 0;
};

static uint8_t pattern(size_t i) { return (uint8_t)(i * 7 + 13); }

static bool spawn_child(Child& c, bool dumpable) {
    int pc[2], cp[2];
    if (pipe(pc) != 0 || pipe(cp) != 0) return false;
    const size_t pages = 2;
    const size_t len = pages * 4096;
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // В ребёнке.
        close(pc[1]); close(cp[0]);
        void* mem = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) _exit(2);
        uint8_t* p = (uint8_t*)mem;
        for (size_t i = 0; i < len; ++i) p[i] = pattern(i);
        p[0] = 0x7F; p[1] = 'E'; p[2] = 'L'; p[3] = 'F';   // «ELF-заголовок»
        if (!dumpable) prctl(PR_SET_DUMPABLE, 0);
        const uint64_t addr = (uint64_t)mem;
        if (write(cp[1], &addr, sizeof(addr)) != (ssize_t)sizeof(addr)) _exit(3);
        for (;;) {
            char cmd = 0;
            if (read(pc[0], &cmd, 1) != 1) break;
            if (cmd == 'q') break;
            if (cmd == 'n') {                    // отдать вторую страницу (munmap)
                const char ack = munmap(p + 4096, len - 4096) == 0 ? 'y' : 'n';
                if (write(cp[1], &ack, 1) != 1) break;
            }
            if (cmd == 'c') {                    // «что у тебя по смещению 0x40?»
                const uint8_t v = p[0x40];
                if (write(cp[1], &v, 1) != 1) break;
            }
        }
        _exit(0);
    }
    close(pc[0]); close(cp[1]);
    c.pid = pid; c.to_child = pc[1]; c.from_child = cp[0];
    c.length = len;
    uint64_t addr = 0;
    if (read(c.from_child, &addr, sizeof(addr)) != (ssize_t)sizeof(addr)) return false;
    c.base = addr;
    return true;
}

static void kill_child(Child& c) {
    if (c.pid > 0) { kill(c.pid, SIGKILL); waitpid(c.pid, nullptr, 0); }
    if (c.to_child >= 0) close(c.to_child);
    if (c.from_child >= 0) close(c.from_child);
    c.pid = -1; c.to_child = -1; c.from_child = -1;
}

static bool child_unmap_second_page(Child& c) {
    const char cmd = 'n';
    if (write(c.to_child, &cmd, 1) != 1) return false;
    char ack = 0;
    return read(c.from_child, &ack, 1) == 1 && ack == 'y';
}

static int child_byte(Child& c) {
    const char cmd = 'c';
    if (write(c.to_child, &cmd, 1) != 1) return -1;
    uint8_t v = 0;
    if (read(c.from_child, &v, 1) != 1) return -1;
    return v;
}

int main() {
    printf("--- стенд доступа к памяти другого процесса\n");
    Child child;
    if (!spawn_child(child, true)) {
        printf("  ПРОВАЛ не удалось запустить процесс-ребёнка\n");
        return 1;
    }

    // 1) Привязка выбирает рабочий способ.
    memio::Reader r;
    check(r.bind(child.pid, child.base), "привязка к живому процессу удалась");
    printf("      способ: %s\n", r.backend_name());
    check(r.bound() && strcmp(r.backend_name(), "/proc/<pid>/mem") == 0,
          "доступ идёт через /proc/<pid>/mem");
    check(r.verify(), "проверка привязки (verify) — заголовок по адресу пробы читается");

    // 2) Чтения отдают данные ребёнка байт в байт (в том числе через кэш).
    {
        bool same = true;
        int k = 0;
        for (size_t off = 0x10; off < 0x400; off += 0x37, ++k) {
            uint8_t v = 0;
            if (!r.read(child.base + off, &v, 1) || v != pattern(off)) { same = false; break; }
        }
        check(same, "чтения из /proc/<pid>/mem совпали с узором ребёнка (26 адресов)");
    }

    // 3) Кэш блоков: мелкие чтения внутри блока — один syscall.
    r.frame_begin();
    r.reset_stats();
    {
        bool ok = true;
        for (int i = 0; i < 200; ++i) {
            // 8..112: первые 4 байта страницы заняты меткой «ELF», по ней bind()
            // и проверяет доступ — узора там уже нет.
            const size_t off = (size_t)(1 + i % 14) * 8;   // тот же 128-байтный блок
            uint64_t v = 0;
            if (!r.read(child.base + off, &v, 8)) { ok = false; break; }
            if ((uint8_t)v != pattern(off)) { ok = false; break; }
        }
        check(ok, "200 чтений по 8 байт из одного блока отдали верные данные");
        check(r.syscalls == 1, "на 200 чтений — 1 syscall (кэш блока)");
        printf("      syscalls=%llu, попаданий в кэш=%llu\n", r.syscalls, r.hits);

        uint64_t v = 0;
        r.read(child.base + 0x100, &v, 8);               // соседний блок
        check(r.syscalls == 2, "соседний блок — ещё один syscall");
    }

    // 4) Начало кадра сбрасывает кэш.
    {
        const unsigned long long before = r.syscalls;
        r.frame_begin();
        uint64_t v = 0;
        r.read(child.base + 0x10, &v, 8);
        check(r.syscalls == before + 1, "после frame_begin данные перечитываются из процесса");
        check((uint8_t)v == pattern(0x10), "значение верное и после сброса кэша");
    }

    // 5) Запись доходит до ребёнка и не отдаётся из кэша.
    {
        const uint8_t want = 0xAB;
        uint8_t got = 0;
        r.read(child.base + 0x40, &got, 1);              // прогреваем кэш блока
        check(r.write(child.base + 0x40, &want, 1), "запись в память ребёнка удалась");
        got = 0;
        check(r.read(child.base + 0x40, &got, 1) && got == want,
              "после записи читается новое значение (блок не отдан из кэша)");
        check(child_byte(child) == want, "ребёнок видит новое значение в своей памяти");
    }

    // 6) Метка в старшем байте адреса (TBI/MTE). Указатели, вычитанные из памяти
    //    процесса на Android 11+, приходят именно так: метку ставит аллокатор, а
    //    ядро при чтении чужой памяти её не снимает — pread по такому адресу
    //    возвращает EIO. Слой доступа обязан снять метку сам.
    {
        const uint64_t tagged_default = child.base + 0x40;
        const uint64_t tagged = tagged_default | 0xAB00000000000000ULL;
        uint8_t want = 0x5C, got = 0;
        // «Как было»: прямой pread по помеченному адресу — так делал прежний код.
        char raw_path[64] = {};
        snprintf(raw_path, sizeof(raw_path), "/proc/%d/mem", (int)child.pid);
        int fd = open(raw_path, O_RDWR | O_CLOEXEC);
        ssize_t raw = fd < 0 ? -1 : pread(fd, &got, 1, (off_t)tagged);
        if (fd >= 0) close(fd);
        check(raw < 0, "прямой pread по адресу с меткой отказывает (так было)");

        const unsigned long long tagged_before = r.tagged_total();
        got = 0;
        const bool ok = r.write(tagged, &want, 1) && r.read(tagged, &got, 1) && got == want;
        check(ok, "через слой доступа адрес с меткой читается и пишется (метка снята)");
        check(r.tagged_total() > tagged_before, "слой доступа посчитал помеченный адрес");
        check(child_byte(child) == want, "запись по адресу с меткой дошла до процесса");
    }

    // 7) Обрыв на границе отображения и нечитаемая страница — два разных случая.
    //    С устройства пришёл лог, где чтения имён классов падали с errno 5: память
    //    метаданных (там лежат имена классов) на том устройстве не читается вовсе,
    //    хотя всё остальное читается: адреса отдают EIO, как у отданной страницы.
    //    Разбираться в логе нужно точно: «начало адреса читается, дальше памяти
    //    нет» — это обрыв, он не значит потерю доступа; «адрес не читается
    //    совсем» — это отказ.
    {
        const uint64_t kPageSize = 4096;
        check(child_unmap_second_page(child), "ребёнок отдал вторую страницу (munmap)");

        const unsigned long long fails_before = r.read_fails_total();
        const unsigned long long cuts_before  = r.cut_total();
        uint8_t buf[8] = {};
        check(!r.read(child.base + kPageSize - 4, buf, sizeof(buf)),
              "чтение, выходящее за конец отображения, отказывает");
        check(r.cut_total() == cuts_before + 1, "такое чтение посчитано обрывом");
        check(r.read_fails_total() == fails_before, "обрыв не засчитан потерей доступа");

        const unsigned long long fails_before2 = r.read_fails_total();
        uint64_t v = 0;
        check(!r.read(child.base + kPageSize + 0x40, &v, sizeof(v)),
              "чтение по нечитаемой странице отказывает");
        check(r.read_fails_total() == fails_before2 + 1,
              "нечитаемая страница — это отказ, а не обрыв");
        check(r.last_error() != 0, "причина отказа записана в счётчиках слоя");
        printf("      errno нечитаемой страницы: %d\n", r.last_error());

        uint64_t first_page = 0;
        check(r.read(child.base + 0x40, &first_page, sizeof(first_page)),
              "первая страница по-прежнему читается (доступ есть)");

        // Чтение «на пробу» (read_quiet) — тот же доступ, но счётчики не трогает:
        // им чит добирает начало строки у границы отображения, где отказ ожидаем.
        const unsigned long long quiet_fails = r.read_fails_total();
        uint8_t one = 0;
        check(r.read_quiet(child.base + kPageSize + 0x40, &one, 1) == false,
              "read_quiet по нечитаемой странице отказывает");
        check(r.read_fails_total() == quiet_fails, "read_quiet не засчитан как отказ");
        one = 0;
        check(r.read_quiet(child.base + 0x48, &one, 1) && one == pattern(0x48),
              "read_quiet читает там, где память есть");
    }

    // 8) Проверка живости: пока процесс жив — истина, после смерти — ложь.
    {
            check(r.verify(), "verify() истинна для живого процесса");
        kill_child(child);
        uint8_t v = 0;
        check(!r.read(child.base, &v, 1), "чтение из мёртвого процесса отказывает");
        check(!r.verify(), "verify() ложна после смерти процесса");
    }

    // 9) Привязка к недоступному процессу честно отказывает.
    {
        Child nd;
        if (spawn_child(nd, false)) {                    // PR_SET_DUMPABLE 0 — как «права отобрали»
            memio::Reader r2;
            check(!r2.bind(nd.pid, nd.base), "к процессу без прав доступа привязки нет");
            check(!r2.verify(), "verify() ложна без доступа");
            kill_child(nd);
        } else {
            check(false, "не удалось запустить процесс для проверки отказа");
        }
        memio::Reader r3;
        check(!r3.bind(999999, 0x400000), "привязка к несуществующему pid отклонена");
    }

    // 10) Замер на «рабочей» нагрузке: 40 объектов по 12 полей, 8 кадров — это
    //    профиль кадра ESP без скелета. «Как было» — каждое поле отдельным
    //    pread, «как стало» — через кэш блоков с началом кадра.
    {
        Child b;
        if (!spawn_child(b, true)) {
            check(false, "не удалось запустить процесс для замера");
        } else {
            constexpr int kObjects = 40;
            constexpr int kFields  = 12;
            constexpr int kRounds  = 8;
            const size_t  stride   = 128;                 // объект = блок
            const size_t  span     = kObjects * stride;
            uint8_t sink = 0;

            uint8_t* local = (uint8_t*)mmap(nullptr, span, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

            char mem_path[40];
            snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", (int)b.pid);
            const int raw_fd = open(mem_path, O_RDONLY | O_CLOEXEC);

            struct timespec t0 {}, t1 {}, t2 {};
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int round = 0; round < kRounds; ++round) {
                for (int obj = 0; obj < kObjects; ++obj) {
                    for (int f = 0; f < kFields; ++f) {
                        pread(raw_fd, local + obj * stride + f * 8, 8,
                              (off_t)(b.base + obj * stride + f * 8));
                        sink ^= local[obj * stride + f * 8];
                    }
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            close(raw_fd);

            memio::Reader br;
            br.bind(b.pid, b.base);
            br.reset_stats();
            for (int round = 0; round < kRounds; ++round) {
                br.frame_begin();
                for (int obj = 0; obj < kObjects; ++obj) {
                    for (int f = 0; f < kFields; ++f) {
                        uint64_t v = 0;
                        br.read(b.base + obj * stride + f * 8, &v, 8);
                        sink ^= (uint8_t)v;
                    }
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &t2);

            const double was_us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
            const double now_us = (t2.tv_sec - t1.tv_sec) * 1e6 + (t2.tv_nsec - t1.tv_nsec) / 1e3;
            const long long was_calls = (long long)kObjects * kFields * kRounds;
            printf("--- замер на %d чтений (40 объектов x 12 полей x 8 кадров):\n",
                   (int)was_calls);
            printf("      было : %8.0f мкс, %lld syscall'ов (по pread на поле)\n", was_us, was_calls);
            printf("      стало: %8.0f мкс, %llu syscall'ов (попаданий в кэш %llu)\n",
                   now_us, br.syscalls, br.hits);
            printf("      выигрыш: x%.1f по времени, x%.1f по числу syscall'ов\n",
                   now_us > 0 ? was_us / now_us : 0.0,
                   br.syscalls ? (double)was_calls / (double)br.syscalls : 0.0);
            check(now_us < was_us, "кэш блоков быстрее прямых чтений");
            check((double)was_calls / (double)(br.syscalls ? br.syscalls : 1) > 10.0,
                  "syscall'ов стало больше чем в 10 раз меньше");
            check(sink != 0x5A, "данные читались (sink ненулевой)");
            munmap(local, span);
            kill_child(b);
        }
    }

    if (g_failed == 0) {
        printf("\nстенд доступа к памяти: ПРОЙДЕН\n");
        return 0;
    }
    printf("\nстенд доступа к памяти: ПРОВАЛЕН (%d проверок)\n", g_failed);
    return 1;
}
