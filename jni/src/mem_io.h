// mem_io.h — доступ к памяти чужого процесса (игра) с хоста читателя.
//
// По умолчанию сначала используется kernel backend /dev/rwmem, совместимый с
// ri-char/rwMem, затем — /proc/<pid>/mem как fallback. Это даёт рабочий путь на
// устройствах, где SELinux/root-manager запрещает proc-mem, но установлен наш
// драйвер. Переменная RWMEM_BACKEND=kernel|proc|auto фиксирует режим; auto —
// значение по умолчанию. RWMEM_DEVICE задаёт нестандартный путь к dev-узлу.
//
// У обоих backend одинаковая семантика полного read/write, поэтому кэш и весь
// игровой код выше транспорта не знают, откуда пришли байты.
//
// Чтений очень много (ESP на каждого игрока, кости, автофарм): тысячи мелких
// чтений по 4-8 байт за кадр. Кэш блоков подтягивает 128 байт одним чтением и
// отдаёт соседние поля без syscall — это и быстрее, и тише: меньше обращений к
// ядру, меньше шума в аудите.
//
// Кэш живёт ОДИН КАДР: game.cpp зовёт frame_begin() в начале кадра, а запись в
// память игры (Иксрей, «Всегда день») помечает затронутый блок некэшируемым —
// иначе мы читали бы свои же значения.
//
// Хост-стенд: tools/memio/run.sh (читает память реального процесса-ребёнка).

#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

