// kernel_io.h — доступ к памяти игры через ядерный драйвер (кернел режим).
//
// Это ОТДЕЛЬНАЯ реализация доступа к памяти: /proc/<pid>/mem не открывается
// вообще — чтение и запись идут через сокет семейства протоколов, которое
// регистрирует драйвер.
//
// ДРАЙВЕРЫ ЭТОГО КЛАССА БЫВАЮТ РАЗНЫЕ, и протокол приходится определять на
// месте (устройство пользователя: маячок 1338 и сокет типа 4 поднялись, а
// getsockopt на код 0x300 ответил ENOPROTOOPT/92 — «такой опции нет», то
// есть сборка драйвера под это ядро просто использует другие коды команд).
// Известны два протокола:
//
//   1) bing_rw — реверс payload из drivers.7z (vermagic 6.6.56):
//        маячок: socket(fam, 6, 0) -> errno 1338;
//        рабочий сокет: socket(fam, 4, 0), root-only;
//        команды: getsockopt(fd, level=pid, optname, &req, 0x28), чтение
//        0x300 / запись 0x303, запрос {pid, pad, addr, buf, size, magic};
//        ядро само находит mm (find_get_pid -> get_task_mm -> page walk).
//
//   1б) bing_rw под 5.10 (payload 5.10.sh, module name=bing_rw, автор
//      NULL1337/t.me/NULLkawaii, vermagic 5.10.223-android12, Goron) —
//      протокол подтверждён полным реверсом-эмуляцией обработчика:
//        getsockopt И setsockopt — заглушки, ЛЮБОЙ код -> ENOPROTOOPT/92,
//        команды идут ЧЕРЕЗ ioctl(fd, код, &req);
//        сокет: socket(fam, 4, 0) (маячок 1338 на типе 6 — тот же);
//        АВТОРИЗАЦИЯ: ioctl(fd, 0x300, &auth, 0x31) — буфер 49 байт:
//          пароль "C61StIeKasRIz89g3c6xLzIc9L0LaRbI\0" (33 байта) + нули.
//          Драйвер сверяет первые 0x21 байта с расшифрованным Goron-ключом
//          (побайтово KEY[i]^state_i, 33 итерации) и ставит ГЛОБАЛЬНЫЙ флаг
//          в .data+0x290 (живёт до выгрузки драйвера). Возврат ioctl может
//          быть -1 даже при успешном входе — флаг важнее кода возврата.
//          Без авторизации рабочие команды молча возвращают -EPERM.
//        ЧТЕНИЕ: ioctl(fd, 0x301, &req) — req 0x28 байт
//          {u32 pid; u32 pad; u64 addr; u64 size; u64 local_buf; u32 magic
//           =0x1337; u32 pad2} — magic сверяется ДО работы (movz w9,#0x1337
//           в обработчике +0x21fc), данные: size байт из addr процесса pid
//           -> local_buf (find_get_pid->get_pid_task->get_task_mm->свой
//           PTE-walk pgd/pmd/pte по 3 уровням -> copy_to_user).
//        ЗАПИСЬ: ioctl(fd, 0x302, &req) — та же структура 0x28 (magic
//          обработчиком не сверяется, но заполняем), local_buf -> addr.
//        0x303 — сырая запись: копирует 0x31 байт по адресу из req+0x08
//          (структура другая, для чита не используется).
//
//   2) gongchuang — публичный референс (github:
//      gongchuang1089/android_kerneldriver_gongchuang, ядра 5.1-6.12):
//        маячок: socket(fam, 6, 0) -> ENOKEY/126 (чужой тип сокета);
//        рабочий сокет: socket(fam, SOCK_RAW=3, 0x090419), root-only;
//        команды 0..6 через getsockopt: ACCESS_PROCESS_VM=4 с запросом
//        {from, from_addr, to, to_addr, size} (копия между процессами в
//        любую сторону), PTE_PHYS_READ/WRITE_MEMORY=5/6 с запросом
//        {vaddr, buffer, size} после ATTACH_PROCESS=2 (level = pid).
//
// Протокол и коды команд ВЫУЧИВАЮТСЯ на месте (detect_protocol): короткий
// список известных кодов -> gongchuang-сокет -> полный скан 0..0x7FF. Все
// пробы идут по СОБСТВЕННОЙ памяти процесса (pid-поля указывают на себя),
// /proc не участвует: данные могут сойтись, только если ядро реально сходило
// по страницам нашего процесса. Диагностика всего этого — xvcen --scan-driver.
//
// Отличие от экстернала: ни fd /proc/pid/mem, ни прав ptrace — SELinux/yama
// на /proc не задействованы.

#pragma once

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mem_io.h"

