// mem_io.h — доступ к памяти чужого процесса (игра) с хоста читателя.
//
// Чтение и запись идут ТОЛЬКО через /proc/<pid>/mem (pread/pwrite). Раньше здесь
// было два способа (process_vm_readv и /proc/<pid>/mem) с пробой по ELF-заголовку
// и переключением при отказах. Выбор способа убран: он не давал ничего, кроме
// ветвлений на каждом обращении, а поведение на устройстве определялось пробой,
// то есть одним из двух исходов, которые и так различимы по факту чтения.
// Теперь исход один: не открылся/не читается /proc/<pid>/mem — привязка
// отклоняется (ESP_ATTACH_NO_ACCESS), и поток привязки повторяет попытку.
//
// Почему /proc/<pid>/mem, а не process_vm_readv: он работает и там, где syscall
// запрещён политикой (SELinux, патчи ядра, «прятки» root-менеджеров), а
// короткие чтения через pread не отличаются по числу обращений к ядру от
// process_vm_readv.
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
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>

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
// трансформа); восемь блоков на поток покрывают типичный кадр ESP: камера,
// локальный игрок, цель, пара моделей.
constexpr size_t kBlockSize = 128;
constexpr int    kBlocks    = 8;

// Потолок жизни блока. Основной предел — смена кадра (frame_begin), это только
// страховка: если кадр почему-то не начался (нет привязки, меню спрятано),
// данные не должны жить в кэше дольше, чем пара кадров.
constexpr double kBlockTtl = 0.05;

// Сколько подряд неудачных чтений считаем «доступ потерян» и переоткрываем
// /proc/<pid>/mem. Дескриптор живёт до первого ESP_ATTACH_OK, но процесс мог
// перезапуститься с тем же pid (дескриптор при этом смотрит в никуда) или
// доступ могли отобрать — тогда помогает только новое открытие.
constexpr int kFailStreak = 8;

// Адрес ниже этого не может быть отображением пользовательского процесса: такой
// «адрес» — не память, а мусор в переменной (читаем поле объекта, которого нет,
// чей-то индекс вместо указателя и т. п.). Такие отказы отделяем от потери
// доступа: иначе каждый мусорный адрес считался бы «доступ пропал» и тянул за
// собой переоткрытие /proc/<pid>/mem.
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
    // Привязка к процессу. probe — адрес, на котором проверяем доступ: начало
    // отображения libil2cpp.so, там ELF-заголовок, который обязан читаться.
    // Возвращает false, если /proc/<pid>/mem не открылся или чтение не прошло.
    bool bind(pid_t pid, uint64_t probe) {
        unbind();
        pid_.store((int)pid);
        probe_addr_ = probe;
        if (pid <= 0 || !probe) return false;
        if (!open_mem()) { pid_.store(-1); return false; }
        if (!probe_readable()) {
            close_mem();
            pid_.store(-1);
            return false;
        }
        fail_streak_.store(0);
        return true;
    }

    void unbind() {
        close_mem();
        pid_.store(-1);
        fail_streak_.store(0);
        generation_.fetch_add(1, std::memory_order_relaxed);
        forget_cache();
    }

    bool  bound() const { return fd_.load() >= 0 && pid_.load() > 0; }
    pid_t pid()   const { return (pid_t)pid_.load(); }

    // Способ доступа один; строка нужна стенду и диагностике.
    const char* backend_name() const {
        return bound() ? "/proc/<pid>/mem" : "нет доступа";
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
    uint64_t junk_first_address() const {
        const uint64_t v = junk_addr_.load(std::memory_order_relaxed);
        return v ? v - 1 : 0;
    }
    const char* junk_phase() const { return junk_phase_.load(std::memory_order_relaxed); }

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
    // true — последняя беда была на открытии /proc/<pid>/mem, а не на чтении.
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
    ssize_t read_once(uint64_t addr, void* out, size_t len) {
        const int fd = fd_.load();
        return fd < 0 ? -1 : pread(fd, out, len, (off_t)addr);
    }

    ssize_t write_once(uint64_t addr, const void* in, size_t len) {
        const int fd = fd_.load();
        return fd < 0 ? -1 : pwrite(fd, in, len, (off_t)addr);
    }

    // Полное чтение: pread на границе незанятой страницы отдаёт меньше
    // запрошенного, поэтому короткие чтения добираем.
    // Учёт отказа: адрес нужен и счётчику, и выборке последних отказов.
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
            if (report) note_fail_at(addr);
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

    // Способ один, переключаться некуда — но дескриптор мог стать негодным
    // (процесс перезапустился с тем же pid, доступ отобрали и вернули). После
    // серии отказов переоткрываем /proc/<pid>/mem и перепроверяем пробу; если и
    // это не помогло, привязку снимаем — поток привязки подключится заново.
    void note_fail() {
        if (fail_streak_.fetch_add(1) + 1 < kFailStreak) return;
        fail_streak_.store(0);
        if (!probe_addr_ || pid_.load() <= 0) return;
        close_mem();
        if (open_mem() && probe_readable()) {
            reopens_.fetch_add(1, std::memory_order_relaxed);
#ifdef MEMIO_STATS
            ++reopens;
#endif
            return;
        }
        close_mem();
    }

    bool open_mem() {
        if (fd_.load() >= 0) return true;
        char path[40];
        snprintf(path, sizeof(path), "/proc/%d/mem", pid_.load());
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            // Ошибка открытия — самая частая причина «работает только меню»
            // (SELinux, hidepid, root-менеджер): пишем её в счётчики, оттуда в лог.
            last_errno_.store(errno, std::memory_order_relaxed);
            last_open_path_.store((uint64_t)1, std::memory_order_relaxed);
            return false;
        }
        fd_.store(fd);
        return true;
    }

    void close_mem() {
        const int fd = fd_.exchange(-1);
        if (fd >= 0) close(fd);
    }

    // Снять метку старшего байта, посчитав такие адреса (см. kAddressTagMask).
    uint64_t normalize(uint64_t addr) {
        if (!(addr >> 56)) return addr;
        tagged_.fetch_add(1, std::memory_order_relaxed);
        return addr & kAddressTagMask;
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
    std::atomic<uint64_t> junk_addr_{0};      // первый мусорный адрес + 1
    std::atomic<const char*> junk_phase_{nullptr};
    std::atomic<const char*> phase_{"старт"};
    static constexpr int kFailSample = 8;
    std::atomic<uint64_t> fail_sample_[kFailSample] = {};
    std::atomic<unsigned> fail_next_{0};
    // Признак «последний отказ был на открытии файла» — для текста в логе.
    std::atomic<uint64_t> last_open_path_{0};

    std::atomic<uint64_t> write_tags_[kWriteTags] = {};
    std::atomic<unsigned> write_next_{0};
};

}  // namespace memio