namespace memio {

// Снять тег старшего байта с адреса.
//
// На Android 11+ (ядро с поддержкой ARM Top-byte Ignore) все указатели на
// heap-объекты процесса несут в старшем байте метку: её ставит аллокатор, а
// процессор её игнорирует (TBI), на MTE-устройствах это ещё и 4 бита ключа
// (bits 59:56). Указатели, которые мы вычитываем ИЗ ПАМЯТИ ИГРЫ, приходят
// вместе с этой меткой — и это ломает не нас, а само чтение: адрес уходит в
// /proc/<pid>/mem как есть, ядро тег не снимает (это «чужая» для syscall
// память), страницы по такому адресу не находит и возвращает EIO.
//
// Отсюда ровно то, что видно на части устройств: привязка проходит (пробный
// ELF-заголовок читается по адресу из карты — он без метки), а дальше почти
// каждое чтение по указателю из игры отказывает с errno 5, и чит молчит при
// живом меню. На устройствах без TBI метки нет — там всё работает.
//
// Маска — та же, что у untagged_addr() в ядре: user-space адреса на arm64 не
// выходят за 56 бит, поэтому старший байт можно отбрасывать всегда.
constexpr uint64_t kAddressTagMask = 0x00FFFFFFFFFFFFFFULL;

inline uint64_t untag(uint64_t address) { return address & kAddressTagMask; }

// 128 байт — это несколько рядом лежащих полей объекта il2cpp (и пара матриц
// трансформа).
//
// Блоков было восемь, и они жили в одном кольце: любой девятый адрес кадра
// вытеснял первый, а ESP за кадр обходит камеру, десяток игроков, их кости,
// маркеры и оружие — кольцо из восьми в таком обходе не переживает даже
// одного игрока, и кэш почти всегда промахивался (тысячи pread на кадр).
// Теперь это набор из kSets групп по kWays блоков: адрес всегда ищется в
// СВОЕЙ группе (индекс — биты номера блока), поэтому обход разных объектов
// больше не выбивает друг друга, а поиск стоит kWays сравнений вместо
// линейного прохода. Данные и их срок жизни не изменились: кэш по-прежнему
// живёт один кадр и сбрасывается записью в тот же блок.
constexpr size_t kBlockSize = 128;
constexpr int    kSets      = 64;
constexpr int    kWays      = 4;
constexpr int    kBlocks    = kSets * kWays;   // 256 блоков = 32 КБ на поток

// Потолок жизни блока. Основной предел — смена кадра (frame_begin), это только
// страховка: если кадр почему-то не начался (нет привязки, меню спрятано),
// данные не должны жить в кэше дольше, чем пара кадров.
constexpr double kBlockTtl = 0.05;

// Сколько подряд неудачных чтений считаем «доступ потерян» и переподключаем
// активный backend. Процесс мог перезапуститься с тем же pid (дескриптор при
// этом смотрит в никуда) или доступ могли отобрать — тогда выбираем транспорт
// заново и повторяем ELF-пробу.
constexpr int kFailStreak = 8;

// Адрес ниже этого не может быть отображением пользовательского процесса: такой
// «адрес» — не память, а мусор в переменной (читаем поле объекта, которого нет,
// чей-то индекс вместо указателя и т. п.). Такие отказы отделяем от потери
// доступа: иначе каждый мусорный адрес считался бы «доступ пропал» и тянул за
// собой переподключение backend.
constexpr uint64_t kJunkAddressFloor = 0x1000;

// Сколько адресов блоков помним как «мы сюда писали» (на деле их 2-3: near clip
// камеры и час в TOD_CycleParameters).
constexpr int kWriteTags = 8;

struct Block {
    uint64_t tag   = 0;      // адрес блока + 1; 0 — пусто
    uint64_t gen   = 0;      // поколение кадра, в котором блок прочитан
    double   stamp = 0.0;    // когда прочитан (страховка от «зависшего» кадра)
    uint8_t  data[kBlockSize];
};

struct Cache {
    // Кольцо вытеснения — своё в каждой группе.
    uint8_t next[kSets] = {};
    Block   b[kBlocks];
};

// Группа, в которой обязан лежать блок с этим базовым адресом.
inline int block_set(uint64_t base) {
    return (int)((base / kBlockSize) & (uint64_t)(kSets - 1));
}

inline double now_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// ABI ri-char/rwMem. read/write получают PID и адрес в первых 17 байтах
// пользовательского буфера; count при этом остаётся размером payload.
constexpr size_t kDriverHeaderSize = 17;
constexpr size_t kDriverMapNameSize = 512;
constexpr size_t kDriverMapEntrySize = 8 + 8 + 4 + kDriverMapNameSize;

// Имена ioctl намеренно совпадают с upstream rwMem.
#define RWMEM_IOCTL_MAPS_COUNT _IOW(100, 0, size_t)
#define RWMEM_IOCTL_MAPS_LIST  _IOWR(100, 1, char*)

// /dev/rwmem — имя текущего upstream; /dev/rwMem поддерживается для старого
// CEServer/rwProcMem ABI.
inline int open_driver_node() {
    const char* override_path = getenv("RWMEM_DEVICE");
    if (override_path && override_path[0])
        return open(override_path, O_RDWR | O_CLOEXEC);

    int fd = open("/dev/rwmem", O_RDWR | O_CLOEXEC);
    if (fd < 0) fd = open("/dev/rwMem", O_RDWR | O_CLOEXEC);
    return fd;
}

enum class Backend : int { None = 0, ProcMem = 1, Kernel = 2 };
enum class BackendPreference : int { Auto = 0, ProcMem = 1, Kernel = 2 };

inline BackendPreference backend_preference() {
    const char* value = getenv("RWMEM_BACKEND");
    if (!value || !value[0] || strcasecmp(value, "auto") == 0)
        return BackendPreference::Auto;
    if (strcasecmp(value, "kernel") == 0 || strcasecmp(value, "driver") == 0 ||
        strcasecmp(value, "rwmem") == 0)
        return BackendPreference::Kernel;
    if (strcasecmp(value, "proc") == 0 || strcasecmp(value, "procfs") == 0)
        return BackendPreference::ProcMem;
    return BackendPreference::Auto;
}

inline bool read_proc_maps(pid_t pid, std::string& text) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    text.clear();
    char chunk[8192];
    for (;;) {
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            text.append(chunk, (size_t)n);
            if (text.size() > (8u << 20)) break;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        const bool ok = n == 0;
        close(fd);
        return ok;
    }
    close(fd);
    return !text.empty();
}