namespace memio {

// ---- класс 1: bing_rw (реверс payload из drivers.7z) ----------------------
constexpr int kDriverFamilyFirst = 12;
constexpr int kDriverFamilyLast  = 43;
constexpr int kDriverProbeType   = 6;
constexpr int kDriverProbeErrno  = 1338;
constexpr int kDriverSockType    = 4;
constexpr int kDriverSockProto   = 0;
constexpr int kDriverCmdAuth     = 0x300;   // вход: пароль 0x31 байт
constexpr int kDriverCmdRead     = 0x301;   // чтение  {pid,addr,size,buf,magic}
constexpr int kDriverCmdWrite    = 0x302;   // запись  {pid,addr,size,buf,magic}
constexpr int kDriverAuthReqSize = 0x31;    // размер буфера авторизации
constexpr uint32_t kDriverMagic  = 0x1337;

// ---- класс 2: gongchuang (публичный референс) -----------------------------
constexpr int kGongSockType    = 3;        // SOCK_RAW
constexpr int kGongSockProto   = 0x090419; // магия протокола в create()
constexpr int kGongProbeErrno  = 126;      // ENOKEY: чужой тип/протокол сокета
constexpr int kGongCmdAttach   = 2;        // ATTACH_PROCESS: level = pid
constexpr int kGongCmdAccessVm = 4;        // ACCESS_PROCESS_VM
constexpr int kGongCmdPteRead  = 5;        // PTE_PHYS_READ_MEMORY
constexpr int kGongCmdPteWrite = 6;        // PTE_PHYS_WRITE_MEMORY

// Запрос bing_rw (0x28 байт; раскладка — по дизассемблу обработчиков).
#pragma pack(push, 1)
struct DriverRwReq {
    uint32_t pid;          // +0x00: целевой процесс
    uint32_t pad;          // +0x04: выравнивание
    uint64_t addr;         // +0x08: адрес в памяти игры (без TBI-метки)
    uint64_t size;         // +0x10: сколько байт (ПОДТВЕРЖДЕНО реверсом 5.10)
    uint64_t user_buf;     // +0x18: наш буфер (источник/приёмник данных)
    uint32_t magic;        // +0x20: kDriverMagic (сверяется в чтении 0x301)
    uint32_t pad2;         // +0x24
};
#pragma pack(pop)
static_assert(sizeof(DriverRwReq) == 0x28, "размер запроса bing_rw изменился");

// Запрос gongchuang ACCESS_PROCESS_VM (естественная раскладка, как в ядре:
// pid_t + паддинг + указатель; копирует from/from_addr -> to/to_addr).
struct GongVmReq {
    uint32_t from;         // +0x00: откуда (pid)
    uint32_t pad;          // +0x04
    uint64_t from_addr;    // +0x08: адрес-источник
    uint32_t to;           // +0x10: куда (pid)
    uint32_t pad2;         // +0x14
    uint64_t to_addr;      // +0x18: адрес-приёмник
    uint64_t size;         // +0x20
};
static_assert(sizeof(GongVmReq) == 0x28, "размер запроса gong_vm изменился");

// Запрос gongchuang PTE_*_MEMORY (после ATTACH_PROCESS: цель — pid сокета).
struct PteRwReq {
    uint64_t vaddr;        // +0x00: адрес в памяти цели
    uint64_t buffer;       // +0x08: наш буфер
    uint64_t size;         // +0x10
};
static_assert(sizeof(PteRwReq) == 0x18, "размер запроса pte изменился");

// ---- живая проба команды (по собственной памяти) --------------------------
//
// Два буфера с разными паттернами, одна команда — и смотрим, кто куда
// скопировался. /proc не участвует: данные сойдутся, только если ядро
// реально сходило по страницам нашего процесса.
enum CmdKind {
    CMD_NONE  = 0,   // команда не выполнилась / данные не двигались
    CMD_READ  = 1,   // память цели -> наш буфер
    CMD_WRITE = 2,   // наш буфер -> память цели
};

struct CmdProbe {
    CmdKind kind = CMD_NONE;
    int     err  = 0;   // errno, если getsockopt отказал
};

// ---- поиск драйвера -------------------------------------------------------
//
// Перебираем семейства и ищем маячок на пробе socket(dom, 6, 0):
// 1338 — bing-класс, 126 (ENOKEY) — gongchuang-класс. Обычные семейства на
// таком типе дают EAFNOSUPPORT/EPROTONOSUPPORT. Без root create() драйвера
// отвечает EAFNOSUPPORT и маячок не виден.
inline int driver_find_family(int* beacon_out = nullptr) {
    const int beacons[2] = { kDriverProbeErrno, kGongProbeErrno };
    for (int b = 0; b < 2; ++b) {
        for (int dom = kDriverFamilyFirst; dom <= kDriverFamilyLast; ++dom) {
            errno = 0;
            const int probe = socket(dom, kDriverProbeType, kDriverSockProto);
            if (probe >= 0) { close(probe); continue; }
            if (errno == beacons[b]) {
                if (beacon_out) *beacon_out = beacons[b];
                return dom;
            }
        }
    }
    return -1;
}

// ---- транспорт к памяти игры через драйвер --------------------------------

enum DriverProto {
    PROTO_BING     = 0,   // 0x28-запрос {pid,addr,buf,size,magic}; pid в запросе
    PROTO_GONG_VM  = 1,   // op4 ACCESS_PROCESS_VM: pids в запросе, направление задаём мы
    PROTO_GONG_PTE = 2,   // attach(op2) + op5/op6 {vaddr,buffer,size}
};

class KernelTransport : public Transport {
public:
    const char* name() const override { return "kernel (драйвер)"; }

    // Привязка к процессу: поднимаем сокеты, протокол и коды команд уже
    // должны быть выучены (detect_protocol в driver_available); для
    // pte-протокола цепляемся к pid сокетом (ATTACH_PROCESS).
    bool open(pid_t pid) override {
        if (pid <= 0) return false;
        if (!ensure_socket()) return false;
        if (!map_known_ && !scanned_) detect_protocol();
        if (use_ioctl_) send_auth();   // 5.10: без пароля команды молчат
        if (proto_ == PROTO_GONG_PTE && !gong_attach((int)pid)) return false;
        pid_ = (int)pid;
        return true;
    }

    // Сокеты драйвера живут, сколько живёт процесс чита: закрывать их на
    // отвязке не нужно (переподключение нужно, только если драйвер выгрузили).
    void close() override { pid_ = -1; }

    // Сокеты потеряны (драйвер выгрузили/перезагрузили) — перевзять при
    // следующем open(). Карта команд тоже сбрасывается: перепрошитый драйвер
    // может говорить другим протоколом.
    void invalidate() {
        if (sock_ >= 0) { ::close(sock_); sock_ = -1; }
        if (gong_sock_ >= 0) { ::close(gong_sock_); gong_sock_ = -1; }
        family_ = -1;
        proto_ = PROTO_BING;
        read_cmd_ = kDriverCmdRead;
        write_cmd_ = kDriverCmdWrite;
        map_known_ = false;
        scanned_ = false;
        auth_done_ = false;
        attached_pid_ = -1;
    }

    ssize_t read_once(uint64_t addr, void* out, size_t len) override {
        switch (proto_) {
            case PROTO_GONG_VM:  return gong_vm_rw(true, addr, out, len);
            case PROTO_GONG_PTE: return gong_pte_rw(true, addr, out, len);
            default:             return rw(read_cmd_, addr, out, len);
        }
    }

    ssize_t write_once(uint64_t addr, const void* in, size_t len) override {
        switch (proto_) {
            case PROTO_GONG_VM:  return gong_vm_rw(false, addr, const_cast<void*>(in), len);
            case PROTO_GONG_PTE: return gong_pte_rw(false, addr, const_cast<void*>(in), len);
            default:             return rw(write_cmd_, addr, const_cast<void*>(in), len);
        }
    }

