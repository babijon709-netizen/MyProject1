#pragma once
// stealth.h — маскировка процесса: имя в списках запущенных, защита от
// отладчика и удаление бинарника с диска на время работы.
//
// Поверхности видимости, которые закрывает модуль (всё из userspace):
//   /proc/<pid>/cmdline — фейк «vold …» поверх области argv (область
//       ограничена длиной командной строки запуска: ядро читает
//       arg_start..arg_end, растянуть её из userspace нельзя);
//   /proc/<pid>/environ — строки окружения зануляются (su/TERM и пр.);
//   /proc/<pid>/status  — Name = «vold» (PR_SET_NAME); новые потоки
//       наследуют это имя; PR_SET_DUMPABLE=0 закрывает /proc/<pid>
//       чужим не-root процессам;
//   /proc/<pid>/exe     — после unlink путь показывает «(deleted)»,
//       самого файла на диске нет;
//   /proc/<pid>/fd      — 0/1/2 уходят с терминала в /dev/null (TTY «?»
//       в ps после setsid), fd-лист чист (см. kernel_io/mem_io);
//   отладчики           — потомок занимает ptrace-слот (PTRACE_SEIZE,
//       БЕЗ остановки родителя): gdb/lldb/Frida получают EPERM.
//
// Не покрывается из userspace (честно): /proc/<pid>/maps всё равно
// показывает региона с меткой «(deleted)»; ppid остаётся su/shell;
// TracerPid != 0 в status (плата за занятый ptrace-слот); SIGKILL —
// файл не восстановить.
//
// Восстановление бинарника: при нормальном выходе (teardown в main),
// exit()/atexit и по фатальным сигналам (SEGV/ABRT/BUS/ILL/FPE) файл
// пишется обратно из /proc/self/exe (иннод держат отображения, канал
// читаем и после unlink). После SIGKILL/ребута — файл придётся
// перекинуть заново.

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__) || defined(__linux__)
#include <sys/ptrace.h>
#define STEALTH_PTRACE_OK 1
#else
#define STEALTH_PTRACE_OK 0
#endif

namespace stealth {

inline constexpr const char kComm[] = "vold";   // comm ≤ 15 символов

// Состояние для восстановления бинарника на выходе.
inline char   g_exe_path[4096] = {};
inline mode_t g_exe_mode = 0755;

// ---- восстановление бинарника из /proc/self/exe ---------------------------
inline void RestoreBinary() {
    if (!g_exe_path[0]) return;
    // Путь сначала копируем и только потом помечаем «выполнено»: сам файл
    // открываем уже по копии, а не по затёртому g_exe_path.
    char path[4096];
    memcpy(path, g_exe_path, sizeof(path));
    g_exe_path[0] = 0;
    const int in = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (in < 0) return;
    const int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) { close(in); return; }
    char buf[65536];
    ssize_t r;
    while ((r = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            const ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w <= 0) { close(in); close(out); return; }
            off += w;
        }
    }
    fchmod(out, g_exe_mode);
    close(in);
    close(out);
}

// ---- фатальные сигналы: вернуть файл и отдать сигнал дальше ---------------
// Не строго async-signal-safe (write/open), но это лучший шанс оставить
// бинарник на диске после падения; падший процесс всё равно умирает.
inline void CrashHandler(int sig) {
    RestoreBinary();
    signal(sig, SIG_DFL);
    raise(sig);
}