// Преобразует upstream binary maps ABI обратно в обычный /proc/maps-текст.
// Старый ABI не передаёт file offset, поэтому ставим 0: lookup библиотек всё
// равно получает начало каждого подходящего VMA как кандидата и затем проверяет
// ELF/TypeInfo. Наш драйвер сохраняет этот ABI для совместимости.
inline bool read_driver_maps(pid_t pid, std::string& text) {
    const int fd = open_driver_node();
    if (fd < 0) return false;

    errno = 0;
    const int count = ioctl(fd, RWMEM_IOCTL_MAPS_COUNT, (unsigned long)pid);
    if (count <= 0 || count > (1 << 20)) {
        close(fd);
        return false;
    }

    const size_t capacity = (size_t)count + 32;
    if (capacity > (SIZE_MAX - 8) / kDriverMapEntrySize) {
        close(fd);
        errno = EOVERFLOW;
        return false;
    }
    std::vector<uint8_t> buffer(8 + capacity * kDriverMapEntrySize, 0);
    const uint64_t raw_pid = (uint64_t)(uint32_t)pid;
    const uint64_t name_size = kDriverMapNameSize;
    const uint64_t buffer_size = buffer.size();
    memcpy(buffer.data(), &raw_pid, sizeof(raw_pid));
    memcpy(buffer.data() + 8, &name_size, sizeof(name_size));
    memcpy(buffer.data() + 16, &buffer_size, sizeof(buffer_size));

    const int unfinished = ioctl(fd, RWMEM_IOCTL_MAPS_LIST, buffer.data());
    const int saved_errno = errno;
    close(fd);
    if (unfinished < 0) {
        errno = saved_errno;
        return false;
    }

    uint64_t actual = 0;
    memcpy(&actual, buffer.data(), sizeof(actual));
    if (actual > capacity) actual = capacity;

    text.clear();
    text.reserve((size_t)actual * 96);
    const uint8_t* entry = buffer.data() + 8;
    for (uint64_t i = 0; i < actual; ++i, entry += kDriverMapEntrySize) {
        uint64_t start = 0, end = 0;
        memcpy(&start, entry, sizeof(start));
        memcpy(&end, entry + 8, sizeof(end));
        if (!start || end <= start) continue;
        const char r = entry[16] ? 'r' : '-';
        const char w = entry[17] ? 'w' : '-';
        const char x = entry[18] ? 'x' : '-';
        const char s = entry[19] ? 's' : 'p';
        const char* name = (const char*)(entry + 20);
        size_t name_len = strnlen(name, kDriverMapNameSize);
        char prefix[96];
        const int n = snprintf(prefix, sizeof(prefix),
                               "%llx-%llx %c%c%c%c 00000000 00:00 0",
                               (unsigned long long)start, (unsigned long long)end,
                               r, w, x, s);
        if (n <= 0) continue;
        text.append(prefix, (size_t)n);
        if (name_len) {
            text.push_back(' ');
            text.append(name, name_len);
        }
        text.push_back('\n');
    }
    return !text.empty();
}

// Карты нужны ещё до Reader::bind(), чтобы найти libil2cpp. В kernel-only
// режиме procfs вообще не трогаем; в auto дешёвый procfs остаётся первым, а
// ioctl драйвера подхватывает устройства с hidepid/SELinux-ограничениями.
inline bool read_process_maps(pid_t pid, std::string& text) {
    if (pid <= 0) return false;
    const BackendPreference pref = backend_preference();
    if (pref == BackendPreference::Kernel) return read_driver_maps(pid, text);
    if (read_proc_maps(pid, text)) return true;
    if (pref == BackendPreference::ProcMem) return false;
    return read_driver_maps(pid, text);
}

class Reader {
public:
    // Привязка к процессу. probe — начало libil2cpp.so с ELF-заголовком.
    // auto сначала проверяет kernel driver и только затем procfs fallback.
    bool bind(pid_t pid, uint64_t probe) {
        unbind();
        pid_.store((int)pid);
        probe_addr_ = probe;
        if (pid <= 0 || !probe) return false;
        if (!connect_and_probe()) {
            close_mem();
            pid_.store(-1);
            return false;
        }
        fail_streak_.store(0);
        last_errno_.store(0, std::memory_order_relaxed);
        last_open_path_.store(0, std::memory_order_relaxed);
        return true;
    }

