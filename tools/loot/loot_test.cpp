// Host stand for the instant-pickup controller (tools/loot/run.sh).
//
// The fake below is not a stub of the controller: it replays the GAME side of
// the mechanism the way the shipped image implements it —
//   * DelayIgnoreTimeScalePromise.MoveNext: elapsed += dt; complete on >= duration
//   * completion invokes core.continuation(continuationState) -> runner Run()
//   * one resume of <CollectAllItems>d__6 moves ONE slot and awaits again
//   * the promise goes back to a LIFO pool and is reused for the next await
//   * items registered during a run wait for the next run (waitQueue)
// so "N slots in N frames instead of N * 0.35 s" is measured, not assumed.
#include "loot_memory.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace loot;

namespace {
constexpr uint64_t kArrayLength = 0x18;   // go_active::IL2CPP_ARRAY_LENGTH
constexpr uint64_t kArrayFirst  = 0x20;   // go_active::IL2CPP_ARRAY_FIRST_ELEMENT
constexpr uint64_t kDelegateTarget = 0x20;
constexpr uint64_t kAllocStride = 0x400;
}

// --------------------------------------------------------------------------
// Memory: byte map + class names, exactly the IO contract of game_loot.cpp.
// --------------------------------------------------------------------------
struct FakeMemory {
    std::map<uint64_t, uint8_t> bytes;
    std::map<uint64_t, std::pair<std::string, std::string>> classes;  // klass -> name, ns
    uint64_t next = 0x10000;
    unsigned reads = 0, fresh_reads = 0, writes = 0, name_checks = 0;
    uint64_t fail_read = 0, fail_write = 0;
    std::vector<uint64_t> write_log;
    // Test hook: called before a heap write lands, so a case can move the
    // object out from under the controller the way the pool would.
    std::function<void(uint64_t)> before_write;

    uint64_t alloc(size_t size = 0x200) {
        const uint64_t p = next;
        next += kAllocStride;
        for (size_t i = 0; i < size; ++i) bytes[p + i] = 0;
        return p;
    }
    template<class T> void set(uint64_t p, T v) {
        const auto* in = reinterpret_cast<const uint8_t*>(&v);
        for (size_t i = 0; i < sizeof(T); ++i) bytes[p + i] = in[i];
    }
    template<class T> T get(uint64_t p) {
        T v{};
        assert(read(p, v));
        return v;
    }
    template<class T> bool read(uint64_t p, T& v) {
        ++reads;
        if (p == fail_read) return false;
        auto* out = reinterpret_cast<uint8_t*>(&v);
        for (size_t i = 0; i < sizeof(T); ++i) {
            auto it = bytes.find(p + i);
            if (it == bytes.end()) return false;
            out[i] = it->second;
        }
        return true;
    }
    template<class T> bool read_fresh(uint64_t p, T& v) { ++fresh_reads; return read(p, v); }
    uint64_t ptr(uint64_t p) {
        uint64_t v = 0;
        return read(p, v) ? (v & 0x00ffffffffffffffULL) : 0;
    }
    uint64_t ptr_fresh(uint64_t p) {
        uint64_t v = 0;
        return read_fresh(p, v) ? (v & 0x00ffffffffffffffULL) : 0;
    }
    bool klass_named(uint64_t klass, const char* name, const char* namespaze) {
        ++name_checks;
        auto it = classes.find(klass);
        return it != classes.end() && it->second.first == name && it->second.second == namespaze;
    }
    bool klass_name(uint64_t klass, char* out, size_t out_len) {
        if (!out || out_len < 2) return false;
        out[0] = 0;
        auto it = classes.find(klass);
        if (it == classes.end()) return false;
        std::snprintf(out, out_len, "%s", it->second.first.c_str());
        return true;
    }
    // Журнал устройства в стенде — вектор строк; проверяем, что след
    // диагностики несёт то, что нужно для чтения отказа.
    std::vector<std::string> trace_log;
    template<class... Args>
    void trace(const char* fmt, Args... args) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), fmt, args...);
        trace_log.emplace_back(buf);
    }
    bool trace_has(const char* sub) const {
        for (const auto& line : trace_log)
            if (std::strstr(line.c_str(), sub)) return true;
        return false;
    }
    template<class T> bool write_heap(uint64_t p, const T& v) {
        if (p == fail_write) return false;
        if (before_write) before_write(p);
        ++writes;
        write_log.push_back(p);
        set(p, v);
        return true;
    }
    // Метода write_code здесь НАРОЧНО нет: контроллер обязан обходиться
    // кучей; обращение к записи кода не скомпилируется — регресс-проверка
    // «ни одной записи в образ» встроена в тип.
    uint64_t klass(const char* name, const char* ns) {
        const uint64_t k = alloc(0x100);
        classes[k] = {name, ns};
        return k;
    }
};

// --------------------------------------------------------------------------
// The game side.
// --------------------------------------------------------------------------
struct Game {
    FakeMemory io;
    // Library image: everything the controller reads but must never write.
    const uint64_t base = 0x7000000000ULL;
    const Layout& layout;
    // UniTask runtime objects.
    uint64_t helper_klass = 0, helper_statics = 0, runners_array = 0;
    uint64_t runner_klass = 0, promise_klass = 0, action_klass = 0, continuation_klass = 0;
    struct Loop { uint64_t object = 0, items = 0; int32_t timing = 0; std::vector<uint64_t> live; };
    std::vector<Loop> loops;
    // The stock coroutine: one slot per resume, 0.35 s between them.
    struct CollectAll {
        int slots = 0, moved = 0, awaits = 0;
        uint64_t runner = 0, delegate = 0, promise = 0;
        bool finished = false;
        Kind kind = Kind::Container;
    } collect;
    std::vector<uint64_t> pool;                 // LIFO, like TaskPool
    std::vector<uint64_t> unrelated;            // other pending delays
    float dt = 1.f / 60.f;

    explicit Game(size_t layout_index = 0) : layout(kLayouts[layout_index]) {}

    void map_library() {
        for (size_t i = 0; i < layout.build_id.size(); ++i)
            io.set<uint8_t>(base + kBuildIdOffset + i, layout.build_id[i]);
        for (const auto& flow : layout.flow) {
            io.set<uint32_t>(base + flow.delay_site, flow.delay_word);
            io.set<uint32_t>(base + flow.flag_site, flow.flag_word);
            io.set<uint32_t>(base + flow.call_site, flow.call_word);
            for (int i = 0; i < 4; ++i) io.set<uint32_t>(base + flow.run + i * 4, kRunPrologue[i]);
            const int32_t imm = static_cast<int32_t>((flow.move_next - flow.run) / 4);
            io.set<uint32_t>(base + flow.run + 16,
                             kBranchOp | (static_cast<uint32_t>(imm) & 0x3FFFFFFu));
        }
    }
    uint64_t array(const std::vector<uint64_t>& elements) {
        const uint64_t a = io.alloc(0x20 + elements.size() * 8 + 8);
        io.set<uint64_t>(a, io.klass("Object[]", "System"));
        // Длина массива IL2CPP — 32 бита (так её читает и сам игрок), а соседние
        // четыре байта нам не принадлежат: чтение длины как uint64 взяло бы их.
        io.set<int32_t>(a + kArrayLength, static_cast<int32_t>(elements.size()));
        io.set<uint32_t>(a + kArrayLength + 4, 0xDEADBEEFu);
        for (size_t i = 0; i < elements.size(); ++i)
            io.set<uint64_t>(a + kArrayFirst + i * 8, elements[i]);
        return a;
    }
    void build_unitask() {
        helper_klass = io.klass(kHelperName, kHelperNs[0]);
        helper_statics = io.alloc(0x80);
        io.set<uint64_t>(helper_klass + kClassStaticFields, helper_statics);
        // The controller resolves the class through the metadata slot.
        io.set<uint64_t>(base + layout.helper_typeinfo, helper_klass);
        runner_klass = io.klass(kLoopRunnerName, kLoopRunnerNs[0]);
        promise_klass = io.klass(kPromiseName, kPromiseNs[0]);
        action_klass = io.klass("Action", "System");
        continuation_klass = io.klass("Action`1", "System");
        loops.resize(14);
        for (size_t t = 0; t < loops.size(); ++t) {
            Loop& loop = loops[t];
            loop.timing = static_cast<int32_t>(t);
            loop.object = io.alloc(0x80);
            io.set<uint64_t>(loop.object, runner_klass);
            io.set<int32_t>(loop.object + kRunnerTiming, loop.timing);
        }
        std::vector<uint64_t> runners;
        for (const auto& loop : loops) runners.push_back(loop.object);
        runners_array = array(runners);
        io.set<uint64_t>(helper_statics + kHelperRunners, runners_array);
        publish_all();
    }
    void publish(size_t timing) {
        Loop& loop = loops[timing];
        loop.items = array(loop.live);
        io.set<uint64_t>(loop.object + kRunnerItems, loop.items);
        io.set<int32_t>(loop.object + kRunnerTail, static_cast<int32_t>(loop.live.size()));
    }
    void publish_all() {
        for (size_t t = 0; t < loops.size(); ++t) publish(t);
    }
    // ---- Zenject DI: модель службы окон для детекта (читается контроллером) --
    struct Di {
        std::vector<uint64_t> ids;      // 161 указ.-«строка» в статиках id-класса
        uint64_t service = 0;
        uint64_t dict_a = 0, dict_b = 0;
        bool built = false;
    } di;