    int  family() const { return family_; }
    int  socket_fd() const { return sock_; }
    bool connected() const { return sock_ >= 0 || gong_sock_ >= 0; }

    // Поднять рабочий сокет bing-класса (если ещё не поднят).
    int connect() {
        if (!ensure_socket()) return -1;
        return sock_;
    }

    // Выученное: протокол и коды команд (после detect_protocol).
    DriverProto proto() const { return proto_; }
    const char* proto_name() const {
        switch (proto_) {
            case PROTO_GONG_VM:  return "gongchuang (ACCESS_PROCESS_VM)";
            case PROTO_GONG_PTE: return "gongchuang (PTE)";
            default:             return "bing_rw";
        }
    }
    int  read_cmd()  const { return read_cmd_; }
    int  write_cmd() const { return write_cmd_; }
    // Транспорт bing-команд: сборка 6.6 — getsockopt, сборка 5.10 — ioctl.
    bool cmd_via_ioctl() const { return use_ioctl_; }
    // Подтверждена ли карта живыми пробами. false — коды дефолтные и не
    // подтверждены: транспорт всё равно пробует привязкой к игре, а
    // отказ виден с errno (92/ENOPROTOOPT = код не распознан).
    bool map_known() const { return map_known_; }

    // ---- пробы (публичные: ими же печатает карту --scan-driver) ----------

    // АВТОРИЗАЦИЯ (сборка 5.10): ioctl(fd, 0x300, пароль 0x31 байт).
    // Драйвер ставит глобальный флаг и после этого пускает рабочие команды;
    // сам вызов может вернуть -1 даже при успехе, поэтому код возврата
    // игнорируем. Повторный вызов безвреден (флаг уже стоит — запрос
    // просто не проверяется). Успех подтверждается пробой чтения 0x301.
    bool send_auth() {
        if (auth_done_) return true;
        if (!ensure_socket()) return false;
        static const char kPassword[] = "C61StIeKasRIz89g3c6xLzIc9L0LaRbI";
        unsigned char authbuf[kDriverAuthReqSize];
        memset(authbuf, 0, sizeof(authbuf));
        memcpy(authbuf, kPassword, sizeof(kPassword) - 1);   // с '\0'
        ioctl(sock_, (unsigned long)kDriverCmdAuth, authbuf);
        auth_done_ = true;   // retval не показатель: флаг внутри драйвера
        return true;
    }
    bool auth_sent() const { return auth_done_; }

    // Проба bing-кода: одна команда, addr=pat, buf=mirror. Чтение скопирует
    // pat -> mirror; запись — mirror -> pat (запрос симметричный).
    CmdProbe classify_cmd(int cmd) {
        CmdProbe r;
        if (!ensure_socket()) { r.err = errno ? errno : EIO; return r; }
        if (use_ioctl_) send_auth();   // без флага 5.10-сборка молчит (-EPERM)
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
        int rc = getsockopt(sock_, req.pid, cmd, (char*)&req, &optlen);
        if (rc != 0) {
            const int gs_err = errno;
            // Сборка bing_rw под 5.10: getsockopt — заглушка (всегда
            // ENOPROTOOPT), те же команды идут через ioctl(fd, код, &req).
            if (gs_err == ENOPROTOOPT || gs_err == EOPNOTSUPP) {
                // 5.10-сборка: сначала авторизация паролем, затем повтор
                // команды через ioctl — без флага драйвер вернёт EPERM.
                use_ioctl_ = true;
                send_auth();
                errno = 0;
                rc = ioctl(sock_, (unsigned long)cmd, &req);
            }
            if (rc != 0) { r.err = gs_err; return r; }
        }
        if (mirror[0] == pat0 && mirror[1] == pat1 &&
            pat[0] == pat0 && pat[1] == pat1) { r.kind = CMD_READ; return r; }
        if (pat[0] == mir0 && pat[1] == mir1) { r.kind = CMD_WRITE; return r; }
        return r;   // принята, но данные не двигались
    }

    // Проба gongchuang op4 (ACCESS_PROCESS_VM): копия from -> to, направление
    // задаём мы сами, поэтому «наш» read и write — одна и та же команда.
    CmdProbe classify_gong_vm(int fd, int cmd) {
        CmdProbe r;
        uint64_t pat[2]    = { 0x1234567890ABCDEFULL, 0x0F1E2D3C4B5A6978ULL };
        uint64_t mirror[2] = { 0xAABBCCDD11223344ULL, 0x5566778899AABBCCULL };
        const uint64_t pat0 = pat[0], pat1 = pat[1];
        GongVmReq req;
        memset(&req, 0, sizeof(req));
        req.from      = (uint32_t)getpid();
        req.from_addr = (uint64_t)(uintptr_t)pat;
        req.to        = (uint32_t)getpid();
        req.to_addr   = (uint64_t)(uintptr_t)mirror;
        req.size      = sizeof(pat);
        socklen_t optlen = sizeof(req);
        if (getsockopt(fd, (int)req.from, cmd, (char*)&req, &optlen) != 0) {
            r.err = errno;
            return r;
        }
        if (mirror[0] == pat0 && mirror[1] == pat1) { r.kind = CMD_READ; return r; }
        return r;
    }