    void unbind() {
        close_mem();
        pid_.store(-1);
        fail_streak_.store(0);
        generation_.fetch_add(1, std::memory_order_relaxed);
        // Метки записей принадлежали прошлому процессу: их адреса в новой
        // сессии ничего не значат, а ненулевой счётчик держал бы медленный
        // путь в written_recently() навсегда.
        for (auto& tag : write_tags_) tag.store(0);
        write_tags_used_.store(0, std::memory_order_relaxed);
        forget_cache();
    }

    bool  bound() const { return fd_.load() >= 0 && pid_.load() > 0; }
    pid_t pid()   const { return (pid_t)pid_.load(); }

    const char* backend_name() const {
        if (!bound()) return "нет доступа";
        return (Backend)backend_.load(std::memory_order_relaxed) == Backend::Kernel
                   ? "/dev/rwmem (kernel)"
                   : "/proc/<pid>/mem";
    }

    // ---- счётчики для мини-лога ------------------------------------------
    // Доступны всегда (в отличие от MEMIO_STATS): по ним видно «читается, но с
    // отказами» и «не читается совсем», а это первое, что нужно разбирать на
    // устройстве, где работает только меню. Атомарный инкремент в hot path
    // дешевле одного pread на четыре порядка, поэтому счётчики не отключаем.
    unsigned long long reads_total() const { return reads_.load(std::memory_order_relaxed); }
    unsigned long long read_fails_total() const { return read_fails_.load(std::memory_order_relaxed); }
    unsigned long long reopens_total() const { return reopens_.load(std::memory_order_relaxed); }
    // Сколько адресов пришло с меткой в старшем байте (TBI/MTE). Ноль — метки
    // на этом устройстве нет; большое число — она есть, и без снятия тега
    // половина чтений уходила бы в EIO.
    unsigned long long tagged_total() const { return tagged_.load(std::memory_order_relaxed); }
    // Отказы на заведомо невозможных («мусорных») адресах — это ошибка логики
    // чтения, а не потеря доступа; в логе видно и адрес, и фазу, которая читала.
    unsigned long long junk_total() const { return junk_.load(std::memory_order_relaxed); }
    // Чтения, которые упёрлись в конец отображения: часть байт отдана, дальше
    // памяти нет. Это НЕ потеря доступа (первый байт прочитался), но и не полное
    // чтение — держим отдельным счётчиком, чтобы отказы в логе не путались.
    unsigned long long cut_total() const { return cut_.load(std::memory_order_relaxed); }
    uint64_t junk_first_address() const {
        const uint64_t v = junk_addr_.load(std::memory_order_relaxed);
        return v ? v - 1 : 0;
    }
    const char* junk_phase() const { return junk_phase_.load(std::memory_order_relaxed); }
    // Фаза последнего настоящего отказа: по ней видно, какой конвейер читал.
    const char* fail_phase() const { return fail_phase_.load(std::memory_order_relaxed); }

    // Фаза работы чит-кода (какой конвейер читает) — только для диагностики.
    void set_phase(const char* phase) { phase_.store(phase, std::memory_order_relaxed); }
    // Последние адреса отказов (не более 8): по ним видно, куда именно читали.
    int fail_sample(uint64_t* out, int max) const {
        if (!out || max <= 0) return 0;
        int n = 0;
        for (int i = 0; i < kFailSample && n < max; ++i) {
            const uint64_t v = fail_sample_[i].load(std::memory_order_relaxed);
            if (v) out[n++] = v - 1;   // храним адрес + 1, чтобы 0 значил «пусто»
        }
        return n;
    }
    int last_error() const { return last_errno_.load(std::memory_order_relaxed); }
    // true — последняя беда была на открытии backend, а не на чтении.
    bool last_error_was_open() const { return last_open_path_.load(std::memory_order_relaxed) != 0; }
    void clear_last_error() {
        last_errno_.store(0, std::memory_order_relaxed);
        last_open_path_.store(0, std::memory_order_relaxed);
    }