    uint64_t di_dict(const std::vector<uint64_t>& keys) {
        const uint32_t cap = static_cast<uint32_t>(keys.size()) + 3;  // лишние = свободные записи
        const uint64_t entries = io.alloc(0x20 + cap * kDiWinStride + 8);
        io.set<uint64_t>(entries, io.klass("Entry[]", "System.Collections.Generic"));
        io.set<int32_t>(entries + kDiArrayLen, static_cast<int32_t>(cap));
        const uint64_t d = io.alloc(0x40);
        io.set<uint64_t>(d, io.klass("Dictionary`2", "System.Collections.Generic"));
        io.set<uint64_t>(d + kDiDictEntries, entries);
        io.set<int32_t>(d + kDiDictCount, static_cast<int32_t>(keys.size()));
        for (size_t i = 0; i < keys.size(); ++i) {
            const uint64_t e = entries + kDiArrayData + i * kDiWinStride;
            io.set<int32_t>(e, 0x1000 + static_cast<int32_t>(i));   // hashCode >= 0: живая
            io.set<int32_t>(e + 4, -1);                             // next
            io.set<uint64_t>(e + kDiWinKey, keys[i]);
        }
        // Одна явно пустая запись (hashCode < 0) за живыми: не должна считаться.
        if (!keys.empty()) {
            const uint64_t e = entries + kDiArrayData + keys.size() * kDiWinStride;
            io.set<int32_t>(e, -1);
        }
        return d;
    }
    // Управляемая строка в фальшивой памяти (System.String: length, UTF-16).
    void di_string(uint64_t str, const char* text) {
        io.set<uint64_t>(str, io.klass("String", "System"));
        const int32_t len = static_cast<int32_t>(std::strlen(text));
        io.set<int32_t>(str + kDiStrLen, len);
        for (int32_t c = 0; c < len; ++c)
            io.set<uint16_t>(str + kDiStrData + static_cast<uint64_t>(c) * 2,
                             static_cast<uint16_t>(text[c]));
    }
    // Собрать контейнер вокруг сервиса: ProjectContext(статики) -> инстанс ->
    // контейнер -> словарь биндингов с ключом BindingId.Type == интерфейс.
    // skip_iface / wrong_cached_klass / wrong_service_klass ломают цепь по сценарию.
    // broken_impl зануляет звено Il2CppType->klass у биндинга интерфейса.
    // direct_impl — живой вариант сборки устройства: RuntimeTypeHandle.value
    // сразу указывает на Il2CppClass без обёртки Il2CppType.
    void build_zenject(int open_root, int open_overlay,
                       bool unknown_key = false, bool skip_iface = false,
                       bool wrong_cached_klass = false, bool wrong_service_klass = false,
                       bool broken_impl = false, bool direct_impl = false) {
        // id-класс окон: статики с 161 указателем на WindowId (= System.String*),
        // имена — живые управляемые строки: length @+0x10, UTF-16 @+0x14.
        for (uint32_t k = 0; k < kDiIdsCount; ++k) di.ids.push_back(io.alloc(0x60));
        // Понятные имена первым двум: первый корневым окном станет "loot".
        di_string(di.ids[0], "loot");
        di_string(di.ids[1], "inventory");
        di_string(di.ids[32], "chat");
        const uint64_t ids_block = io.alloc(kDiIdsCount * 8 + 8);
        for (uint32_t k = 0; k < kDiIdsCount; ++k)
            io.set<uint64_t>(ids_block + k * 8, di.ids[k]);
        const uint64_t ids_klass = io.klass("idsclass", "");
        io.set<uint64_t>(ids_klass + 0xB8, ids_block);
        io.set<uint64_t>(base + layout.ids_typeinfo, ids_klass);
        // Сервис окон.
        di.service = io.alloc(0x100);
        io.set<uint64_t>(di.service,
                         io.klass(wrong_service_klass ? "not-service" : layout.service_name, ""));
        std::vector<uint64_t> keys_a, keys_b;
        for (int i = 0; i < open_root; ++i) keys_a.push_back(di.ids[i]);
        for (int i = 0; i < open_overlay; ++i) keys_b.push_back(di.ids[32 + i]);
        if (unknown_key) keys_b.push_back(io.alloc(0x20));           // чужая строка
        di.dict_a = di_dict(keys_a);
        di.dict_b = di_dict(keys_b);
        io.set<uint64_t>(di.service + kDiServiceDictA, di.dict_a);
        io.set<uint64_t>(di.service + kDiServiceDictB, di.dict_b);
        // CachedProvider с одним инстансом.
        const uint64_t provider = io.alloc(0x40);
        io.set<uint64_t>(provider,
                         io.klass(wrong_cached_klass ? "TransientProvider" : "CachedProvider", "Zenject"));
        const uint64_t insts = io.alloc(0x40);
        io.set<uint64_t>(insts, io.klass("List`1", "System.Collections.Generic"));
        io.set<uint64_t>(insts + kDiListItems, array({di.service}));
        io.set<int32_t>(insts + kDiListSize, 1);
        io.set<uint64_t>(provider + kDiProvInstances, insts);
        const uint64_t info = io.alloc(0x40);
        io.set<uint64_t>(info, io.klass("ProviderInfo", "Zenject"));
        io.set<uint64_t>(info + kDiProviderInfo, provider);
        const uint64_t prov_list = io.alloc(0x40);
        io.set<uint64_t>(prov_list, io.klass("List`1", "System.Collections.Generic"));
        io.set<uint64_t>(prov_list + kDiListItems, array({info}));
        io.set<int32_t>(prov_list + kDiListSize, 1);
        // BindingId.Type == интерфейс: Type (+0x10 → Il2CppType; [0] → klass).
        const uint64_t iface_klass = io.klass(layout.iface_name, "");
        const uint64_t il2c_type = io.alloc(0x10);
        io.set<uint64_t>(il2c_type, broken_impl ? 0x40 : iface_klass);
        const uint64_t type_obj = io.alloc(0x20);
        io.set<uint64_t>(type_obj, io.klass("RuntimeType", "System"));
        io.set<uint64_t>(type_obj + kDiTypeImpl, direct_impl ? iface_klass : il2c_type);
        // Мешающий биндинг с другим типом ключа.
        const uint64_t other_klass = io.klass("OtherService", "Other");
        const uint64_t other_type = io.alloc(0x10);
        io.set<uint64_t>(other_type, other_klass);
        const uint64_t other_obj = io.alloc(0x20);
        io.set<uint64_t>(other_obj, io.klass("RuntimeType", "System"));
        io.set<uint64_t>(other_obj + kDiTypeImpl, other_type);
        // Словарь биндингов: distractor, [iface], свободная запись.
        std::vector<uint64_t> binding_keys = {other_obj};
        if (!skip_iface) binding_keys.push_back(type_obj);
        const uint32_t bcap = 6;
        const uint64_t bentries = io.alloc(0x20 + bcap * kDiBindStride + 8);
        io.set<uint64_t>(bentries, io.klass("Entry[]", "System.Collections.Generic"));
        io.set<int32_t>(bentries + kDiArrayLen, static_cast<int32_t>(bcap));
        for (size_t i = 0; i < binding_keys.size(); ++i) {
            const uint64_t e = bentries + kDiArrayData + i * kDiBindStride;
            io.set<int32_t>(e, 0x2000 + static_cast<int32_t>(i));
            io.set<int32_t>(e + 4, -1);
            io.set<uint64_t>(e + kDiBindKeyType, binding_keys[i]);
        }
        if (!skip_iface) io.set<uint64_t>(bentries + kDiArrayData + kDiBindStride + kDiBindValue, prov_list);
        io.set<int32_t>(bentries + kDiArrayData + 2 * kDiBindStride, -1);  // свободная
        const uint64_t providers = io.alloc(0x40);
        io.set<uint64_t>(providers, io.klass("Dictionary`2", "System.Collections.Generic"));
        io.set<uint64_t>(providers + kDiDictEntries, bentries);
        io.set<int32_t>(providers + kDiDictCount, static_cast<int32_t>(binding_keys.size()));
        const uint64_t container = io.alloc(0x80);
        io.set<uint64_t>(container, io.klass("DiContainer", "Zenject"));
        io.set<uint64_t>(container + kDiContainerProviders, providers);
        const uint64_t instance = io.alloc(0x80);
        io.set<uint64_t>(instance + kDiProjCtxContainer, container);
        const uint64_t statics = io.alloc(0x40);
        io.set<uint64_t>(statics + kDiProjCtxInstance, instance);
        const uint64_t projctx_klass = io.klass("ProjectContext", "Zenject");
        io.set<uint64_t>(projctx_klass + 0xB8, statics);
        io.set<uint64_t>(base + layout.projctx_typeinfo, projctx_klass);
        di.built = true;
    }

