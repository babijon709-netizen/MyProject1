// mem_io.h — доступ к памяти чужого процесса (игра) с хоста читателя.
//
// Почему не «просто process_vm_readv», как было раньше:
//
//   * на части прошивок этот syscall запрещён политикой (SELinux, патчи ядра,
//     «прятки» менеджеров root), но разрешён pread по /proc/<pid>/mem — и
//     наоборот. С одним способом на таких устройствах чит молча не работал:
//     базовый адрес libil2cpp.so находился, меню рисовалось, а каждое чтение
//     возвращало ошибку. Поэтому способов два, выбор — пробой по ELF-заголовку
//     libil2cpp.so, и переключение на второй, если выбранный начал отказывать;
//
//   * чтений очень много (ESP на каждого игрока, кости, автофарм): тысячи
//     мелких чтений по 4-8 байт за кадр. Кэш блоков подтягивает 128 байт одним
//     чтением и отдаёт соседние поля без syscall — это и быстрее, и тише:
//     меньше обращений к ядру, меньше шума в аудите.
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
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

namespace memio {

enum class Backend : int { None = 0, Readv = 1, ProcMem = 2 };

// 128 байт — это несколько рядом лежащих полей объекта il2cpp (и пара матриц
// трансформа); восемь блоков на поток покрывают типичный кадр ESP: камера,
// локальный игрок, цель, пара моделей.
constexpr size_t kBlockSize = 128;
constexpr int    kBlocks    = 8;

// Потолок жизни блока. Основной предел — смена кадра (frame_begin), это только
// страховка: если кадр почему-то не начался (нет привязки, меню спрятано),
// данные не должны жить в кэше дольше, чем пара кадров.
constexpr double kBlockTtl = 0.05;

// Сколько подряд отказов чтения считается «способ не работает» и запускает
// пробу второго способа.
constexpr int kFailStreak = 8;

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
    int   next = 0;
    Block b[kBlocks];
};

inline double now_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

class Reader {
public:
    // Привязка к процессу. probe — адрес, на котором проверяем способ чтения
    // (начало отображения libil2cpp.so: там ELF-заголовок, который обязан
    // читаться). Возвращает false, если ни один способ не работает.
    bool bind(pid_t pid, uint64_t probe) {
        unbind();
        pid_.store((int)pid);
        probe_addr_ = probe;
        if (pid <= 0 || !probe) return false;
        if (use_backend(Backend::Readv, probe)) return true;
        if (use_backend(Backend::ProcMem, probe)) return true;
        pid_.store(-1);
        return false;
    }

    void unbind() {
        close_mem();
        pid_.store(-1);
        backend_.store(Backend::None);
        fail_streak_.store(0);
        generation_.fetch_add(1, std::memory_order_relaxed);
        forget_cache();
    }

    bool    bound()  const { return backend_.load() != Backend::None && pid_.load() > 0; }
    Backend backend() const { return backend_.load(); }
    pid_t   pid()    const { return (pid_t)pid_.load(); }

    // Проверить способ пробой и сделать его текущим. Открывает /proc/<pid>/mem
    // под ProcMem и закрывает, если он не понадобился.
    bool use_backend(Backend want, uint64_t probe) {
        if (pid_.load() <= 0) return false;
        if (want == Backend::ProcMem && !open_mem()) return false;
        const Backend prev = backend_.load();
        backend_.store(want);
        uint8_t magic[4] = {};
        if (!raw_read(probe, magic, sizeof(magic)) ||
            magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
            backend_.store(prev);
            if (want == Backend::ProcMem) {
                close_mem();
            }
            return false;
        }
        fail_streak_.store(0);
        forget_cache();
        return true;
    }

    // Кадр начался: данные, прочитанные в прошлом кадре, больше не отдаём.
    void frame_begin() {
        generation_.fetch_add(1, std::memory_order_relaxed);
#ifdef MEMIO_STATS
        ++frames;
#endif
    }

    // Чтение. Крупное (больше блока) идёт напрямую, мелкое — через кэш блоков.
    bool read(uint64_t addr, void* out, size_t len) {
        if (!addr || !out || !len) return false;
        if (backend_.load() == Backend::None || pid_.load() <= 0) return false;
        if (len > kBlockSize) return raw_read(addr, out, len);

        const uint64_t base = addr & ~(uint64_t)(kBlockSize - 1);
        const size_t   off  = (size_t)(addr - base);
        if (off + len > kBlockSize) return raw_read(addr, out, len); // хвост блока

        Cache& c = cache();
        const uint64_t gen = generation_.load(std::memory_order_relaxed);
        const double   now = now_seconds();

        Block* hit = nullptr;
        for (Block& b : c.b) {
            if (b.tag != base + 1) continue;
            hit = &b;
            if (b.gen == gen && (now - b.stamp) <= kBlockTtl && !written_recently(base)) {
#ifdef MEMIO_STATS
                ++hits;
#endif
                memcpy(out, b.data + off, len);
                return true;
            }
            break;
        }

        Block& b = hit ? *hit : c.b[c.next];
        if (!hit) c.next = (c.next + 1) % kBlocks;

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
        if (backend_.load() == Backend::None || pid_.load() <= 0) return false;
        if (!raw_write(addr, in, len)) return false;
        mark_written(addr, len);
        return true;
    }