    // Кадр начался: данные, прочитанные в прошлом кадре, больше не отдаём.
    void frame_begin() {
        generation_.fetch_add(1, std::memory_order_relaxed);
#ifdef MEMIO_STATS
        ++frames;
#endif
    }

    // Чтение. Крупное (больше блока) идёт напрямую, мелкое — через кэш блоков.
    // Чтение «на пробу»: тот же доступ, но отказ не попадает в счётчики. Нужно
    // там, где отказ ожидаем и не значит потерю доступа — например, добор строки
    // по байту у границы отображения.
    bool read_quiet(uint64_t addr, void* out, size_t len) {
        if (!addr || !out || !len) return false;
        return raw_read(normalize(addr), out, len, false);
    }

    bool read(uint64_t addr, void* out, size_t len) {
        if (!addr || !out || !len) return false;
        if (fd_.load() < 0 || pid_.load() <= 0) return false;
        addr = normalize(addr);
        if (len > kBlockSize) return raw_read(addr, out, len);

        const uint64_t base = addr & ~(uint64_t)(kBlockSize - 1);
        const size_t   off  = (size_t)(addr - base);
        if (off + len > kBlockSize) return raw_read(addr, out, len); // хвост блока

        Cache& c = cache();
        const uint64_t gen = generation_.load(std::memory_order_relaxed);
        const double   now = now_seconds();

        const int set = block_set(base);
        Block* const group = c.b + (size_t)set * kWays;

        Block* hit = nullptr;
        for (int w = 0; w < kWays; ++w) {
            if (group[w].tag != base + 1) continue;
            hit = &group[w];
            if (hit->gen == gen && (now - hit->stamp) <= kBlockTtl && !written_recently(base)) {
#ifdef MEMIO_STATS
                ++hits;
#endif
                memcpy(out, hit->data + off, len);
                return true;
            }
            break;
        }

        Block& b = hit ? *hit : group[c.next[set]];
        if (!hit) c.next[set] = (uint8_t)((c.next[set] + 1) % kWays);

        if (!raw_read_block(base, b)) {
            // Блок может не читаться целиком: объект лежит вплотную к концу
            // отображения, и 128 байт выходят за него. Тогда читаем ровно
            // столько, сколько просили.
            b.tag = 0;
            return raw_read(addr, out, len);
        }
        b.tag = base + 1;
        b.gen = gen;
        b.stamp = now;
        memcpy(out, b.data + off, len);
        return true;
    }

    bool write(uint64_t addr, const void* in, size_t len) {
        if (!addr || !in || !len) return false;
        if (fd_.load() < 0 || pid_.load() <= 0) return false;
        addr = normalize(addr);
        if (!raw_write(addr, in, len)) return false;
        mark_written(addr, len);
        return true;
    }

    // Проверка «привязка ещё жива»: читаем ELF-заголовок по адресу пробы. Один
    // syscall; нужна, чтобы заметить, что доступ отобрали или процесс перезапустился
    // с тем же pid, — иначе чит молча стоит с привязкой, которая ничего не читает.
    bool verify() {
        if (!bound() || !probe_addr_) return false;
        return probe_readable();
    }

#ifdef MEMIO_STATS
    unsigned long long syscalls = 0, hits = 0, frames = 0, block_reads = 0, reopens = 0;
    void reset_stats() { syscalls = hits = frames = block_reads = 0; }
#endif

private:
    // ---- сам доступ --------------------------------------------------------
    ssize_t driver_read_once(int fd, uint64_t addr, void* out, size_t len) {
        // thread_local убирает malloc из тысяч мелких чтений и не смешивает
        // буферы render/attach/writer потоков.
        static thread_local std::vector<uint8_t> wire;
        const size_t wire_size = len < kDriverHeaderSize ? kDriverHeaderSize : len;
        wire.resize(wire_size);
        const uint64_t raw_pid = (uint64_t)(uint32_t)pid_.load();
        memcpy(wire.data(), &raw_pid, 8);
        memcpy(wire.data() + 8, &addr, 8);
        wire[16] = 0;  // обычное чтение; VMA должна иметь VM_READ

        const ssize_t n = ::read(fd, wire.data(), len);
        if (n > 0) memcpy(out, wire.data(), (size_t)n);
        return n;
    }