    // Проба gongchuang pte-команды (op5 читает vaddr -> buffer, op6 пишет
    // buffer -> vaddr; цель — ATTACHнутый pid, для пробы цепляемся к себе).
    CmdProbe classify_pte(int fd, int cmd) {
        CmdProbe r;
        // чтение: vaddr=pat, buffer=mirror
        {
            uint64_t pat[2]    = { 0x1234567890ABCDEFULL, 0x0F1E2D3C4B5A6978ULL };
            uint64_t mirror[2] = { 0xAABBCCDD11223344ULL, 0x5566778899AABBCCULL };
            const uint64_t pat0 = pat[0], pat1 = pat[1];
            PteRwReq req;
            memset(&req, 0, sizeof(req));
            req.vaddr  = (uint64_t)(uintptr_t)pat;
            req.buffer = (uint64_t)(uintptr_t)mirror;
            req.size   = sizeof(pat);
            socklen_t optlen = sizeof(req);
            if (getsockopt(fd, (int)getpid(), cmd, (char*)&req, &optlen) == 0 &&
                mirror[0] == pat0 && mirror[1] == pat1) { r.kind = CMD_READ; return r; }
            if (r.kind == CMD_NONE && errno != ENOPROTOOPT && errno != EOPNOTSUPP)
                r.err = errno;
        }
        // запись: vaddr=scratch, buffer=wsrc
        {
            uint64_t scratch[2] = { 1, 1 };
            uint64_t wsrc[2]    = { 0xA5A55A5ADEADBEEFULL, 0x0BADC0DE0DDC0FFEULL };
            PteRwReq req;
            memset(&req, 0, sizeof(req));
            req.vaddr  = (uint64_t)(uintptr_t)scratch;
            req.buffer = (uint64_t)(uintptr_t)wsrc;
            req.size   = sizeof(scratch);
            socklen_t optlen = sizeof(req);
            if (getsockopt(fd, (int)getpid(), cmd, (char*)&req, &optlen) == 0 &&
                scratch[0] == wsrc[0] && scratch[1] == wsrc[1]) {
                r.kind = CMD_WRITE;
                r.err = 0;
                return r;
            }
        }
        return r;
    }

    // ATTACH_PROCESS (op2): привязать сокет к pid — цель pte-команд.
    bool gong_attach(int pid) {
        if (pid <= 0) return false;
        if (!gong_ensure_socket()) return false;
        if (attached_pid_ == pid) return true;
        char dummy[8] = {};
        socklen_t optlen = sizeof(dummy);
        if (getsockopt(gong_sock_, pid, kGongCmdAttach, dummy, &optlen) != 0)
            return false;
        attached_pid_ = pid;
        return true;
    }

    // Поднять gongchuang-сокет (socket(fam, SOCK_RAW, 0x090419)). Не каждый
    // драйвер класса его даёт — bing-сборки отвечают своим маячком.
    int gong_connect() {
        if (!gong_ensure_socket()) return -1;
        return gong_sock_;
    }
    int gong_socket_fd() const { return gong_sock_; }

    // ---- выучить протокол и коды команд ----------------------------------
    //
    // 1) известные коды bing (0x300/0x301/0x303);
    // 2) gongchuang-сокет: op4 (ACCESS_PROCESS_VM), затем op5/op6 (PTE);
    // 3) полный скан 0..0x7FF bing-структурой — сборки под другие ядра
    //    используют другие коды (маячок и сокет те же).
    // Пробы идут по собственной памяти, pid-поля указывают на себя.
    void detect_protocol() {
        map_known_ = false;
        proto_ = PROTO_BING;
        read_cmd_ = kDriverCmdRead;
        write_cmd_ = kDriverCmdWrite;
        if (family_ < 0) family_ = driver_find_family();
        if (family_ < 0) return;

        // 1) известные коды bing (нужен bing-сокет — не у всех сборок он есть):
        //    5.10 — чтение 0x301 / запись 0x302; 6.6 — 0x300/0x303 (та же
        //    структура, транспорт getsockopt; коды пробуем и там и там).
        const int bingCand[4] = { kDriverCmdRead, kDriverCmdWrite, 0x300, 0x303 };
        int rd = 0, wr = 0;
        if (ensure_socket()) {
            for (int i = 0; i < 3; ++i) {
                const CmdProbe p = classify_cmd(bingCand[i]);
                if (p.kind == CMD_READ  && !rd) rd = bingCand[i];
                if (p.kind == CMD_WRITE && !wr) wr = bingCand[i];
            }
        }
        if (rd && wr) {
            read_cmd_ = rd; write_cmd_ = wr;
            map_known_ = true;
            return;
        }

        // 2) gongchuang: ACCESS_PROCESS_VM
        if (gong_ensure_socket()) {
            if (classify_gong_vm(gong_sock_, kGongCmdAccessVm).kind == CMD_READ) {
                proto_ = PROTO_GONG_VM;
                map_known_ = true;
                return;
            }
            // pte: attach к себе + op5/op6
            if (gong_attach((int)getpid())) {
                if (classify_pte(gong_sock_, kGongCmdPteRead).kind == CMD_READ &&
                    classify_pte(gong_sock_, kGongCmdPteWrite).kind == CMD_WRITE) {
                    proto_ = PROTO_GONG_PTE;
                    map_known_ = true;
                    attached_pid_ = -1;   // к игре цепимся заново в open()
                    return;
                }
                attached_pid_ = -1;
            }
        }

        // 3) полный скан: bing-класс с другими кодами (наш случай на 5.10.240)
        scanned_ = true;
        if (sock_ >= 0) {
            for (int c = 0; c <= 0x7FF && !(rd && wr); ++c) {
                if (c == kDriverCmdAuth || c == 0x303 ||
                    (c >= kDriverCmdRead && c <= kDriverCmdWrite))
                    continue;   // уже проверены на шаге 1
                const CmdProbe p = classify_cmd(c);
                if (p.kind == CMD_NONE) continue;
                map_known_ = true;
                if (p.kind == CMD_READ  && !rd) { rd = c; read_cmd_ = c; }
                if (p.kind == CMD_WRITE && !wr) { wr = c; write_cmd_ = c; }
            }
        }
        if (rd || wr) map_known_ = true;   // хоть что-то нашли — коды рабочие
    }

    bool scanned() const { return scanned_; }