    // Проверка «привязка ещё жива»: читаем ELF-заголовок по адресу пробы. Один
    // syscall; нужна, чтобы заметить, что доступ отобрали или процесс перезапустился
    // с тем же pid, — иначе чит молча стоит с привязкой, которая ничего не читает.
    bool verify() {
        if (!bound() || !probe_addr_) return false;
        uint8_t magic[4] = {};
        if (!raw_read(probe_addr_, magic, sizeof(magic))) return false;
        return magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
    }

    // ---- только для стенда и диагностики -----------------------------------
    const char* backend_name() const {
        switch (backend_.load()) {
            case Backend::Readv:   return "process_vm_readv";
            case Backend::ProcMem: return "/proc/<pid>/mem";
            default:               return "нет доступа";
        }
    }
#ifdef MEMIO_STATS
    unsigned long long syscalls = 0, hits = 0, frames = 0, block_reads = 0;
    void reset_stats() { syscalls = hits = frames = block_reads = 0; }
#endif

private:
    // ---- сам доступ --------------------------------------------------------
    ssize_t read_once(uint64_t addr, void* out, size_t len) {
        const Backend be = backend_.load();
        if (be == Backend::Readv) {
            struct iovec local  = {out, len};
            struct iovec remote = {(void*)addr, len};
            return syscall(__NR_process_vm_readv, pid_.load(), &local, 1, &remote, 1, 0);
        }
        if (be == Backend::ProcMem) {
            const int fd = fd_.load();
            return fd < 0 ? -1 : pread(fd, out, len, (off_t)addr);
        }
        return -1;
    }

    ssize_t write_once(uint64_t addr, const void* in, size_t len) {
        if (backend_.load() == Backend::Readv) {
            struct iovec local  = {(void*)in, len};
            struct iovec remote = {(void*)addr, len};
            return syscall(__NR_process_vm_writev, pid_.load(), &local, 1, &remote, 1, 0);
        }
        if (backend_.load() == Backend::ProcMem) {
            const int fd = fd_.load();
            return fd < 0 ? -1 : pwrite(fd, in, len, (off_t)addr);
        }
        return -1;
    }

    // Полное чтение: process_vm_readv на границе незанятой страницы отдаёт
    // меньше запрошенного, /proc/<pid>/mem — тоже умеет короткие чтения.
    template <typename Once>
    bool read_full(Once once, uint64_t addr, void* out, size_t len) {
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
                note_fail();
                return false;
            }
            break;
        }
        if (got != len) {
            note_fail();
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

    bool raw_read(uint64_t addr, void* out, size_t len) {
        return read_full([this](uint64_t a, void* o, size_t l) { return read_once(a, o, l); },
                         addr, out, len);
    }

    bool raw_write(uint64_t addr, const void* in, size_t len) {
        return write_full([this](uint64_t a, const void* i, size_t l) { return write_once(a, i, l); },
                          addr, in, len);
    }

    bool raw_read_block(uint64_t base, Block& b) {
#ifdef MEMIO_STATS
        ++block_reads;
#endif
        return raw_read(base, b.data, kBlockSize);
    }

    // ---- отказы и переключение способа -------------------------------------
    void note_ok() { fail_streak_.store(0); }

    void note_fail() {
        if (fail_streak_.fetch_add(1) + 1 < kFailStreak) return;
        fail_streak_.store(0);
        failover();
    }

    // Выбранный способ перестал работать (процесс перезапустился, отобрали
    // права, ядро вернуло ошибку). Пробуем второй, при неудаче — тот же ещё раз
    // (у ProcMem мог закрыться дескриптор).
    void failover() {
        const uint64_t probe = probe_addr_;
        if (!probe) return;
        const Backend cur = backend_.load();
        if (cur == Backend::Readv) {
            if (use_backend(Backend::ProcMem, probe)) return;
            use_backend(Backend::Readv, probe);
            return;
        }
        if (cur == Backend::ProcMem) {
            close_mem();
            if (open_mem()) {
                uint8_t magic[4] = {};
                if (raw_read(probe, magic, sizeof(magic)) &&
                    magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F')
                    return;
            }
            if (!use_backend(Backend::Readv, probe)) backend_.store(Backend::None);
        }
    }

    bool open_mem() {
        if (fd_.load() >= 0) return true;
        char path[40];
        snprintf(path, sizeof(path), "/proc/%d/mem", pid_.load());
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        fd_.store(fd);
        return true;
    }

    void close_mem() {
        const int fd = fd_.exchange(-1);
        if (fd >= 0) close(fd);
    }

    // ---- «мы сюда писали»: такие блоки не кэшируем совсем ------------------
    void mark_written(uint64_t addr, size_t len) {
        const uint64_t first = addr & ~(uint64_t)(kBlockSize - 1);
        const uint64_t last  = (addr + len - 1) & ~(uint64_t)(kBlockSize - 1);
        for (uint64_t t = first; t <= last; t += kBlockSize)
            write_tags_[write_next_.fetch_add(1) % kWriteTags].store(t + 1);
    }

    bool written_recently(uint64_t base) const {
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
    std::atomic<Backend> backend_{Backend::None};
    std::atomic<int> fail_streak_{0};
    // Адрес для пробы при переключении способа (запоминается в bind через
    // use_backend — источник: начало отображения libil2cpp.so).
    uint64_t probe_addr_ = 0;

    std::atomic<uint64_t> generation_{1};

    std::atomic<uint64_t> write_tags_[kWriteTags] = {};
    std::atomic<unsigned> write_next_{0};
};

}  // namespace memio