    // A pending delay of the stock collect-all coroutine.
    uint64_t make_collect_promise(Kind kind, float duration = 0.35f) {
        const size_t index = static_cast<size_t>(kind);
        uint64_t promise;
        if (!pool.empty()) { promise = pool.back(); pool.pop_back(); }
        else promise = io.alloc(0x100);
        io.set<uint64_t>(promise, promise_klass);
        io.set<float>(promise + kPromiseDuration, duration);
        io.set<float>(promise + kPromiseElapsed, 0.f);
        io.set<uint64_t>(promise + kPromiseCore + kCoreContinuation, continuation_klass);
        if (!collect.delegate) {
            collect.delegate = io.alloc(0x100);
            collect.runner = io.alloc(0x100);
            io.set<uint64_t>(collect.delegate, action_klass);
            io.set<uint64_t>(collect.runner, io.klass(kRunnerName, kRunnerNs[0]));
            io.set<uint64_t>(collect.delegate + kDelegateTarget, collect.runner);
        }
        io.set<uint64_t>(collect.delegate + kDelegateMethodPtr, base + layout.flow[index].run);
        io.set<uint64_t>(promise + kPromiseCore + kCoreState, collect.delegate);
        collect.promise = promise;
        collect.kind = kind;
        return promise;
    }
    // Any other subsystem awaiting the same promise class (UI animation etc.):
    // a different AsyncUniTask`1 instantiation, so a different Run.
    uint64_t make_unrelated_promise(float duration = 5.f) {
        const uint64_t promise = io.alloc(0x100);
        const uint64_t delegate = io.alloc(0x100);
        const uint64_t target = io.alloc(0x100);
        io.set<uint64_t>(target, io.klass(kRunnerName, kRunnerNs[0]));
        io.set<uint64_t>(delegate, action_klass);
        io.set<uint64_t>(delegate + kDelegateMethodPtr, base + layout.flow[0].run + 0x4000);
        io.set<uint64_t>(delegate + kDelegateTarget, target);
        io.set<uint64_t>(promise, promise_klass);
        io.set<float>(promise + kPromiseDuration, duration);
        io.set<float>(promise + kPromiseElapsed, 0.f);
        io.set<uint64_t>(promise + kPromiseCore + kCoreContinuation, continuation_klass);
        io.set<uint64_t>(promise + kPromiseCore + kCoreState, delegate);
        unrelated.push_back(promise);
        return promise;
    }
    void start_collect(int slots, Kind kind = Kind::Container, size_t timing = kUpdateTiming) {
        collect.slots = slots;
        collect.moved = 0;
        collect.awaits = 0;
        collect.finished = false;
        loops[timing].live.push_back(make_collect_promise(kind));
        ++collect.awaits;
        publish(timing);
    }
    // PlayerLoop tick: every registered item gets one MoveNext, exactly like
    // DelayIgnoreTimeScalePromise.MoveNext in the shipped image. Items added
    // while it runs wait for the next tick, like UniTask's waitQueue.
    void tick(size_t timing = kUpdateTiming) {
        Loop& loop = loops[timing];
        const std::vector<uint64_t> snapshot = loop.live;
        for (uint64_t promise : snapshot) {
            if (!promise || io.ptr(promise) != promise_klass) continue;
            if (io.ptr(promise + kPromiseCore + kCoreState) == 0) continue;  // already done
            const float duration = io.get<float>(promise + kPromiseDuration);
            float elapsed = io.get<float>(promise + kPromiseElapsed);
            elapsed += dt;
            io.set<float>(promise + kPromiseElapsed, elapsed);
            if (elapsed < duration) continue;
            // Completed: TryReturn clears both floats and the continuation, and
            // the object goes back to the pool (str xzr,[x0,#0x18] in the image).
            io.set<uint64_t>(promise + kPromiseCore + kCoreState, 0);
            io.set<uint64_t>(promise + kPromiseCore + kCoreContinuation, 0);
            io.set<float>(promise + kPromiseDuration, 0.f);
            io.set<float>(promise + kPromiseElapsed, 0.f);
            for (size_t i = 0; i < loop.live.size(); ++i)
                if (loop.live[i] == promise) { loop.live.erase(loop.live.begin() + static_cast<long>(i)); break; }
            const bool ours = promise == collect.promise;
            pool.push_back(promise);
            if (ours) resume_collect(timing);
        }
        publish(timing);
    }
    // One resume of the stock coroutine: ONE slot, then await again.
    void resume_collect(size_t timing) {
        ++collect.moved;
        --collect.slots;
        if (collect.slots <= 0) { collect.finished = true; collect.promise = 0; return; }
        loops[timing].live.push_back(make_collect_promise(collect.kind));
        ++collect.awaits;
    }
    bool pending(uint64_t promise) { return io.ptr(promise + kPromiseCore + kCoreState) != 0; }
};

// One overlay frame: the controller looks, then the game runs its PlayerLoop.
static void frame(Game& g, Controller<FakeMemory>& c, uint64_t session, bool enabled,
                  size_t timing = kUpdateTiming) {
    c.tick(session, g.base, enabled);
    g.tick(timing);
}