    // Полный self-test (диагностика): карта выучена и подтверждена.
    bool self_test() {
        detect_protocol();
        if (!map_known_) return false;
        switch (proto_) {
            case PROTO_GONG_VM:
                return classify_gong_vm(gong_sock_, kGongCmdAccessVm).kind == CMD_READ;
            case PROTO_GONG_PTE:
                return classify_pte(gong_sock_, kGongCmdPteRead).kind == CMD_READ &&
                       classify_pte(gong_sock_, kGongCmdPteWrite).kind == CMD_WRITE;
            default:
                return classify_cmd(read_cmd_).kind == CMD_READ &&
                       classify_cmd(write_cmd_).kind == CMD_WRITE;
        }
    }

private:
    // bing: собрать 0x28-запрос и позвать getsockopt.
    ssize_t rw(int cmd, uint64_t addr, void* buf, size_t len) {
        const int fd  = sock_;
        const int pid = pid_;
        if (fd < 0 || pid <= 0 || !buf || !len) { errno = EBADF; return -1; }
        DriverRwReq req;
        memset(&req, 0, sizeof(req));
        req.pid      = (uint32_t)pid;
        req.addr     = addr & kAddressTagMask;   // снять TBI/MTE-метку
        req.size     = (uint64_t)len;            // +0x10 (подтверждено 5.10)
        req.user_buf = (uint64_t)(uintptr_t)buf; // +0x18
        req.magic    = kDriverMagic;             // +0x20 (нужно чтению 0x301)
        if (use_ioctl_) {
            // Сборка 5.10: команды через ioctl(fd, код, &req) — getsockopt
            // там заглушка. Авторизация уже отправлена (send_auth при
            // переключении транспорта/пробе); повторяем при необходимости.
            if (!auth_done_) send_auth();
            if (ioctl(fd, (unsigned long)cmd, &req) != 0) return -1;
            return (ssize_t)len;
        }
        // level обязан быть != SOL_SOCKET, иначе getsockopt уйдёт в generic
        // обработчик сокета (он-то и отвечает ENOPROTOOPT на чужой optname).
        socklen_t optlen = sizeof(req);
        const int rc = getsockopt(fd, (int)pid, cmd, (char*)&req, &optlen);
        if (rc != 0) return -1;
        return (ssize_t)len;
    }

    // gongchuang op4: копия from -> to; направление задаём раскладкой полей.
    ssize_t gong_vm_rw(bool is_read, uint64_t addr, void* buf, size_t len) {
        const int fd = gong_sock_ >= 0 ? gong_sock_ : sock_;
        const int pid = pid_;
        const int self = (int)getpid();
        if (fd < 0 || pid <= 0 || !buf || !len) { errno = EBADF; return -1; }
        GongVmReq req;
        memset(&req, 0, sizeof(req));
        if (is_read) {
            req.from = (uint32_t)pid;   req.from_addr = addr & kAddressTagMask;
            req.to   = (uint32_t)self;  req.to_addr   = (uint64_t)(uintptr_t)buf;
        } else {
            req.from = (uint32_t)self;  req.from_addr = (uint64_t)(uintptr_t)buf;
            req.to   = (uint32_t)pid;   req.to_addr   = addr & kAddressTagMask;
        }
        req.size = (uint64_t)len;
        socklen_t optlen = sizeof(req);
        if (getsockopt(fd, pid, kGongCmdAccessVm, (char*)&req, &optlen) != 0)
            return -1;
        return (ssize_t)len;
    }

    // gongchuang pte: op5 читает vaddr -> buffer, op6 пишет buffer -> vaddr;
    // цель — pid, к которому сокет прицеплен (gong_attach).
    ssize_t gong_pte_rw(bool is_read, uint64_t addr, void* buf, size_t len) {
        const int fd = gong_sock_ >= 0 ? gong_sock_ : sock_;
        const int pid = pid_;
        if (fd < 0 || pid <= 0 || !buf || !len) { errno = EBADF; return -1; }
        if (attached_pid_ != pid && !gong_attach(pid)) return -1;
        PteRwReq req;
        memset(&req, 0, sizeof(req));
        req.vaddr  = addr & kAddressTagMask;
        req.buffer = (uint64_t)(uintptr_t)buf;
        req.size   = (uint64_t)len;
        socklen_t optlen = sizeof(req);
        const int cmd = is_read ? kGongCmdPteRead : kGongCmdPteWrite;
        if (getsockopt(fd, pid, cmd, (char*)&req, &optlen) != 0) return -1;
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

    bool gong_ensure_socket() {
        if (gong_sock_ >= 0) return true;
        if (family_ < 0) family_ = driver_find_family();
        if (family_ < 0) { errno = EAFNOSUPPORT; return false; }
        errno = 0;
        const int fd = socket(family_, kGongSockType, kGongSockProto);
        if (fd < 0) return false;
        gong_sock_ = fd;
        return true;
    }

    int sock_   = -1;   // рабочий сокет bing-класса (тип 4)
    int gong_sock_ = -1;// сокет gongchuang-класса (SOCK_RAW, 0x090419)
    int family_ = -1;   // найденное семейство
    int pid_    = -1;   // целевой процесс
    // Карта команд/протокол (выучиваются пробами по собственной памяти).
    DriverProto proto_ = PROTO_BING;
    int  read_cmd_  = kDriverCmdRead;
    int  write_cmd_ = kDriverCmdWrite;
    bool use_ioctl_ = false;  // bing-команды через ioctl (сборка 5.10)
    bool auth_done_ = false;  // пароль 0x300 отправлен (флаг в драйвере)
    bool map_known_ = false;
    bool scanned_   = false;   // полный скан 0..0x7FF уже делали
    int  attached_pid_ = -1;   // pid, к которому прицеплен gong-сокет (op2)
};

// ---- класс 3: RT-Dev — /dev/<случайное имя>, ioctl 0x801/0x802 ---------
//
// Пакет «rt dev Драйвера root cheat.zip» содержит RTkernel для ядер
// 4.9–6.6. Статический разбор всех 23 модулей подтверждает единый ABI:
// char-устройство с именем, которое init_module генерирует сам, и команды
// 0x801/0x802 с COPY_MEMOBY (0x20):
//   u32 pid; pad; u64 addr; u64 buffer; u64 size.
// 0x800 — OP_INIT_KEY, 0x803 — OP_MODULE_BASE; ни один не нужен Reader:
// базы по-прежнему берём из /proc/<pid>/maps.
//
// read/write_process_memory транслируют одну страницу за запрос, поэтому
// транспорт режет операции по 4К. Драйвер возвращает 0 при успехе и -1 при
// ошибке.

constexpr int kRtDevCmdRead        = 0x801;
constexpr int kRtDevCmdWrite       = 0x802;
constexpr int kRtDevCmdModuleBase  = 0x803;

#pragma pack(push, 1)
struct RtDevCopyMem {
    uint32_t pid;      // +0x00: целевой процесс
    uint32_t pad;      // +0x04
    uint64_t addr;     // +0x08: адрес в памяти игры
    uint64_t buffer;   // +0x10: наш буфер
    uint64_t size;     // +0x18: сколько байт (не через границу 4К!)
};
#pragma pack(pop)
static_assert(sizeof(RtDevCopyMem) == 0x20, "размер запроса RT-Dev изменился");

class RtDevTransport : public Transport {
public:
    const char* name() const override { return "rt-dev (драйвер)"; }

