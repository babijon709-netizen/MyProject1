// kernel_io.h — доступ к памяти игры через ядерный драйвер (кернел режим).
//
// Это ОТДЕЛЬНАЯ реализация доступа к памяти: /proc/<pid>/mem не открывается
// вообще — чтение и запись идут через сокет семейства протоколов, которое
// регистрирует драйвер.
//
// Как устроен драйвер (реверс payload'ов из drivers.7z, модуль «bing_rw»,
// автор NULL1337 / t.me/NULLkawaii; протокол того же класса, что публичный
// драйвер gongchuang — архитектура совпадает один в один):
//
//   * .sh-скрипт распаковывает статический инсталлятор, инсталлятор
//     расшифровывает встроенный модуль bing_rw.ko (vermagic под конкретную
//     версию ядра) и грузит его через insmod с трюком
//     import_ns=VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver
//     (обход проверки сигнатур GKI-ядер).
//   * Модуль регистрирует СВОЁ семейство протоколов сокета (sock_register,
//     свободный слот ищется от AF_DECnet=12 вверх) и прячет себя из
//     /proc/modules, /sys/module и vmalloc-списков.
//   * Проба «есть ли драйвер»: socket(семейство, 6, 0). Обычные семейства
//     на таком типе дают EAFNOSUPPORT, а create() драйвера отвечает своим
//     маячком errno 1338 (0x53A). Инсталлятор так ищет семейство, перебирая
//     номера 12..43 — и мы делаем так же.
//   * Рабочий сокет: socket(семейство, 4, 0), только root (uid 0).
//   * Все команды идут через getsockopt(fd, level, optname, optval, optlen):
//       0x300 — чтение памяти:  { u32 pid; u32 pad; u64 addr; u64 buf; u64 size; u32 magic }
//       0x303 — запись памяти:   та же структура
//     Размер запроса 0x28 байт, магия 0x1337 в поле +0x20; направление задаёт
//     код команды. Оба обработчика по дизассемблу зовут общий движок
//     find_get_pid → get_pid_task → get_task_mm → down_read(mmap_lock) →
//     проход по таблицам страниц → copy_to_user / copy_from_user кусками
//     через границу страниц.
//
// Отличие от экстернала: ни fd /proc/pid/mem, ни прав ptrace — ядро само
// находит mm целевого процесса по pid и переносит данные в наш буфер.
// SELinux/yama на /proc при этом не задействованы.
//
// Все константы протокола собраны в kDriver* ниже: если на конкретной сборке
// драйвера коды команд другие — правится одно место.

#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mem_io.h"