    ssize_t driver_write_once(int fd, uint64_t addr, const void* in, size_t len) {
        if (len > SIZE_MAX - kDriverHeaderSize) {
            errno = EOVERFLOW;
            return -1;
        }
        static thread_local std::vector<uint8_t> wire;
        wire.resize(kDriverHeaderSize + len);
        const uint64_t raw_pid = (uint64_t)(uint32_t)pid_.load();
        memcpy(wire.data(), &raw_pid, 8);
        memcpy(wire.data() + 8, &addr, 8);
        wire[16] = 1;  // upstream rwMem force-write ABI
        memcpy(wire.data() + kDriverHeaderSize, in, len);
        return ::write(fd, wire.data(), len);
    }

    ssize_t read_once(uint64_t addr, void* out, size_t len) {
        const int fd = fd_.load();
        if (fd < 0) { errno = EBADF; return -1; }
        if ((Backend)backend_.load(std::memory_order_relaxed) == Backend::Kernel)
            return driver_read_once(fd, addr, out, len);
        return pread(fd, out, len, (off_t)addr);
    }

    ssize_t write_once(uint64_t addr, const void* in, size_t len) {
        const int fd = fd_.load();
        if (fd < 0) { errno = EBADF; return -1; }
        if ((Backend)backend_.load(std::memory_order_relaxed) == Backend::Kernel)
            return driver_write_once(fd, addr, in, len);
        return pwrite(fd, in, len, (off_t)addr);
    }

    // Полное чтение: pread на границе незанятой страницы отдаёт меньше
    // запрошенного, поэтому короткие чтения добираем.
    // Учёт отказа: адрес нужен и счётчику, и выборке последних отказов.
    // Обрыв на границе отображения: адрес начинается в отображённой памяти, но
    // запрошено больше, чем там есть.
    void note_cut_at(uint64_t addr) {
        fail_sample_[fail_next_.fetch_add(1, std::memory_order_relaxed) % kFailSample]
            .store(addr + 1, std::memory_order_relaxed);
        cut_.fetch_add(1, std::memory_order_relaxed);
        // Первый байт прочитался — доступ к памяти есть, копить серию отказов
        // (и переоткрывать файл) не из-за чего.
        note_ok();
    }