    // Устройство открывается один раз и живёт, пока живёт процесс чита:
    // драйвер переживает переподключения к игре.
    bool open(pid_t pid) override {
        if (pid <= 0) return false;
        // До привязки кандидат обязан пройти R/W-пробу по собственной памяти.
        // Открытый, но чужой /dev-узел никогда не становится транспортом игры.
        if (!verified_ && connect() < 0) return false;
        pid_ = (int)pid;
        return true;
    }
    void close() override { pid_ = -1; }

    // Драйвер перезашит (имя устройства новое) — сбросить всё.
    void invalidate() {
        if (dev_fd_ >= 0) { ::close(dev_fd_); dev_fd_ = -1; }
        dev_path_.clear();
        pid_ = -1;
        verified_ = false;
    }

    ssize_t read_once(uint64_t addr, void* out, size_t len) override {
        return rw(true, addr, out, len);
    }
    ssize_t write_once(uint64_t addr, const void* in, size_t len) override {
        return rw(false, addr, const_cast<void*>(in), len);
    }

    int  device_fd() const { return dev_fd_; }
    bool connected() const { return dev_fd_ >= 0; }
    const char* device_path() const { return dev_path_.c_str(); }

    // Поднять устройство (найти + открыть + проверить пробой по своей
    // памяти). При коллизии эвристики проверяет КАЖДЫЙ подходящий /dev-узел,
    // а не останавливается на первом чужом char-device. Возвращает fd или -1.
    int connect() {
        if (dev_fd_ >= 0 && verified_) return dev_fd_;
        return classify().kind == CMD_READ ? dev_fd_ : -1;
    }
    // Устройство найдено и подтверждено пробой чтения.
    bool verified() const { return verified_; }

    // Живая проба по СОБСТВЕННОЙ памяти. Если первый кандидат — чужой
    // char-device с похожим случайным именем, он отбрасывается и поиск идёт
    // дальше по /dev. Это важно: один чужой узел не должен скрывать RT-Dev.
    CmdProbe classify() {
        if (dev_fd_ >= 0 && verified_) {
            CmdProbe r;
            r.kind = CMD_READ;
            return r;
        }
        return find_and_verify_device();
    }

private:
    // Найти /dev-узел RT-Dev. Имя генерируется модулем, поэтому используем
    // char-устройство, имя только из строчных букв, без '_','-',';', не
    // gpiochip/std*, uid=gid=0, size 0, atime==ctime, год создания не 1970,
    // длина имени <= 7. Это оставляет лишь случайное имя, которое создаёт модуль.
    static bool rt_dev_candidate(const char* name, const struct stat& st) {
        const size_t len = strlen(name);
        if (len == 0 || len > 7) return false;
        bool has_lower = false;
        for (size_t i = 0; i < len; ++i) {
            const char c = name[i];
            if (c >= 'a' && c <= 'z') { has_lower = true; continue; }
            return false;   // только строчные буквы
        }
        if (!has_lower) return false;
        if (strstr(name, "gpiochip")) return false;
        if (S_ISCHR(st.st_mode) == false && S_ISBLK(st.st_mode) == false) return false;
        if (strchr(name, '_') || strchr(name, '-') || strchr(name, ':')) return false;
        if (st.st_uid != 0 || st.st_gid != 0 || st.st_size != 0) return false;
        if (st.st_atime != st.st_ctime) return false;
        struct tm tmv;
        localtime_r(&st.st_ctime, &tmv);
        if (tmv.tm_year + 1900 == 1970) return false;
        return true;
    }

    // Проба уже открытого кандидата. Не закрывает fd: find_and_verify_device
    // решает, принять его или продолжить скан.
    CmdProbe classify_open_device() {
        CmdProbe r;
        if (dev_fd_ < 0) { r.err = EBADF; return r; }
        uint64_t pat[2]    = {0x1234567890ABCDEFULL, 0x0F1E2D3C4B5A6978ULL};
        uint64_t mirror[2] = {0xAABBCCDD11223344ULL, 0x5566778899AABBCCULL};
        const uint64_t pat0 = pat[0], pat1 = pat[1];
        const int save_pid = pid_;
        pid_ = (int)getpid();
        // errno обнуляем перед пробой: если чужой драйвер вдруг вернёт
        // нестандартное положительное значение, в диагностике не останется
        // errno от предыдущего кандидата.
        errno = 0;
        if (rw(true, (uint64_t)(uintptr_t)pat, mirror, sizeof(pat)) == sizeof(pat) &&
            mirror[0] == pat0 && mirror[1] == pat1) {
            uint64_t scratch[2] = {1, 1};
            uint64_t wsrc[2]    = {0xA5A55A5ADEADBEEFULL, 0x0BADC0DE0DDC0FFEULL};
            errno = 0;
            if (rw(false, (uint64_t)(uintptr_t)scratch, wsrc, sizeof(scratch)) == sizeof(scratch) &&
                scratch[0] == wsrc[0] && scratch[1] == wsrc[1]) {
                r.kind = CMD_READ;   // читает и пишет — транспорт полный
                verified_ = true;
                pid_ = save_pid;
                return r;
            }
        }
        pid_ = save_pid;
        r.err = errno ? errno : EPERM;
        verified_ = false;
        return r;
    }