namespace memio {

// ---- константы протокола драйвера bing_rw -------------------------------
// Диапазон семейств, в котором драйвер регистрируется: sock_register ищет
// свободный слот начиная с AF_DECnet (12); выше NPROTO (44) слотов нет.
constexpr int kDriverFamilyFirst = 12;
constexpr int kDriverFamilyLast  = 43;
// Тип сокета-маячка: на нём create() драйвера отвечает своим errno.
constexpr int kDriverProbeType   = 6;
// Маячок: errno, которым драйвер отвечает на пробу не тем типом сокета.
constexpr int kDriverProbeErrno  = 1338;
// Тип и протокол рабочего сокета.
constexpr int kDriverSockType    = 4;
constexpr int kDriverSockProto   = 0;
// Коды команд getsockopt (optname).
constexpr int kDriverCmdRead     = 0x300;
constexpr int kDriverCmdWrite    = 0x303;
// Магия в поле +0x20 запроса.
constexpr uint32_t kDriverMagic  = 0x1337;

// Запрос чтения/записи (0x28 байт; раскладка полей — по дизассемблу
// обработчиков команды 0x300: pid из +0x00, addr из +0x08, buf из +0x10,
// size из +0x18, магия +0x20).
#pragma pack(push, 1)
struct DriverRwReq {
    uint32_t pid;          // +0x00: целевой процесс
    uint32_t pad;          // +0x04: выравнивание
    uint64_t addr;         // +0x08: адрес в памяти игры (без TBI-метки)
    uint64_t user_buf;     // +0x10: наш буфер (источник/приёмник данных)
    uint64_t size;         // +0x18: сколько байт
    uint32_t magic;        // +0x20: kDriverMagic
    uint32_t pad2;         // +0x24
};
#pragma pack(pop)
static_assert(sizeof(DriverRwReq) == 0x28, "размер запроса драйвера изменился");

// ---- поиск драйвера -------------------------------------------------------

// Перебираем семейства и ищем маячок (errno 1338 на socket(dom, 6, 0)).
// Возвращает номер семейства или -1. Вызывать от root'а: без root create()
// драйвера отвечает EAFNOSUPPORT и маячок не виден.
inline int driver_find_family() {
    for (int dom = kDriverFamilyFirst; dom <= kDriverFamilyLast; ++dom) {
        errno = 0;
        const int probe = socket(dom, kDriverProbeType, kDriverSockProto);
        if (probe >= 0) {
            // Обычное семейство (netlink, alg…) — не драйвер.
            close(probe);
            continue;
        }
        if (errno == kDriverProbeErrno) return dom;
    }
    return -1;
}

// ---- живая карта команд драйвера ------------------------------------------
//
// Проверка «драйвер действительно переносит данные» и «этот код команды —
// чтение или запись» делается ОДНОЙ пробой против СОБСТВЕННОЙ памяти
// процесса: два буфера с разными известными паттернами, одна команда, и
// смотрим, какой буфер изменился. /proc здесь не участвует — данные могут
// сойтись, только если ядро реально сходило по страницам нашего процесса
// (find_get_pid → get_task_mm → проход таблиц → copy_to/from_user).
//
// Это же — защита от расхождений между сборками драйвера: коды чтения/записи
// ВЫУЧИВАЮТСЯ на месте (learn_commands) по живым пробам, а не зашиты намертво.
// Если драйвер отказывает в доступе к самому себе (бывает у сторонних сборок),
// карта остаётся неизвестной — это НЕ приговор: транспорт к игре проверяется
// по-настоящему при привязке (bind читает ELF-заголовок libil2cpp через
// драйвер, экстернал-путь при этом не задействован вообще).
enum CmdKind {
    CMD_NONE  = 0,   // команда не выполнилась / данные не двигались
    CMD_READ  = 1,   // память процесса скопирована К нам (target -> buf)
    CMD_WRITE = 2,   // наш буфер скопирован В память процесса (buf -> target)
};

struct CmdProbe {
    CmdKind kind = CMD_NONE;
    int     err  = 0;   // errno, если getsockopt отказал
};

// ---- транспорт к памяти игры через драйвер --------------------------------

class KernelTransport : public Transport {
public:
    const char* name() const override { return "kernel (драйвер)"; }

    // Привязка к процессу: рабочего сокета может ещё не быть — поднимаем.
    // pid запоминаем: он уходит в каждый запрос, драйвер сам находит mm.
    bool open(pid_t pid) override {
        if (pid <= 0) return false;
        if (!ensure_socket()) return false;
        pid_ = (int)pid;
        return true;
    }

    // Сокет драйвера живёт, сколько живёт процесс чита: закрывать его на
    // отвязке не нужно (переподключение нужно, только если драйвер выгрузили).
    void close() override { pid_ = -1; }

    // Сокет потерян (драйвер выгрузили/перезагрузили) — перевзять при
    // следующем open(). Вызывается после серии отказов.
    void invalidate() {
        if (sock_ >= 0) {
            ::close(sock_);
            sock_ = -1;
        }
        family_ = -1;
        // Карта команд могла измениться (драйвер перепрошили) — сброс к
        // дефолтам, learn_commands() при следующей проверке выучит заново.
        read_cmd_  = kDriverCmdRead;
        write_cmd_ = kDriverCmdWrite;
        map_known_ = false;
    }