    void note_fail_at(uint64_t addr) {
        fail_sample_[fail_next_.fetch_add(1, std::memory_order_relaxed) % kFailSample]
            .store(addr + 1, std::memory_order_relaxed);
        read_fails_.fetch_add(1, std::memory_order_relaxed);
        last_errno_.store(errno, std::memory_order_relaxed);
        if (addr < kJunkAddressFloor) {
            // Такого отображения быть не может: это мусор в адресе. Запоминаем
            // первый такой отказ вместе с фазой, которая читала, и НЕ считаем
            // его потерей доступа (переоткрывать файл тут нечего).
            junk_.fetch_add(1, std::memory_order_relaxed);
            if (!junk_addr_.load(std::memory_order_relaxed)) {
                junk_addr_.store(addr + 1, std::memory_order_relaxed);
                junk_phase_.store(phase_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            return;
        }
        fail_phase_.store(phase_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        note_fail();
    }

    // report = false — попытка «на пробу» (чтение блока целиком, которое может
    // не пройти из-за конца отображения): об отказе не сообщаем, чтобы не
    // считать это потерей доступа и не тянуть переоткрытие файла.
    template <typename Once>
    bool read_full(Once once, uint64_t addr, void* out, size_t len, bool report = true) {
        uint8_t* p = (uint8_t*)out;
        size_t got = 0;
        while (got < len) {
            const ssize_t n = once(addr + got, p + got, len - got);
#ifdef MEMIO_STATS
            ++syscalls;
#endif
            if (n > 0) { got += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (got == 0) {
                if (report) note_fail_at(addr);
                return false;
            }
            break;
        }
        if (got != len) {
            // Часть байт есть, дальше — конец отображения. Это обрыв, а не отказ:
            // вызывающий сам решает, хватает ли прочитанного (строку читаем
            // кусками именно поэтому).
            if (report) note_cut_at(addr);
            return false;
        }
        note_ok();
        return true;
    }

    template <typename Once>
    bool write_full(Once once, uint64_t addr, const void* in, size_t len) {
        const uint8_t* p = (const uint8_t*)in;
        size_t done = 0;
        while (done < len) {
            const ssize_t n = once(addr + done, p + done, len - done);
            if (n > 0) { done += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    bool raw_read(uint64_t addr, void* out, size_t len, bool report = true) {
        return read_full([this](uint64_t a, void* o, size_t l) { return read_once(a, o, l); },
                         addr, out, len, report);
    }

    bool raw_write(uint64_t addr, const void* in, size_t len) {
        return write_full([this](uint64_t a, const void* i, size_t l) { return write_once(a, i, l); },
                          addr, in, len);
    }

    bool raw_read_block(uint64_t base, Block& b) {
#ifdef MEMIO_STATS
        ++block_reads;
#endif
        // Отказ здесь — не потеря доступа: блок мог выйти за конец отображения,
        // и вызывающий всё равно прочитает нужные байты поштучно.
        return raw_read(base, b.data, kBlockSize, false);
    }

    // ELF-заголовок по адресу пробы — и заодно проверка доступа к памяти.
    bool probe_readable() {
        uint8_t magic[4] = {};
        if (!raw_read(probe_addr_, magic, sizeof(magic))) return false;
        return magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
    }

    // ---- отказы -------------------------------------------------------------
    void note_ok() {
        fail_streak_.store(0);
        reads_.fetch_add(1, std::memory_order_relaxed);
    }

    // После серии отказов заново выбираем backend и проверяем ELF. В auto это
    // также позволяет перейти на драйвер, если его загрузили уже после старта.
    void note_fail() {
        if (fail_streak_.fetch_add(1) + 1 < kFailStreak) return;
        fail_streak_.store(0);
        if (!probe_addr_ || pid_.load() <= 0) return;
        close_mem();
        if (connect_and_probe()) {
            reopens_.fetch_add(1, std::memory_order_relaxed);
#ifdef MEMIO_STATS
            ++reopens;
#endif
            return;
        }
        close_mem();
    }

    bool open_kernel() {
        const int fd = open_driver_node();
        if (fd < 0) {
            last_errno_.store(errno, std::memory_order_relaxed);
            last_open_path_.store(1, std::memory_order_relaxed);
            return false;
        }
        backend_.store((int)Backend::Kernel, std::memory_order_relaxed);
        fd_.store(fd);
        return true;
    }

    bool open_proc() {
        char path[40];
        snprintf(path, sizeof(path), "/proc/%d/mem", pid_.load());
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            last_errno_.store(errno, std::memory_order_relaxed);
            last_open_path_.store(1, std::memory_order_relaxed);
            return false;
        }
        backend_.store((int)Backend::ProcMem, std::memory_order_relaxed);
        fd_.store(fd);
        return true;
    }

    bool try_backend(Backend backend) {
        if (!(backend == Backend::Kernel ? open_kernel() : open_proc())) return false;
        if (probe_readable()) return true;
        close_mem();
        return false;
    }

    bool connect_and_probe() {
        if (fd_.load() >= 0) return probe_readable();
        const BackendPreference pref = backend_preference();
        if (pref != BackendPreference::ProcMem && try_backend(Backend::Kernel)) return true;
        if (pref != BackendPreference::Kernel && try_backend(Backend::ProcMem)) return true;
        return false;
    }

    void close_mem() {
        const int fd = fd_.exchange(-1);
        backend_.store((int)Backend::None, std::memory_order_relaxed);
        if (fd >= 0) close(fd);
    }

    // Снять метку старшего байта, посчитав такие адреса (см. kAddressTagMask).
    uint64_t normalize(uint64_t addr) {
        if (!(addr >> 56)) return addr;
        tagged_.fetch_add(1, std::memory_order_relaxed);
        return addr & kAddressTagMask;
    }

    // ---- «мы сюда писали»: такие блоки не кэшируем совсем ------------------
    // Кольцо всего на восемь слотов: в комментарии к kWriteTags так и сказано,
    // «на деле их 2-3». Писатель фрикама пишет один и тот же блок две тысячи
    // раз в секунду и за минуту вытесняет из кольца ВСЁ остальное — после чего
    // written_recently() перестаёт находить блоки, в которые мы пишем редко
    // (near clip камеры, час в TOD_CycleParameters), и кэш начинает отдавать
    // по ним устаревшие данные. Поэтому повторно метку не ставим: если блок
    // уже помечен, кольцо не двигаем.
    void mark_written(uint64_t addr, size_t len) {
        const uint64_t first = addr & ~(uint64_t)(kBlockSize - 1);
        const uint64_t last  = (addr + len - 1) & ~(uint64_t)(kBlockSize - 1);
        for (uint64_t t = first; t <= last; t += kBlockSize) {
            if (written_recently(t)) continue;
            write_tags_[write_next_.fetch_add(1) % kWriteTags].store(t + 1);
            write_tags_used_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bool written_recently(uint64_t base) const {
        // Метки ставятся только Иксреем и «Всегда днём»; обычно их нет вовсе,
        // а проверка идёт на КАЖДОМ попадании в кэш. Один атомарный счётчик
        // снимает восемь атомарных чтений с горячего пути.
        if (write_tags_used_.load(std::memory_order_relaxed) == 0) return false;
        for (int i = 0; i < kWriteTags; ++i)
            if (write_tags_[i].load() == base + 1) return true;
        return false;
    }

    static Cache& cache() {
        static thread_local Cache c;
        return c;
    }

    void forget_cache() {
        Cache& c = cache();
        for (Block& b : c.b) b.tag = 0;
    }

    // Читает поток отрисовки, дёргают ещё поток привязки (bind/verify) и
    // писатель «Всегда день» — поэтому состояние атомарное.
    std::atomic<int> pid_{-1};
    std::atomic<int> fd_{-1};
    std::atomic<int> backend_{(int)Backend::None};
    std::atomic<int> fail_streak_{0};
    // Адрес для пробы доступа (начало отображения libil2cpp.so).
    uint64_t probe_addr_ = 0;

    std::atomic<uint64_t> generation_{1};

    // Счётчики для мини-лога (см. accessors выше).
    std::atomic<unsigned long long> reads_{0};
    std::atomic<unsigned long long> read_fails_{0};
    std::atomic<unsigned long long> reopens_{0};
    std::atomic<int> last_errno_{0};
    std::atomic<unsigned long long> tagged_{0};
    std::atomic<unsigned long long> junk_{0};
    std::atomic<unsigned long long> cut_{0};
    std::atomic<uint64_t> junk_addr_{0};      // первый мусорный адрес + 1
    std::atomic<const char*> junk_phase_{nullptr};
    std::atomic<const char*> phase_{"старт"};
    std::atomic<const char*> fail_phase_{nullptr};
    static constexpr int kFailSample = 8;
    std::atomic<uint64_t> fail_sample_[kFailSample] = {};
    std::atomic<unsigned> fail_next_{0};
    // Признак «последний отказ был на открытии файла» — для текста в логе.
    std::atomic<uint64_t> last_open_path_{0};

    std::atomic<uint64_t> write_tags_[kWriteTags] = {};
    std::atomic<unsigned> write_next_{0};
    // Сколько меток вообще поставлено за сессию (не убывает): ноль — быстрый
    // выход из written_recently() на горячем пути чтения.
    std::atomic<unsigned> write_tags_used_{0};
};

}  // namespace memio