int main() {
    // ---- 1. A full container: one slot per frame, not one per 0.35 s -------
    for (size_t image = 0; image < sizeof(kLayouts) / sizeof(kLayouts[0]); ++image) {
        Game g(image);
        g.map_library();
        g.build_unitask();
        g.start_collect(24);
        Controller<FakeMemory> c(g.io);
        int frames = 0;
        while (!g.collect.finished && frames < 1000) { frame(g, c, 1, true); ++frames; }
        assert(g.collect.finished);
        assert(g.collect.moved == 24);
        assert(frames <= 26);                       // one frame per slot + discovery
        const Diagnostics d = c.diagnostics();
        assert(d.kind == Kind::Container);
        assert(d.timing == static_cast<int>(kUpdateTiming));
        assert(d.expired >= 23);
        assert(d.races == 0);
        assert(g.collect.awaits == 24);             // the game still awaited per item
        // Without the controller the same container needs 0.35 s per slot.
        Game stock(image);
        stock.map_library();
        stock.build_unitask();
        stock.start_collect(24);
        Controller<FakeMemory> idle(stock.io);
        int stock_frames = 0;
        while (!stock.collect.finished && stock_frames < 100000) {
            idle.tick(1, stock.base, false);        // feature off: no writes at all
            stock.tick();
            ++stock_frames;
        }
        assert(stock.collect.finished);
        assert(stock_frames > static_cast<int>(23 * 0.35 / stock.dt));
        assert(idle.diagnostics().status == Status::Off);
        assert(stock.io.writes == 0);
        std::printf("container[%zu]: 24 slots in %d frames with the driver, %d stock\n",
                    image, frames, stock_frames);
    }

    // ---- 2. Every write is the 4-byte elapsed of a promise, in the heap ----
    {
        Game g;
        g.map_library();
        g.build_unitask();
        g.start_collect(6);
        Controller<FakeMemory> c(g.io);
        for (int i = 0; i < 10 && !g.collect.finished; ++i) frame(g, c, 1, true);
        assert(g.collect.moved == 6);
        assert(!g.io.write_log.empty());
        for (uint64_t address : g.io.write_log) {
            // Objects are kAllocStride-aligned, so the residue IS the field.
            assert(address % kAllocStride == kPromiseElapsed);
            assert(address < g.base);               // heap, never the library image
        }
        std::printf("writes: %zu heap writes, all of them the elapsed field\n",
                    g.io.write_log.size());
    }

    // ---- 3. Another subsystem awaiting the same class is never touched -----
    {
        Game g;
        g.map_library();
        g.build_unitask();
        const uint64_t ui = g.make_unrelated_promise(5.f);
        g.loops[kUpdateTiming].live.push_back(ui);
        g.publish(kUpdateTiming);
        Controller<FakeMemory> c(g.io);
        for (int i = 0; i < 40; ++i) frame(g, c, 1, true);
        assert(g.io.writes == 0);
        assert(c.diagnostics().status == Status::Waiting);
        assert(g.pending(ui));
        assert(g.io.get<float>(ui + kPromiseElapsed) < 1.f);
        // And it starts working as soon as the real collect-all appears.
        g.start_collect(3);
        for (int i = 0; i < 40 && !g.collect.finished; ++i) frame(g, c, 1, true);
        assert(g.collect.moved == 3);
        assert(g.pending(ui));                      // the foreign delay is intact
        assert(g.io.get<float>(ui + kPromiseElapsed) < 1.f);
        std::printf("selectivity: foreign delay untouched, collect-all found anyway\n");
    }

    // ---- 4. All three stock flows are recognised --------------------------
    {
        for (Kind kind : {Kind::Container, Kind::Pickup, Kind::FastLoad}) {
            Game g;
            g.map_library();
            g.build_unitask();
            g.start_collect(4, kind);
            Controller<FakeMemory> c(g.io);
            for (int i = 0; i < 20 && !g.collect.finished; ++i) frame(g, c, 1, true);
            assert(g.collect.moved == 4);
            assert(c.diagnostics().kind == kind);
        }
        std::printf("flows: container, pickup and fastload all accelerated\n");
    }

    // ---- 5. A delay registered in another PlayerLoopTiming is still found --
    {
        Game g;
        g.map_library();
        g.build_unitask();
        g.start_collect(3, Kind::Container, 10 /* PreLateUpdate */);
        Controller<FakeMemory> c(g.io);
        for (int i = 0; i < 20 && !g.collect.finished; ++i) {
            c.tick(1, g.base, true);
            g.tick(10);
        }
        assert(g.collect.moved == 3);
        assert(c.diagnostics().timing == 10);
        std::printf("timing: found outside Update as well (timing=%d)\n", c.diagnostics().timing);
    }

    // ---- 6. Image checks: unknown build, retired patch, broken Run --------
    {
        {   // unknown build-id -> refuse, no writes
            Game g;
            g.map_library();
            g.build_unitask();
            g.io.set<uint8_t>(g.base + kBuildIdOffset, 0x00);
            g.start_collect(4);
            Controller<FakeMemory> c(g.io);
            for (int i = 0; i < 5; ++i) frame(g, c, 1, true);
            assert(c.diagnostics().status == Status::Failed);
            assert(c.diagnostics().reason == Reason::Image);
            // Число к причине — первые 8 байт прочитанного build-id.
            assert(c.diagnostics().detail != 0);
            assert(g.io.writes == 0);
            assert(g.collect.moved == 0);
        }
        // Три способа испортить поток. Чужой патч точки задержки — жёсткий отказ
        // потока: там уже «ускорено» чужой правкой инструкций, а duration после
        // неё, скорее всего, ноль. Изменённое тело Run — не отказ: build-id уже
        // привязал таблицу к образу, а принадлежность ожидания доказывается
        // именами классов и указателем метода делегата, не инструкциями Run.
        struct Break { const char* what; uint32_t off; uint32_t word; bool hard; };
        const Break breaks[] = {
            {"the retired instruction patch is still in the process",
             0, 0x14000010u, true},
            {"Run() no longer has the shape the identity chain relies on",
             4, 0xd65f03c0u, false},
            {"Run() branches somewhere else than the state machine we know",
             16, kBranchOp | 0x100u, false},
        };
        const uint32_t all_flows = (1u << kFlowCount) - 1u;
        for (const auto& brk : breaks) {
            const auto site = [&brk](const Game& g, size_t f) {
                return brk.off ? g.layout.flow[f].run + brk.off : g.layout.flow[f].delay_site;
            };
            for (size_t flow = 0; flow < kFlowCount; ++flow) {
                const size_t other = (flow + 1) % kFlowCount;
                {   // испорчен один поток: соседний продолжает ускоряться
                    Game g;
                    g.map_library();
                    g.build_unitask();
                    g.io.set<uint32_t>(g.base + site(g, flow), brk.word);
                    g.start_collect(4, static_cast<Kind>(other));
                    Controller<FakeMemory> c(g.io);
                    for (int i = 0; i < 20 && !g.collect.finished; ++i) frame(g, c, 1, true);
                    assert(c.diagnostics().flows ==
                           (all_flows & ~(brk.hard ? (1u << flow) : 0u)));
                    assert(c.diagnostics().status != Status::Failed);
                    assert(c.diagnostics().kind == static_cast<Kind>(other));
                    assert(g.collect.moved == 4);
                }
                {   // ожидание самого испорченного потока
                    Game g;
                    g.map_library();
                    g.build_unitask();
                    g.io.set<uint32_t>(g.base + site(g, flow), brk.word);
                    g.start_collect(4, static_cast<Kind>(flow));
                    Controller<FakeMemory> c(g.io);
                    for (int i = 0; i < 20 && !g.collect.finished; ++i) frame(g, c, 1, true);
                    if (brk.hard) {
                        // чужой патч задержки: поток отключён, ни одной записи
                        assert(c.diagnostics().flows == (all_flows & ~(1u << flow)));
                        assert(c.diagnostics().hooked == 0);
                        assert(g.io.writes == 0);
                        assert(g.collect.moved == 0);
                    } else {
                        // изменённый Run: поток работает, а слова видны в диагностике
                        assert(c.diagnostics().flows == all_flows);
                        assert(c.diagnostics().hooked == (1u << flow));
                        assert(c.diagnostics().hooked_run == g.layout.flow[flow].run);
                        assert(c.diagnostics().hooked_code[brk.off / 4] == brk.word);
                        assert(g.collect.moved == 4);
                        assert(g.io.writes >= 4);
                    }
                }
            }
            {   // испорчены все три потока
                Game g;
                g.map_library();
                g.build_unitask();
                for (size_t f = 0; f < kFlowCount; ++f)
                    g.io.set<uint32_t>(g.base + site(g, f), brk.word);
                g.start_collect(4);
                Controller<FakeMemory> c(g.io);
                for (int i = 0; i < 20 && !g.collect.finished; ++i) frame(g, c, 1, true);
                if (brk.hard) {
                    assert(c.diagnostics().status == Status::Failed);
                    assert(c.diagnostics().reason == Reason::Patched);
                    assert(c.diagnostics().flows == 0);
                    // Число к причине — точка, по которой отказ опознаётся на
                    // устройстве: для чужого патча это сама точка задержки.
                    assert(c.diagnostics().detail == g.layout.flow[0].delay_site);
                    assert(g.io.writes == 0);
                    assert(g.collect.moved == 0);
                } else {
                    assert(c.diagnostics().status != Status::Failed);
                    assert(c.diagnostics().flows == all_flows);
                    assert(c.diagnostics().hooked == all_flows);
                    assert(c.diagnostics().hooked_run == g.layout.flow[0].run);
                    assert(c.diagnostics().hooked_code[brk.off / 4] == brk.word);
                    assert(g.collect.moved == 4);
                }
            }
        }
        {   // the metadata slot no longer holds PlayerLoopHelper
            Game g;
            g.map_library();
            g.build_unitask();
            g.io.set<uint64_t>(g.base + g.layout.helper_typeinfo, g.promise_klass);
            g.start_collect(4);
            Controller<FakeMemory> c(g.io);
            for (int i = 0; i < 5; ++i) frame(g, c, 1, true);
            assert(c.diagnostics().status == Status::Failed);
            assert(c.diagnostics().reason == Reason::Helper);
            assert(c.diagnostics().detail == g.promise_klass);
            assert(g.io.writes == 0);
        }
        {   // runners[] is gone (helper not initialised yet)
            Game g;
            g.map_library();
            g.build_unitask();
            g.io.set<uint64_t>(g.helper_statics + kHelperRunners, 0);
            g.start_collect(4);
            Controller<FakeMemory> c(g.io);
            for (int i = 0; i < 5; ++i) frame(g, c, 1, true);
            assert(c.diagnostics().status == Status::Failed);
            assert(c.diagnostics().reason == Reason::Runners);
            assert(g.io.writes == 0);
        }
        {   // длина массива раннеров вне границ — тоже отказ, с числом в меню
            Game g;
            g.map_library();
            g.build_unitask();
            g.io.set<int32_t>(g.runners_array + kArrayLength, 4096);
            g.start_collect(4);
            Controller<FakeMemory> c(g.io);
            for (int i = 0; i < 5; ++i) frame(g, c, 1, true);
            assert(c.diagnostics().status == Status::Failed);
            assert(c.diagnostics().reason == Reason::Runners);
            assert(c.diagnostics().detail == 4096);
            assert(g.io.writes == 0);
        }
        std::printf("image checks: unknown build refused, retired patch disables its flow,\n"
                    "              a modified Run keeps the flow alive and is reported\n");
    }

    // ---- 7. Disabled means disabled, and re-enabling rebinds ---------------
    {
        Game g;
        g.map_library();
        g.build_unitask();
        g.start_collect(20);
        Controller<FakeMemory> c(g.io);
        for (int i = 0; i < 3; ++i) frame(g, c, 1, true);
        const int moved_on = g.collect.moved;
        assert(moved_on >= 2);
        const unsigned writes_on = g.io.writes;
        // Fewer frames than one 0.35 s wait: nothing may move on its own.
        for (int i = 0; i < 10; ++i) frame(g, c, 1, false);
        assert(g.io.writes == writes_on);                    // no writes while off
        assert(c.diagnostics().status == Status::Off);
        assert(g.collect.moved == moved_on);                 // the game waits again
        assert(!g.collect.finished);
        assert(g.io.get<float>(g.collect.promise + kPromiseElapsed) < 0.35f);
        for (int i = 0; i < 40 && !g.collect.finished; ++i) frame(g, c, 1, true);
        assert(g.collect.finished);
        assert(g.collect.moved == 20);
        std::printf("toggle: off stops every write, on picks the same coroutine up\n");
    }

    // ---- 8. Session and base changes drop all addresses --------------------
    {
        Game g;
        g.map_library();
        g.build_unitask();
        g.start_collect(10);
        Controller<FakeMemory> c(g.io);
        for (int i = 0; i < 3; ++i) frame(g, c, 1, true);
        assert(g.collect.moved > 0);
        c.reset();
        assert(!c.has_binding());
        const unsigned writes = g.io.writes;
        c.tick(2, 0, true);                                  // process gone
        assert(c.diagnostics().status == Status::Waiting);
        assert(g.io.writes == writes);
        c.tick(3, g.base, true);                             // new session
        for (int i = 0; i < 30 && !g.collect.finished; ++i) frame(g, c, 3, true);
        assert(g.collect.moved == 10);
        std::printf("sessions: reset and rebind without writing to old addresses\n");
    }

    // ---- 9. Pool race: the object is recycled between the read and write ---
    {
        Game g;
        g.map_library();
        g.build_unitask();
        g.start_collect(4);
        Controller<FakeMemory> c(g.io);
        frame(g, c, 1, true);
        assert(c.has_binding());
        const uint64_t promise = g.collect.promise;
        // Somebody else's continuation, as if the pool had recycled the object
        // in the window between our identity check and our write.
        const uint64_t foreign = g.make_unrelated_promise(2.f);
        const uint64_t foreign_delegate = g.io.ptr(foreign + kPromiseCore + kCoreState);
        bool fired = false;
        g.io.before_write = [&](uint64_t address) {
            if (fired || address != promise + kPromiseElapsed) return;
            fired = true;
            g.io.set<float>(promise + kPromiseDuration, 2.f);
            g.io.set<float>(promise + kPromiseElapsed, 0.f);
            g.io.set<uint64_t>(promise + kPromiseCore + kCoreState, foreign_delegate);
        };
        c.tick(1, g.base, true);
        assert(fired);
        assert(g.io.get<float>(promise + kPromiseElapsed) == 0.f);   // repaired
        assert(g.io.get<float>(promise + kPromiseDuration) == 2.f);  // untouched
        assert(c.diagnostics().races == 1);
        assert(!c.has_binding());
        g.io.before_write = nullptr;
        std::printf("race: recycled promise handed back with elapsed = 0\n");
    }

    // ---- 10. TryReturn-zeroed object: end of work, not a failure ----------
    {
        Game g;
        g.map_library();
        g.build_unitask();
        g.start_collect(4);
        Controller<FakeMemory> c(g.io);
        frame(g, c, 1, true);
        assert(c.has_binding());
        const uint64_t promise = g.collect.promise;
        // Корутина дошла до конца: TryReturn зануляет duration/elapsed одной
        // записью xzr (образ: str xzr,[obj,#0x18]), объект ещё висит в списке.
        g.io.set<float>(promise + kPromiseDuration, 0.f);
        g.io.set<float>(promise + kPromiseElapsed, 0.f);
        c.tick(1, g.base, true);
        // Отцепиться без «не применено»: записи нет, статус ждёт, причины нет.
        assert(c.diagnostics().status == Status::Waiting);
        assert(c.diagnostics().reason == Reason::None);
        assert(!c.has_binding());
        // Скан на следующем кадре тоже не цепляется за почищенный объект.
        c.tick(1, g.base, true);
        assert(!c.has_binding());
        assert(c.diagnostics().status == Status::Waiting);
        // Мусор вне окна (не обязательно нули) — то же решение: не трогаем.
        g.io.set<float>(promise + kPromiseDuration, 1234.f);
        g.io.set<float>(promise + kPromiseElapsed, 0.25f);
        c.tick(1, g.base, true);
        assert(c.diagnostics().status == Status::Waiting);
        assert(!c.has_binding());
        assert(g.io.get<float>(promise + kPromiseElapsed) == 0.25f);  // не писали
        // Ни в одном из событий пула записи быть не могло.
        const unsigned writes_after_pool = std::count(g.io.write_log.begin(), g.io.write_log.end(),
                                                      promise + kPromiseElapsed);
        assert(writes_after_pool == 1);            // единственная — само ускорение до этого
        std::printf("pool-zeroed and bogus timers are dropped and rescan silently skips them\n");
    }

    // ---- 11. Memory errors are reported, never guessed around --------------
    {
        Game g;
        g.map_library();
        g.build_unitask();
        g.start_collect(4);
        Controller<FakeMemory> c(g.io);
        frame(g, c, 1, true);
        assert(c.has_binding());
        g.io.fail_write = g.collect.promise + kPromiseElapsed;
        c.tick(1, g.base, true);
        assert(c.diagnostics().status == Status::Failed);
        assert(c.diagnostics().reason == Reason::Write);
        assert(c.diagnostics().detail == g.collect.promise + kPromiseElapsed);
        g.io.fail_write = 0;
        g.io.fail_read = g.collect.promise + kPromiseDuration;
        c.tick(1, g.base, true);
        assert(c.diagnostics().status == Status::Failed);
        assert(c.diagnostics().reason == Reason::Read);
        assert(c.diagnostics().detail == g.collect.promise);
        g.io.fail_read = 0;
        // Таймер вне окна — уже не «отказ» (см. тест 10): пул почищает объект
        // на выходе, и завершающий await гарантированно попадает под этот случай.
        g.io.set<float>(g.collect.promise + kPromiseDuration, 1234.f);
        g.io.set<float>(g.collect.promise + kPromiseElapsed, 0.25f);
        c.tick(1, g.base, true);
        assert(c.diagnostics().status == Status::Waiting);
        assert(c.diagnostics().reason == Reason::None);
        assert(!c.has_binding());
        assert(g.io.get<float>(g.collect.promise + kPromiseElapsed) == 0.25f);  // не писали
        std::printf("errors: failed write and failed read report Failed; bogus timer is a silent pool event\n");
    }

    // ---- 12. Junk in the item list and an absurd tail are survived ---------
    {
        Game g;
        g.map_library();
        g.build_unitask();
        g.loops[kUpdateTiming].live.push_back(0);
        g.loops[kUpdateTiming].live.push_back(0xdeadbeef);
        g.loops[kUpdateTiming].live.push_back(g.io.alloc(0x40));   // foreign object
        for (int i = 0; i < 40; ++i)
            g.loops[kUpdateTiming].live.push_back(g.make_unrelated_promise());
        g.start_collect(2);
        // A tail far beyond the array: the scan must clamp to the real length.
        g.io.set<int32_t>(g.loops[kUpdateTiming].object + kRunnerTail, 1000000);
        g.publish(kUpdateTiming);
        g.io.set<int32_t>(g.loops[kUpdateTiming].object + kRunnerTail, 1000000);
        Controller<FakeMemory> c(g.io);
        const unsigned reads_before = g.io.reads;
        for (int i = 0; i < 30 && !g.collect.finished; ++i) {
            c.tick(1, g.base, true);
            g.tick();
            g.io.set<int32_t>(g.loops[kUpdateTiming].object + kRunnerTail, 1000000);
        }
        assert(g.collect.moved == 2);
        assert(g.io.reads - reads_before < 100000);          // bounded scan
        for (uint64_t promise : g.unrelated) {
            assert(g.pending(promise));
            assert(g.io.get<float>(promise + kPromiseElapsed) < 1.f);
        }
        std::printf("robustness: junk items and a lying tail neither crash nor stall\n");
    }

    // ---- 13. Empty namespace: nested and generic types report it that way --
    {
        Game g;
        g.map_library();
        g.build_unitask();
        g.promise_klass = g.io.klass(kPromiseName, "");     // UniTask/Delay...
        g.start_collect(3);
        const uint64_t runner_klass = g.io.ptr(g.collect.runner);
        g.io.classes[runner_klass] = {kRunnerName, ""};     // AsyncUniTask`1<...>
        Controller<FakeMemory> c(g.io);
        for (int i = 0; i < 20 && !g.collect.finished; ++i) frame(g, c, 1, true);
        assert(g.collect.moved == 3);
        assert(c.diagnostics().status != Status::Failed);
        std::printf("namespaces: an empty one is accepted for nested/generic types\n");
    }

    // ---- 14. A busy item list: foreign classes cannot park the search ------
    {
        // `ours_last` повторяет игру: AddAction дописывает ожидание в хвост,
        // поэтому свежий перенос оказывается последним элементом списка. Обратный
        // случай нужен, чтобы проверка бюджета имён и мемо осталась покрытой.
        auto build = [](Game& g, int foreign, bool ours_last) {
            g.map_library();
            g.build_unitask();
            const auto push_foreign = [&]() {
                for (int i = 0; i < foreign; ++i) {
                    const uint64_t object = g.io.alloc(0x80);
                    const std::string name = "ForeignItem" + std::to_string(i);
                    g.io.set<uint64_t>(object, g.io.klass(name.c_str(), "Some.Namespace"));
                    g.loops[kUpdateTiming].live.push_back(object);
                }
            };
            if (ours_last) push_foreign();
            g.start_collect(3);
            if (!ours_last) { push_foreign(); g.publish(kUpdateTiming); }
        };
        {   // свежее ожидание в хвосте: находится первым же проходом
            Game g;
            build(g, 40, true);
            Controller<FakeMemory> c(g.io);
            int frames = 0;
            while (!g.collect.finished && frames < 60) { frame(g, c, 1, true); ++frames; }
            assert(g.collect.moved == 3);
            assert(frames <= 6);
            assert(c.diagnostics().scans == 1);   // просмотр с конца нашёл его сразу
            g.start_collect(3);
            const unsigned checks = g.io.name_checks;
            frames = 0;
            while (!g.collect.finished && frames < 60) { frame(g, c, 1, true); ++frames; }
            assert(g.collect.moved == 3);
            assert(g.io.name_checks == checks);   // no class is named twice
        }
        {   // 100 чужих классов впереди нашего: тоже один проход
            Game g;
            build(g, 100, true);
            Controller<FakeMemory> c(g.io);
            int frames = 0;
            while (!g.collect.finished && frames < 60) { frame(g, c, 1, true); ++frames; }
            assert(g.collect.moved == 3);
            assert(frames <= 12);
            assert(c.diagnostics().scans == 1);
        }
        {   // наше ожидание в начале, чужих классов больше, чем мемо и бюджет
            Game g;
            build(g, 100, false);
            Controller<FakeMemory> c(g.io);
            int frames = 0;
            while (!g.collect.finished && frames < 60) { frame(g, c, 1, true); ++frames; }
            assert(g.collect.moved == 3);
            assert(frames <= 20);
            assert(c.diagnostics().scans >= 2);   // the name budget was spent
            const unsigned before = g.io.name_checks;
            c.tick(1, g.base, true);
            // One controller check costs one klass_named per accepted namespace.
            const unsigned per_check = sizeof(kPromiseNs) / sizeof(kPromiseNs[0]);
            assert(g.io.name_checks - before <= kNameChecks * per_check);   // bounded
        }
        {   // длинный список: хвост за пределами kMaxItems всё равно виден
            Game g;
            g.map_library();
            g.build_unitask();
            for (int i = 0; i < 1200; ++i) {
                const uint64_t object = g.io.alloc(0x80);
                g.io.set<uint64_t>(object, g.io.klass("Crowd", "Some.Namespace"));
                g.loops[kUpdateTiming].live.push_back(object);
            }
            g.start_collect(3);          // element 1200 of 1201
            Controller<FakeMemory> c(g.io);
            int frames = 0;
            while (!g.collect.finished && frames < 60) { frame(g, c, 1, true); ++frames; }
            assert(g.collect.moved == 3);
            assert(c.diagnostics().scans == 1);
            std::printf("long list: 1201 items, the fresh await at the very end\n");
        }
        std::printf("busy list: 40 and 100 foreign classes, either end of the list\n");
    }

    // ---- 15. Код образа не изменяется никогда ------------------------------
    {
        // Метода записи кода у IO-интерфейса нет (см. комментарий в FakeMemory),
        // поэтому ниже проверяется ровно обратное старому поведению: активный
        // подбор идёт, а слова потоков в образе остаются штатными.
        Game g;
        g.map_library();
        g.build_unitask();
        g.start_collect(24);
        Controller<FakeMemory> c(g.io);
        int frames = 0;
        while (!g.collect.finished && frames < 1000) { frame(g, c, 1, true); ++frames; }
        assert(g.collect.finished && g.collect.moved == 24);
        assert(g.io.writes > 0);                          // только куча
        for (const auto& flow : g.layout.flow) {
            assert(g.io.get<uint32_t>(g.base + flow.delay_site) == flow.delay_word);
            assert(g.io.get<uint32_t>(g.base + flow.flag_site) == flow.flag_word);
            assert(g.io.get<uint32_t>(g.base + flow.call_site) == flow.call_word);
        }
        std::printf("no-patch: window drains per-frame, flow words in the image intact\n");
    }
    {
        // Образ несёт ОСТАТКИ устаревшего инструкционного патча (старая сборка
        // оверлея или чужой мод с тем же трюком): слова отличаются от табличных,
        // для нас это чужая правка — такие потоки отключены, ни одной записи.
        for (size_t broken = 1; broken <= kFlowCount; ++broken) {
            Game g;
            g.map_library();
            g.build_unitask();
            for (size_t f = 0; f < broken; ++f) {
                g.io.set<uint32_t>(g.base + g.layout.flow[f].flag_site, 0xAA1F03E0u);
                g.io.set<uint32_t>(g.base + g.layout.flow[f].call_site, 0xAA1F03E1u);
            }
            const uint32_t alive = ((1u << kFlowCount) - 1u) & ~((1u << broken) - 1u);
            Controller<FakeMemory> c(g.io);
            if (broken < kFlowCount) {
                // Живой поток продолжает ускоряться, испорченный не трогается.
                g.start_collect(4, static_cast<Kind>(broken));
                for (int i = 0; i < 30 && !g.collect.finished; ++i) frame(g, c, 1, true);
                assert(c.diagnostics().flows == alive);
                assert(c.diagnostics().status != Status::Failed);
                assert(g.collect.moved == 4);
            } else {
                // Все потоки несут чужую правку: жёсткий отказ образа.
                g.start_collect(4);
                for (int i = 0; i < 20 && !g.collect.finished; ++i) frame(g, c, 1, true);
                assert(c.diagnostics().status == Status::Failed);
                assert(c.diagnostics().reason == Reason::Patched);
                assert(c.diagnostics().flows == 0);
                assert(c.diagnostics().detail == g.layout.flow[0].call_site);
                assert(g.io.writes == 0);
                assert(g.collect.moved == 0);
            }
        }
        std::printf("no-patch: a patched image is refused flow-by-flow, nothing written\n");
    }
    {
        // Чужая правка кода только в одном потоке: отключается один, два соседа
        // ускоряются; над словами чужой правки ничего не записывается.
        Game g;
        g.map_library();
        g.build_unitask();
        g.io.set<uint32_t>(g.base + g.layout.flow[1].flag_site, 0x14000042u);
        g.io.set<uint32_t>(g.base + g.layout.flow[1].call_site, 0xD65F03C0u);
        Controller<FakeMemory> c(g.io);
        g.start_collect(5, Kind::Pickup);
        for (int i = 0; i < 20; ++i) frame(g, c, 1, true);
        assert(g.collect.moved == 0);
        assert(g.io.writes == 0);
        assert(c.diagnostics().flows == (7u & ~2u));
        assert(g.io.get<uint32_t>(g.base + g.layout.flow[1].flag_site) == 0x14000042u);
        g.start_collect(4, Kind::FastLoad);
        for (int i = 0; i < 30 && !g.collect.finished; ++i) frame(g, c, 1, true);
        assert(g.collect.moved == 4 && g.collect.finished);
        assert(g.io.writes > 0);                         // живой поток — куча
        std::printf("no-patch: a foreign code patch disables only its flow\n");
    }
    {
        // Выкл/вкл тумблера: пишется только kucha, образ остаётся байт-в-байт.
        Game g;
        g.map_library();
        g.build_unitask();
        Controller<FakeMemory> c(g.io);
        frame(g, c, 1, true);
        frame(g, c, 1, false);
        frame(g, c, 1, true);
        g.start_collect(4, Kind::FastLoad);
        for (int i = 0; i < 30 && !g.collect.finished; ++i) frame(g, c, 1, true);
        assert(g.collect.finished && g.collect.moved == 4);
        for (const auto& flow : g.layout.flow) {
            assert(g.io.get<uint32_t>(g.base + flow.flag_site) == flow.flag_word);
            assert(g.io.get<uint32_t>(g.base + flow.call_site) == flow.call_word);
        }
        std::printf("no-patch: toggling never touches image bytes\n");
    }

    // ---- 16. The shipped tables describe themselves consistently -----------
    {
        for (const auto& layout : kLayouts) {
            assert(layout.helper_typeinfo > 0x100000);
            assert(layout.projctx_typeinfo > 0x100000 && layout.ids_typeinfo > 0x100000);
            assert(layout.iface_name[0] == 'm' || layout.iface_name[0] == 'q');
            assert(layout.service_name[0] == layout.iface_name[0]);
            for (const auto& flow : layout.flow) {
                assert(flow.run > 0x100000 && flow.move_next > 0x100000);
                assert(flow.delay_site > 0x100000);
                assert(flow.move_next != flow.run);
                const long long delta = static_cast<long long>(flow.move_next) -
                                        static_cast<long long>(flow.run);
                assert(delta % 4 == 0 && delta > -(1 << 27) && delta < (1 << 27));
                // ldr s0,[x8] / ldr s0,[x8,#imm]: the retired patch site.
                assert((flow.delay_word & 0xFFC003E0u) == 0xBD400100u);
                assert(flow.delay_word == 0xBD495100u || flow.delay_word == 0xBD400100u);
                // Две инструкции у точки задержки как опознавательные знаки
                // «поток штатный»: `mov w0, #1` за 8 байт до загрузки константы
                // и `bl` — через 4 слова после неё. Сами байты перепроверяет
                // check_native.py; несовпадение читается как чужая правка.
                assert(flow.flag_word == 0x52800020u);
                assert((flow.call_word & 0xFC000000u) == 0x94000000u);
                assert(flow.flag_site + 8 == flow.delay_site);
                assert(flow.call_site == flow.delay_site + 0x10);
            }
        }
        std::printf("layouts: %zu images, %zu flows each\n",
                    sizeof(kLayouts) / sizeof(kLayouts[0]), kFlowCount);
    }

    // ---- 17. Детект окна через DI: только чтение, отказ без догадок --------
    for (size_t image = 0; image < sizeof(kLayouts) / sizeof(kLayouts[0]); ++image) {
        {
            // Полная цепочка: ProjectContext -> контейнер -> биндинг интерфейса
            // -> CachedProvider -> инстанс сервиса -> словари окон.
            Game g(image);
            g.map_library();
            g.build_unitask();
            g.build_zenject(2, 1);
            Controller<FakeMemory> c(g.io);
            frame(g, c, 1, true);
            const Diagnostics d = c.diagnostics();
            assert(d.di_stage == 5 && d.di_fail == 0);
            assert(d.di_root == 2 && d.di_overlay == 1 && d.di_unknown == 0);
            assert(d.di_scans == 1);
            // Подписи окон: живые имена прочитаны из ключей, с индексами id и
            // пометкой оверлея. "loot" — корневое окно (ids[0]), "inventory"
            // второе корневое (ids[1]), "chat" — оверлей (ids[32]).
            assert(d.di_names_n == 3);
            assert(std::strcmp(d.di_names[0].data(), "loot") == 0 && d.di_idindex[0] == 0 &&
                   d.di_overlay_win[0] == 0);
            assert(std::strcmp(d.di_names[1].data(), "inventory") == 0 && d.di_idindex[1] == 1);
            assert(std::strcmp(d.di_names[2].data(), "chat") == 0 && d.di_idindex[2] == 32 &&
                   d.di_overlay_win[2] == 1);
            // След журнала ведёт полный разбор резолва: биндинг-интерфейс
            // помечен, провайдерная цепочка завершена адресами.
            assert(g.io.trace_has("== ИНТЕРФЕЙС"));
            assert(g.io.trace_has("di: resolve ok svc="));
            assert(g.io.trace_has("di: скан root=2 overlay=1"));
            // Спонтанное повторное открытие окна: словари перечитываются
            // периодически, новые ключи подхватываются.
            g.start_collect(4, Kind::Container);
            for (int i = 0; i < static_cast<int>(kDiScanFrames + 2); ++i) frame(g, c, 1, true);
            assert(c.diagnostics().di_stage == 5);
            std::printf("window-detect: image %zu full chain, 2 open + 1 overlay, names loot/inventory/chat\n", image);
        }
        {
            // Неизвестный ключ окна (не из таблицы id) считается отдельно.
            Game g(image);
            g.map_library();
            g.build_unitask();
            g.build_zenject(1, 1, true);
            Controller<FakeMemory> c(g.io);
            frame(g, c, 1, true);
            const Diagnostics d = c.diagnostics();
            assert(d.di_stage == 5 && d.di_root == 1 && d.di_overlay == 2 && d.di_unknown == 1);
            // Неизвестный ключ не попадает в имена (ему нет idx), известные читаются.
            assert(d.di_names_n == 2);
            assert(std::strcmp(d.di_names[0].data(), "loot") == 0);
            assert(std::strcmp(d.di_names[1].data(), "chat") == 0 && d.di_overlay_win[1] == 1);
        }
        {
            // Имя ключа — мусор (отрицательная/огромная длина): не «loot» и не
            // отказ, имя просто молча пропущено, счётчики окон живы.
            Game g(image);
            g.map_library();
            g.build_unitask();
            g.build_zenject(1, 0);
            g.io.set<int32_t>(g.di.ids[0] + kDiStrLen, -7);
            Controller<FakeMemory> c(g.io);
            frame(g, c, 1, true);
            const Diagnostics d = c.diagnostics();
            assert(d.di_stage == 5 && d.di_root == 1 && d.di_unknown == 0);
            assert(d.di_names_n == 0);
            // То же с длиной за пределами капы.
            Game g2(image);
            g2.map_library();
            g2.build_unitask();
            g2.build_zenject(1, 0);
            g2.io.set<int32_t>(g2.di.ids[0] + kDiStrLen, 4096);
            Controller<FakeMemory> c2(g2.io);
            frame(g2, c2, 1, true);
            assert(c2.diagnostics().di_names_n == 0);
        }
        {
            // Цепи контекста нет: отказ по самому раннему звену, подбор не тронут.
            Game g(image);
            g.map_library();
            g.build_unitask();
            g.start_collect(4, Kind::Container);
            Controller<FakeMemory> c(g.io);
            for (int i = 0; i < 80 && !g.collect.finished; ++i) frame(g, c, 1, true);
            assert(c.diagnostics().di_fail == 1);
            assert(g.collect.finished && g.collect.moved == 4);
            assert(c.diagnostics().status == Status::Active ||
                   c.diagnostics().status == Status::Waiting);
        }
        {
            // Биндинг интерфейса отсутствует среди чужих — «не найдено»,
            // и это не отказ образа: подбор продолжает работать.
            Game g(image);
            g.map_library();
            g.build_unitask();
            g.build_zenject(0, 0, false, true);
            g.start_collect(4, Kind::Container);
            Controller<FakeMemory> c(g.io);
            for (int i = 0; i < 70 && !g.collect.finished; ++i) frame(g, c, 1, true);
            const Diagnostics d = c.diagnostics();
            assert(d.di_stage == 3 && d.di_fail == 6);
            assert(g.collect.finished);
            // Свидетельный канал: при отказе «интерфейс не найден» имена
            // классов ключей биндингов подняты в диагностику (только чтение).
            assert(d.di_names_n >= 1);
            assert(std::memcmp(d.di_names[0].data(), "OtherService", 13) == 0);
            assert(d.di_idindex[0] == 0 && d.di_overlay_win[0] == 0);
            // Счётчики прохода: одна запись дошла до класса, имя прочитано.
            assert(d.di_walk_visited == 1 && d.di_walk_keyread == 1);
            assert(d.di_walk_tclass == 1 && d.di_walk_count == 1);
            assert(d.di_walk_impl0 != 0);
            // След журнала: старт прохода, перечисленная запись, итог.
            assert(g.io.trace_has("di: resolve старт"));
            assert(g.io.trace_has("name='OtherService'"));
            assert(g.io.trace_has("среди ключей не найден"));
        }
        {
            // Живой словарь, но звено Il2CppType->klass битое: интерфейс есть
            // в ключах, а разрешить его нельзя — ноль имён и счётчики
            // останавливаются ровно на этой ступени (сводка устройства).
            Game g(image);
            g.map_library();
            g.build_unitask();
            g.build_zenject(0, 0, false, false, false, false, true);
            g.start_collect(4, Kind::Container);
            Controller<FakeMemory> c(g.io);
            for (int i = 0; i < 70 && !g.collect.finished; ++i) frame(g, c, 1, true);
            const Diagnostics d = c.diagnostics();
            assert(d.di_stage == 3 && d.di_fail == 6);
            assert(g.collect.finished);
            assert(d.di_walk_visited == 2 && d.di_walk_keyread == 2);
            assert(d.di_walk_tclass == 1 && d.di_walk_count == 2);
            assert(d.di_names_n == 1);
            assert(std::memcmp(d.di_names[0].data(), "OtherService", 13) == 0);
            // Обрыв на звене Il2CppType->klass виден в следе отдельной строкой.
            assert(g.io.trace_has("битый klass"));
        }
        {
            // Живой вариант устройства: RuntimeTypeHandle.value — сам klass
            // (без Il2CppType-обёртки). Цепочка обязана различаться по
            // читаемости имени и находить интерфейс всё равно.
            Game g(image);
            g.map_library();
            g.build_unitask();
            g.build_zenject(1, 0, false, false, false, false, false, true);
            Controller<FakeMemory> c(g.io);
            frame(g, c, 1, true);
            const Diagnostics d = c.diagnostics();
            assert(d.di_stage == 5 && d.di_fail == 0);
            assert(d.di_root == 1 && d.di_unknown == 0);
            assert(d.di_walk_tclass == 2);   // оба биндинга разрешились в klass
            assert(g.io.trace_has("== ИНТЕРФЕЙС"));
            assert(g.io.trace_has("di: resolve ok svc="));
        }
        {
            // Провайдер не CachedProvider или класс инстанса не сервис — отказ,
            // поверх несовпадения ничего не принимается.
            Game g(image);
            g.map_library();
            g.build_unitask();
            g.build_zenject(0, 0, false, false, true);
            Controller<FakeMemory> c(g.io);
            frame(g, c, 1, true);
            assert(c.diagnostics().di_fail == 8);
            Game g2(image);
            g2.map_library();
            g2.build_unitask();
            g2.build_zenject(0, 0, false, false, false, true);
            Controller<FakeMemory> c2(g2.io);
            frame(g2, c2, 1, true);
            assert(c2.diagnostics().di_fail == 10);
        }
        {
            // Выкл: ди-диагностика обнуляется, состояние сохраняется и
            // продолжает после вкл без повторного поиска сервиса.
            Game g(image);
            g.map_library();
            g.build_unitask();
            g.build_zenject(1, 0);
            Controller<FakeMemory> c(g.io);
            frame(g, c, 1, true);
            assert(c.diagnostics().di_stage == 5 && c.diagnostics().di_root == 1);
            frame(g, c, 1, false);
            const Diagnostics d = c.diagnostics();
            assert(d.di_stage == 0 && d.di_root == 0 && d.di_overlay == 0);
            frame(g, c, 1, true);
            assert(c.diagnostics().di_stage == 5 && c.diagnostics().di_root == 1);
        }
        std::printf("window-detect: image %zu refusal paths ok\n", image);
    }

    std::puts("loot: all instant-pickup controller checks passed");
    return 0;
}
