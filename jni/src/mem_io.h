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
// Кэш живёт по TTL (kBlockTtl, 50 мс) СКВОЗЬ КАДРЫ: game.cpp зовёт frame_begin()
// в начале кадра, но он только открывает новое бюджетное окно (kFrameBudget),
// а блоки не сбрасывает. Раньше сбрасывал — и при стриминге игры каждый кадр
// заново упирался в холодные страницы: ESP стоял 200-300 мс при cpu 3-4 мс
// (вскрытие 07:47). Запись в память игры (фрикам, аим, Иксрей, «Всегда день»)
// помечает затронутый блок некэшируемым — иначе мы читали бы свои же значения,
// а замкнутые контуры расходились бы (жалоба 19.09 «фрикам летит сам»).
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
#include <functional>
#include <thread>

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

// Сколько живёт блок. Это ОСНОВНОЙ предел свежести: frame_begin() кэш не
// сбрасывает, и тёплые данные отдаются сквозь кадры (иначе при стриминге игры
// каждый кадр заново упирался бы в холодные страницы — вскрытие 07:47).
// 50 мс несвежести для ESP/аима/фарма не видны; замкнутые контуры (фрикам,
// накопитель аима) читают либо в обход кэша (read_quiet/rd_fresh), либо блоки,
// в которые сами пишут (written_recently — всегда свежие).
constexpr double kBlockTtl = 0.05;

// Бюджет syscall-времени чтений/записей одного кадра оверлея. Принадлежит
// потоку, открывшему кадр (frame_begin запоминает его; писатель фрикама и
// поток привязки не урезаются). Исчерпан — вызовы возвращают false БЕЗ syscall,
// пока кадр не сменится: скан ESP обрывается на полуслове, фильтры держат
// боксы, следующий кадр дочитывает (кэш сквозной — прогресс не теряется).
// 8 мс — половина кадра 60 fps: оверлей не встанет, даже если игра стоит.
// Меряем именно время ВНУТРИ syscall, а не стену кадра: тяжёлый кадр меню или
// вытеснение потока между чтениями бюджет тратить не должны.
constexpr double kFrameBudget = 0.008;

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
    // Чтения, которые упёрлись в конец отображения: часть байт отдана, дальше
    // памяти нет. Это НЕ потеря доступа (первый байт прочитался), но и не полное
    // чтение — держим отдельным счётчиком, чтобы отказы в логе не путались.
    unsigned long long cut_total() const { return cut_.load(std::memory_order_relaxed); }
    // Бюджет кадра (см. kFrameBudget): true — чтения/записи этого кадра уже
    // упирались в бюджет и обрываются. Оборванный скан — это «не прочиталось»,
    // а не «мира нет»: серии неудач не мотать, кэши не сбрасывать, показывать
    // прошлое. Сбрасывается следующим frame_begin().
    bool frame_budget_aborted() const { return frame_aborted_.load(std::memory_order_relaxed); }
    // Всего оборванных бюджетом обращений (телеметрия).
    unsigned long long budget_aborts_total() const { return budget_aborts_.load(std::memory_order_relaxed); }
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
    // true — последняя беда была на открытии /proc/<pid>/mem, а не на чтении.
    bool last_error_was_open() const { return last_open_path_.load(std::memory_order_relaxed) != 0; }
    void clear_last_error() {
        last_errno_.store(0, std::memory_order_relaxed);
        last_open_path_.store(0, std::memory_order_relaxed);
    }

    // Кадр начался: открываем новое бюджетное окно (см. kFrameBudget). Блоки
    // при этом НЕ сбрасываем: они живут по TTL сквозь кадры, тёплые страницы
    // отдаются из кэша, а перечитывается только протухшее. Инвалидация всего
    // сразу осталась только там, где она нужна: unbind() при потере привязки.
    void frame_begin() {
        frame_spent_.store(0.0, std::memory_order_relaxed);
        frame_aborted_.store(false, std::memory_order_relaxed);
        budget_tid_.store(tid_hash(), std::memory_order_relaxed);
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
    // Хэш id текущего потока (считается один раз на поток): бюджет кадра
    // принадлежит тому, кто его открыл (frame_begin), остальные не урезаются.
    static uint64_t tid_hash() {
        static thread_local const uint64_t h =
            (uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id());
        return h;
    }

    // Этот вызов — из потока, открывшего кадр (только он тратит бюджет).
    bool budget_mine() const {
        const uint64_t want = budget_tid_.load(std::memory_order_relaxed);
        return want != 0 && want == tid_hash();
    }

    // true — бюджет исчерпан, syscall делать нельзя (возвращать отказ).
    // Каждый оборванный вызов считается: по счётчику виден объём срезанной
    // работы. Пока окно не открыто (budget_tid_ == 0) не урезан никто.
    bool budget_exceeded() {
        if (!budget_mine()) return false;
        if (!frame_aborted_.load(std::memory_order_relaxed)) return false;
        budget_aborts_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Учесть время syscall в бюджете кадра (вызывает только свой поток).
    void note_spent(double t0) {
        const double spent = frame_spent_.load(std::memory_order_relaxed) + (now_seconds() - t0);
        frame_spent_.store(spent, std::memory_order_relaxed);
        if (spent > kFrameBudget) frame_aborted_.store(true, std::memory_order_relaxed);
    }

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

    // Бюджет — здесь, в единственной точке syscall: попадания в кэш его не
    // тратят (стоять там не на чем), а каждый pread/pwrite — учитывается.
    bool raw_read(uint64_t addr, void* out, size_t len, bool report = true) {
        if (budget_exceeded()) return false;
        const bool mine = budget_mine();
        const double t0 = mine ? now_seconds() : 0.0;
        const bool ok = read_full([this](uint64_t a, void* o, size_t l) { return read_once(a, o, l); },
                                  addr, out, len, report);
        if (mine) note_spent(t0);
        return ok;
    }

    bool raw_write(uint64_t addr, const void* in, size_t len) {
        if (budget_exceeded()) return false;
        const bool mine = budget_mine();
        const double t0 = mine ? now_seconds() : 0.0;
        const bool ok = write_full([this](uint64_t a, const void* i, size_t l) { return write_once(a, i, l); },
                                   addr, in, len);
        if (mine) note_spent(t0);
        return ok;
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
        }
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
    // Бюджетное окно кадра (см. kFrameBudget): открывает frame_begin.
    std::atomic<double>   frame_spent_{0.0};
    std::atomic<bool>     frame_aborted_{false};
    std::atomic<uint64_t> budget_tid_{0};   // 0 — окно не открыто, не урезан никто
    std::atomic<unsigned long long> budget_aborts_{0};

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
};

}  // namespace memio