    // Найти И подтвердить RT-Dev. Имена /dev генерируются случайно, поэтому
    // одного stat/open недостаточно: на прошивке могут быть другие root-owned
    // char-devices, которые проходят эвристику имени. Для каждого кандидата
    // разрешён только read-тест; write выполняется лишь после точного R/W read
    // совпадения паттерна собственной памяти.
    CmdProbe find_and_verify_device() {
        CmdProbe last;
        int last_err = ENOENT;
        if (dev_fd_ >= 0) {
            last = classify_open_device();
            if (last.kind == CMD_READ) return last;
            if (last.err) last_err = last.err;
            ::close(dev_fd_);
            dev_fd_ = -1;
            dev_path_.clear();
            pid_ = -1;
        }

        DIR* dir = opendir("/dev");
        if (!dir) {
            last.err = errno ? errno : ENOENT;
            return last;
        }
        struct dirent* e;
        while ((e = readdir(dir)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            char path[96];
            snprintf(path, sizeof(path), "/dev/%s", e->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            if (!rt_dev_candidate(e->d_name, st)) continue;
            const int fd = ::open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                if (errno) last_err = errno;
                continue;
            }
            dev_fd_ = fd;
            dev_path_ = path;
            verified_ = false;
            const CmdProbe probe = classify_open_device();
            if (probe.kind == CMD_READ) {
                closedir(dir);
                return probe;
            }
            if (probe.err) last_err = probe.err;
            ::close(dev_fd_);
            dev_fd_ = -1;
            dev_path_.clear();
            pid_ = -1;
        }
        closedir(dir);
        last.err = last_err;
        errno = last_err;
        return last;
    }

    // Запрос с разбиением по границам страниц (драйвер копирует size байт
    // с ОДНОЙ физической страницы — за границей читать нельзя).
    ssize_t rw(bool is_read, uint64_t addr, void* buf, size_t len) {
        if (dev_fd_ < 0 || pid_ <= 0 || !buf || !len) { errno = EBADF; return -1; }
        uint8_t* p = (uint8_t*)buf;
        uint64_t a = addr & kAddressTagMask;   // снять TBI/MTE-метку
        size_t done = 0;
        while (done < len) {
            const uint64_t pageLeft = 0x1000 - (a & 0xfff);
            const size_t chunk = (uint64_t)(len - done) < pageLeft
                                       ? (len - done) : (size_t)pageLeft;
            RtDevCopyMem q;
            memset(&q, 0, sizeof(q));
            q.pid    = (uint32_t)pid_;
            q.addr   = a;
            q.buffer = (uint64_t)(uintptr_t)p;
            q.size   = (uint64_t)chunk;
            const int cmd = is_read ? kRtDevCmdRead : kRtDevCmdWrite;
            if (ioctl(dev_fd_, (unsigned long)cmd, &q) != 0)
                return done ? (ssize_t)done : -1;
            a += chunk;
            p += chunk;
            done += chunk;
        }
        return (ssize_t)done;
    }

    int dev_fd_ = -1;
    std::string dev_path_;
    int pid_ = -1;
    bool verified_ = false;
};

// Единый экземпляр RT-Dev транспорта (по образцу kernel_transport).
inline RtDevTransport& rt_dev_transport() {
    static RtDevTransport t;
    return t;
}

// RT-Dev доступен? (устройство находится и отвечает пробой по своей
// памяти — для меню выбора и --check-driver).
inline bool rt_dev_available() {
    RtDevTransport& t = rt_dev_transport();
    if (t.connected()) return t.verified() ? true : t.classify().kind == CMD_READ;
    if (t.connect() < 0) return false;
    return t.classify().kind == CMD_READ;
}

// ---- класс 4: RT-hook (hookdd) — хук syscall ioctl, команды 0x259/0x25a ----
//
// Драйверы «RT-hook» (архив RT-hook.7z, канал @RTdrivers, автор waangchuan,
// module hookdd/gynhdd, исходники /root/yijian/gynhdd/: entry.c hook.c
// memory.h hw_breakpoint.h comm.h cvector.c). Не регистрирует НИ сокет, НИ
// /dev-узел: init_module патчит sys_call_table[__NR_ioctl=29] (install_hook),
// страницу таблицы сам делает writable (inline PTE-walk + бит DBM + tlbi),
// затем самоскрывается (list_del(&module.list) + kobject_del + rb_erase из
// vmap_area_root/list). Обращение — обычный syscall ioctl с ЛЮБЫМ fd (fd
// диспетчер НЕ проверяет вовсе); чужие cmd проходят в оригинальный ioctl
// без изменений, т.е. для остальных процессов всё выглядит как обычно.
//
//   0x259 чтение  {u32 pid; u32 pad; u64 addr; u64 buffer; u64 size} — 0x20;
//   0x25a запись  — та же структура;
//   0x25b база модуля {u32 pid; char* name; u16 ?; -> u64 base @+0x10}
//          (root-gated, нами не используется — базы из /proc/<pid>/maps);
//   0x25d показать процесс / 0x25f скрыть процесс (list_del_rcu + restore);
//   0x25e снять хук (sys_hook(29, original) + hw_bp exit) — «выгрузка»;
//   0x260/0x261 аппаратные breakpoints (perf_event: register_user_hw_
//          breakpoint + счётчик попаданий) — читу не нужны;
//   0x25c и всё вне 0x259..0x261 — проход в оригинальный ioctl.
//
// 0x259/0x25b гейтятся root'ом (uid==0 || euid==0 || capable(CAP_SYS_ADMIN),
// отказ — printk «noroot» и -1); 0x25a записи гейта НЕ имеют. Структура
// read/write_process_memory: find_get_pid -> get_pid_task -> get_task_mm ->
// свой PTE-walk по 4 уровням -> vmap физической страницы + sync кэша; САМИ
// чанкуют по 4К в цикле, статус true(1). hook_ioctl
// возвращает 0 (успех) / -1 (EPERM). Проба наличия без побочных эффектов:
// ioctl(-1, 0x259, &req) — без драйвера ядро отвечает EBADF, с драйвером
// возвращается 0: fd невалиден, но диспетчеру всё равно.
// След на устройстве: в fd-листе чита НЕТ НИЧЕГО (ни сокета, ни /dev) —
// самый скрытный из классов; хук виден только из ядра (sys_call_table).

constexpr int kRtHookCmdRead  = 0x259;   // чтение {pid,addr,buffer,size}
constexpr int kRtHookCmdWrite = 0x25a;   // запись — та же структура

#pragma pack(push, 1)
struct RtHookCopyMem {
    uint32_t pid;      // +0x00: целевой процесс
    uint32_t pad;      // +0x04
    uint64_t addr;     // +0x08: адрес в памяти игры
    uint64_t buffer;   // +0x10: наш буфер
    uint64_t size;     // +0x18: сколько байт
};
#pragma pack(pop)
static_assert(sizeof(RtHookCopyMem) == 0x20, "размер запроса RT-Hook изменился");

class RtHookTransport : public Transport {
public:
    const char* name() const override { return "rt-hook (драйвер)"; }