// ---- 1) маскировка имени: cmdline/environ/comm/dumpable -------------------
// Вызывать строго ПОСЛЕ разбора аргументов (строки argv затираются) и до
// создания потоков (наследуют comm).
inline void ApplyEarly(int argc, char* argv[]) {
    if (!argv || !argv[0]) return;

    // Границы области cmdline: строки argv лежат в стеке подряд, ядро
    // показывает /proc/<pid>/cmdline ровно от arg_start до arg_end.
    char* begin = argv[0];
    char* end = begin + strlen(begin) + 1;
    for (int i = 1; i < argc; ++i) {
        if ((char*)argv[i] > begin && (char*)argv[i] < begin + 0x8000) {
            const char* e = argv[i] + strlen(argv[i]) + 1;
            if (e > end) end = (char*)e;
        }
    }
    const size_t cap = (size_t)(end - begin);

    // Окружение зануляем: /proc/<pid>/environ читается root'ом и выдаёт
    // следы запуска (su, TERM, путь шелла). getenv в коде не используется.
    // (environ объявлен глобально в <unistd.h>.)
    if (environ)
        for (char** e = environ; *e; ++e) memset(*e, 0, strlen(*e));

    // Фейк cmdline: самый длинный из влезающих в область (область = длина
    // командной строки запуска). Остаток — пробелы: ни кусков настоящей
    // строки, ни пустых «аргументов» из нулей в хвосте.
    static const char kFakeLong[] =
        "/system/bin/vold --blkidcontext=u:r:blkid:s0 --fsckcontext=u:r:fsck:s0";
    static const char kFakeMid[]  = "/system/bin/vold";
    static const char kFakeMin[]  = "vold";
    if (cap >= 2) {
        const char* fake = (sizeof(kFakeLong) <= cap) ? kFakeLong
                        : (sizeof(kFakeMid)  <= cap) ? kFakeMid
                        : (sizeof(kFakeMin)  <= cap) ? kFakeMin : nullptr;
        memset(begin, ' ', cap);
        begin[cap - 1] = 0;
        if (fake) memcpy(begin, fake, strlen(fake));
    }

    prctl(PR_SET_NAME, (unsigned long)kComm, 0, 0, 0);
    // /proc/<pid> — только root; core-дампы не собираются.
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
}

// ---- 2) рантайм-скрытие: терминал, ptrace-слот, файл ----------------------
// Вызывать после успешного подъёма оверлея и тача (если GUI не поднялся —
// main успевает вернуть -1 с файлом на месте), до экрана выбора режима.
inline void ApplyRuntime() {
    // Отвязка от терминала: без сессии ps показывает TTY «?». Отказ не
    // критичен (если мы уже лидер сессии).
    setsid();

    // 0/1/2 больше не указывают на /dev/pts: в fd-листе чита нет терминала,
    // из которого его запустили.
    const int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nul >= 0) {
        dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
        if (nul > 2) close(nul);
    }

    // Убираем бинарник с диска. Файл исчезает из файловых менеджеров и
    // ls; /proc/<pid>/exe продолжает работать (иннод держат отображения)
    // и служит источником для восстановления на выходе.
    const ssize_t n = readlink("/proc/self/exe", g_exe_path, sizeof(g_exe_path) - 1);
    if (n > 0) {
        g_exe_path[n] = 0;
        struct stat st {};
        if (stat(g_exe_path, &st) == 0) g_exe_mode = st.st_mode & 0777;
        if (unlink(g_exe_path) != 0) g_exe_path[0] = 0;
    } else {
        g_exe_path[0] = 0;
    }

    // Нормальный выход и exit() возвращают файл на место.
    atexit(RestoreBinary);
    struct sigaction sa {};
    sa.sa_handler = CrashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

#if STEALTH_PTRACE_OK
    // Потомок занимает ptrace-слот: PTRACE_SEIZE не останавливает родителя
    // и не требует никакого обслуживания, но второй tracer невозможен —
    // attach gdb/lldb/Frida заканчивается EPERM. Потомок живёт, пока жив
    // родитель (PDEATHSIG + контроль getppid), и не держит никаких fd.
    const pid_t parent = getpid();
    const pid_t child = fork();
    if (child == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        if (ptrace(PTRACE_SEIZE, (pid_t)parent, nullptr, nullptr) != 0) _exit(0);
        for (;;) {
            if (getppid() != parent) _exit(0);
            sleep(2);
        }
    }
#endif
}

}  // namespace stealth