    ssize_t read_once(uint64_t addr, void* out, size_t len) override {
        return rw(read_cmd_, addr, out, len);
    }

    ssize_t write_once(uint64_t addr, const void* in, size_t len) override {
        return rw(write_cmd_, addr, const_cast<void*>(in), len);
    }

    int  family() const { return family_; }
    int  socket_fd() const { return sock_; }
    bool connected() const { return sock_ >= 0; }

    // Поднять рабочий сокет (если ещё не поднят). Возвращает fd или -1.
    int connect() {
        if (!ensure_socket()) return -1;
        return sock_;
    }

    // Выученные коды команд (после learn_commands; до неё — дефолты).
    int  read_cmd()  const { return read_cmd_; }
    int  write_cmd() const { return write_cmd_; }
    // Подтверждена ли карта команд живыми пробами. false — драйвер, похоже,
    // не даёт доступ к собственной памяти: транспорт всё равно рабочий,
    // настоящая проверка — привязка к игре.
    bool map_known() const { return map_known_; }

    // Одна живая проба команды против собственной памяти: заполняем два
    // буфера разными паттернами, отдаём команду «addr=первый, buf=второй»
    // и смотрим, кто куда скопировался. classify(x)==CMD_READ гарантирует,
    // что данные прошли через ядро (экстернал здесь не замешан).
    CmdProbe classify_cmd(int cmd) {
        CmdProbe r;
        if (!ensure_socket()) { r.err = errno ? errno : EIO; return r; }
        uint64_t pat[2]    = { 0x1234567890ABCDEFULL, 0x0F1E2D3C4B5A6978ULL };
        uint64_t mirror[2] = { 0xAABBCCDD11223344ULL, 0x5566778899AABBCCULL };
        const uint64_t pat0 = pat[0], pat1 = pat[1];
        const uint64_t mir0 = mirror[0], mir1 = mirror[1];
        DriverRwReq req;
        memset(&req, 0, sizeof(req));
        req.pid      = (uint32_t)getpid();
        req.addr     = (uint64_t)(uintptr_t)pat;
        req.user_buf = (uint64_t)(uintptr_t)mirror;
        req.size     = sizeof(pat);
        req.magic    = kDriverMagic;
        socklen_t optlen = sizeof(req);
        if (getsockopt(sock_, req.pid, cmd, (char*)&req, &optlen) != 0) {
            r.err = errno;
            return r;
        }
        // mirror получил содержимое pat, pat не тронут — команда ЧИТАЕТ.
        if (mirror[0] == pat0 && mirror[1] == pat1 &&
            pat[0] == pat0 && pat[1] == pat1) { r.kind = CMD_READ; return r; }
        // pat получил содержимое mirror — команда ПИШЕТ (user_buf -> addr).
        if (pat[0] == mir0 && pat[1] == mir1) { r.kind = CMD_WRITE; return r; }
        // Команда принята, но данные не двигались (чужая структура запроса
        // или команда-заглушка).
        return r;
    }

    // Выучить карту команд: перебираем известные коды этого класса драйверов
    // (все с 0x28-байтной структурой запроса; 0x302 берёт другую структуру —
    // не трогаем) и запоминаем, какой читает, какой пишет. Вызывать от root'а
    // ДО первого bind (экран режима / --check-driver): пробы идут по нашей
    // собственной памяти и ничего в игре не трогают.
    void learn_commands() {
        map_known_ = false;
        if (!ensure_socket()) return;
        const int cand[3] = { kDriverCmdRead, 0x301, kDriverCmdWrite };
        int readFound = 0, writeFound = 0;
        for (int i = 0; i < 3; ++i) {
            const CmdProbe p = classify_cmd(cand[i]);
            if (p.kind == CMD_NONE) continue;
            map_known_ = true;
            if (p.kind == CMD_READ  && !readFound)  readFound  = cand[i];
            if (p.kind == CMD_WRITE && !writeFound) writeFound = cand[i];
        }
        if (readFound)  read_cmd_  = readFound;
        if (writeFound) write_cmd_ = writeFound;
    }