    // «Открытия» нет: fd не нужен вообще (диспетчер его игнорирует).
    // Только подтверждаем, что хук стоит, и запоминаем цель.
    bool open(pid_t pid) override {
        if (pid <= 0) return false;
        if (!ensure_driver()) return false;
        pid_ = (int)pid;
        return true;
    }
    void close() override { pid_ = -1; }

    // Драйвер перезашит (или снят хуком 0x25e) — сбросить подтверждение:
    // следующая проба ioctl(-1, 0x259) покажет, стоит ли хук снова.
    void invalidate() {
        pid_ = -1;
        verified_ = false;
    }

    ssize_t read_once(uint64_t addr, void* out, size_t len) override {
        return rw(true, addr, out, len);
    }
    ssize_t write_once(uint64_t addr, const void* in, size_t len) override {
        return rw(false, addr, const_cast<void*>(in), len);
    }

    bool connected() const { return verified_; }
    bool verified()  const { return verified_; }

    // Живая проба по СОБСТВЕННОЙ памяти (по образцу проверки FT): читаем паттерн
    // и пишем в scratch через ioctl(-1, 0x259/0x25a). Подтверждает, что
    // syscall ioctl действительно перехвачен и данные двигает драйвер.
    CmdProbe classify() {
        CmdProbe r;
        uint64_t pat[2]    = {0x1234567890ABCDEFULL, 0x0F1E2D3C4B5A6978ULL};
        uint64_t mirror[2] = {0xAABBCCDD11223344ULL, 0x5566778899AABBCCULL};
        const uint64_t pat0 = pat[0], pat1 = pat[1];
        const int save_pid = pid_;
        pid_ = (int)getpid();
        if (rw(true, (uint64_t)(uintptr_t)pat, mirror, sizeof(pat)) == sizeof(pat) &&
            mirror[0] == pat0 && mirror[1] == pat1) {
            uint64_t scratch[2] = {1, 1};
            uint64_t wsrc[2]    = {0xA5A55A5ADEADBEEFULL, 0x0BADC0DE0DDC0FFEULL};
            if (rw(false, (uint64_t)(uintptr_t)scratch, wsrc, sizeof(scratch)) == sizeof(scratch) &&
                scratch[0] == wsrc[0] && scratch[1] == wsrc[1]) {
                r.kind = CMD_READ;   // читает и пишет — транспорт полный
                verified_ = true;
                pid_ = save_pid;
                return r;
            }
        }
        pid_ = save_pid;
        r.err = errno ? errno : EPERM;
        verified_ = false;
        return r;
    }

private:
    // Хук стоит? Одна проба чтения своей памяти через ioctl(-1, 0x259).
    bool ensure_driver() {
        if (verified_) return true;
        return classify().kind == CMD_READ;
    }

    // Запрос. Драйвер 5.10 сам режет по страницам и копирует в цикле, но
    // чанкинг 4К оставлен: страховка от сборок, где трансляция одна на
    // запрос, — это безопасно и для старых сборок.
    ssize_t rw(bool is_read, uint64_t addr, void* buf, size_t len) {
        if (pid_ <= 0 || !buf || !len) { errno = EBADF; return -1; }
        uint8_t* p = (uint8_t*)buf;
        uint64_t a = addr & kAddressTagMask;   // снять TBI/MTE-метку
        size_t done = 0;
        while (done < len) {
            const uint64_t pageLeft = 0x1000 - (a & 0xfff);
            const size_t chunk = (uint64_t)(len - done) < pageLeft
                                       ? (len - done) : (size_t)pageLeft;
            RtHookCopyMem q;
            memset(&q, 0, sizeof(q));
            q.pid    = (uint32_t)pid_;
            q.addr   = a;
            q.buffer = (uint64_t)(uintptr_t)p;
            q.size   = (uint64_t)chunk;
            const int cmd = is_read ? kRtHookCmdRead : kRtHookCmdWrite;
            if (ioctl(kRtAnyFd, (unsigned long)cmd, &q) != 0)
                return done ? (ssize_t)done : -1;
            a += chunk;
            p += chunk;
            done += chunk;
        }
        return (ssize_t)done;
    }

    int  pid_ = -1;
    bool verified_ = false;

    // fd для syscall ioctl: диспетчеру драйвера он безразличен (в handler
    // не читается), поэтому -1 — без открытия чего-либо и без следа в fd.
    static constexpr int kRtAnyFd = -1;
};

// Единый экземпляр RT-Hook транспорта (по образцу kernel_transport).
inline RtHookTransport& rt_hook_transport() {
    static RtHookTransport t;
    return t;
}

// RT-Hook доступен? (хук ioctl отвечает пробой по своей памяти — для
// меню выбора и --check-driver).
inline bool rt_hook_available() {
    RtHookTransport& t = rt_hook_transport();
    return t.connected() ? true : t.classify().kind == CMD_READ;
}

// Единый экземпляр транспорта: читает поток отрисовки, дёргают поток
// привязки (open) и фоновые писатели. Запрос живёт на стеке вызывающего.
inline KernelTransport& kernel_transport() {
    static KernelTransport t;
    return t;
}

// Проверка «драйвер загружен?» для меню: находит семейство, поднимает сокет
// (bing или gongchuang) и ВЫУЧИВАЕТ протокол (короткий список кодов, при
// неудаче — полный скан). Пробы идут по собственной памяти, до привязки к игре.
inline bool driver_available(int* family_out = nullptr) {
    KernelTransport& t = kernel_transport();
    if (!t.connected() && t.connect() < 0 && t.gong_connect() < 0) return false;
    if (family_out) *family_out = t.family();
    if (!t.map_known()) t.detect_protocol();
    return true;
}

// Полная проверка драйвера (диагностика): протокол выучен и подтверждён
// живыми пробами. false не значит «кернел выключен»: режим включается и
// проверяется привязкой к игре (bind читает ELF-заголовок libil2cpp).
inline bool driver_self_test() {
    return kernel_transport().self_test();
}

}  // namespace memio