    // Полный self-test: карта выучена, чтение читает, запись пишет.
    // false значит лишь «по собственной памяти не проверить» — режим при
    // найденном сокете всё равно включается и проверяется привязкой к игре.
    bool self_test() {
        learn_commands();
        if (!map_known_) return false;
        return classify_cmd(read_cmd_).kind == CMD_READ &&
               classify_cmd(write_cmd_).kind == CMD_WRITE;
    }

private:
    // Общий движок: собрать запрос и позвать getsockopt.
    ssize_t rw(int cmd, uint64_t addr, void* buf, size_t len) {
        const int fd  = sock_;
        const int pid = pid_;
        if (fd < 0 || pid <= 0 || !buf || !len) { errno = EBADF; return -1; }
        DriverRwReq req;
        memset(&req, 0, sizeof(req));
        req.pid      = (uint32_t)pid;
        req.addr     = addr & kAddressTagMask;   // снять TBI/MTE-метку
        req.user_buf = (uint64_t)(uintptr_t)buf;
        req.size     = (uint64_t)len;
        req.magic    = kDriverMagic;
        // level обязан быть != SOL_SOCKET, иначе getsockopt уйдёт в generic
        // обработчик сокета, а не в драйвер. Сам модуль level не проверяет —
        // передаём pid (так же делает публичный драйвер этого класса).
        socklen_t optlen = sizeof(req);
        const int rc = getsockopt(fd, (int)pid, cmd, (char*)&req, &optlen);
        if (rc != 0) return -1;
        return (ssize_t)len;
    }

    bool ensure_socket() {
        if (sock_ >= 0) return true;
        if (family_ < 0) family_ = driver_find_family();
        if (family_ < 0) { errno = EAFNOSUPPORT; return false; }
        errno = 0;
        const int fd = socket(family_, kDriverSockType, kDriverSockProto);
        if (fd < 0) return false;
        sock_ = fd;
        return true;
    }

    int sock_   = -1;   // рабочий сокет драйвера
    int family_ = -1;   // найденное семейство
    int pid_    = -1;   // целевой процесс
    // Карта команд (выучивается live-пробами по собственной памяти; дефолты —
    // из реверса диспетчера драйвера).
    int  read_cmd_  = kDriverCmdRead;
    int  write_cmd_ = kDriverCmdWrite;
    bool map_known_ = false;
};

// Единый экземпляр транспорта: читает поток отрисовки, дёргают поток
// привязки (open) и фоновые писатели. Состояние — два int, сокет
// сериализуется ядром, запрос живёт на стеке вызывающего.
inline KernelTransport& kernel_transport() {
    static KernelTransport t;
    return t;
}

// Быстрая проверка «драйвер загружен?» для меню: находит семейство и
// поднимает рабочий сокет, но не привязывается к процессу. Заодно (дёшево,
// три getsockopt) выучивает карту команд — какой код читает, какой пишет.
inline bool driver_available(int* family_out = nullptr) {
    KernelTransport& t = kernel_transport();
    if (!t.connected() && t.connect() < 0) return false;
    if (family_out) *family_out = t.family();
    t.learn_commands();
    return true;
}

// Полная проверка драйвера: не только «сокет открывается», но и «данные
// реально ходят» (чтение + запись собственной памяти через драйвер).
// false не значит «кернел выключен»: если драйвер найден, режим включается
// и по-настоящему проверяется привязкой к игре (bind читает ELF-заголовок
// libil2cpp через драйвер, /proc не задействован).
inline bool driver_self_test() {
    return kernel_transport().self_test();
}

}  // namespace memio
