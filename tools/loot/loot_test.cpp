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
    template<class T> bool write_heap(uint64_t p, const T& v) {
        if (p == fail_write) return false;
        if (before_write) before_write(p);
        ++writes;
        write_log.push_back(p);
        set(p, v);
        return true;
    }
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
        // Три способа испортить поток: чужой патч точки задержки, изменённый
        // пролог Run, ветка Run не в тот MoveNext. Каждый проверяется в трёх
        // вариантах: испорчен один поток (образ принимается, остальные потоки
        // работают, ожидание испорченного не трогается) и испорчены все три
        // (образ отклоняется с причиной и RVA первой точки).
        struct Break { const char* what; uint32_t off; uint32_t word; Reason reason; };
        const Break breaks[] = {
            {"the retired instruction patch is still in the process",
             0, 0x14000010u, Reason::Patched},
            {"Run() no longer has the shape the identity chain relies on",
             4, 0xd65f03c0u, Reason::RunShape},
            {"Run() branches somewhere else than the state machine we know",
             16, kBranchOp | 0x100u, Reason::RunShape},
        };
        const uint32_t all_flows = (1u << kFlowCount) - 1u;
        for (const auto& brk : breaks) {
            const auto site = [&brk](const Game& g, size_t f) {
                return brk.off ? g.layout.flow[f].run + brk.off : g.layout.flow[f].delay_site;
            };
            for (size_t flow = 0; flow < kFlowCount; ++flow) {
                const size_t other = (flow + 1) % kFlowCount;
                {   // один поток испорчен: соседний продолжает ускоряться
                    Game g;
                    g.map_library();
                    g.build_unitask();
                    g.io.set<uint32_t>(g.base + site(g, flow), brk.word);
                    g.start_collect(4, static_cast<Kind>(other));
                    Controller<FakeMemory> c(g.io);
                    for (int i = 0; i < 20 && !g.collect.finished; ++i) frame(g, c, 1, true);
                    assert(c.diagnostics().flows == (all_flows & ~(1u << flow)));
                    assert(c.diagnostics().status != Status::Failed);
                    assert(c.diagnostics().kind == static_cast<Kind>(other));
                    assert(g.collect.moved == 4);
                }
                {   // ожидание испорченного потока не принимается и не пишется
                    Game g;
                    g.map_library();
                    g.build_unitask();
                    g.io.set<uint32_t>(g.base + site(g, flow), brk.word);
                    g.start_collect(4, static_cast<Kind>(flow));
                    Controller<FakeMemory> c(g.io);
                    for (int i = 0; i < 20; ++i) frame(g, c, 1, true);
                    assert(c.diagnostics().flows == (all_flows & ~(1u << flow)));
                    assert(c.diagnostics().status != Status::Failed);
                    assert(g.io.writes == 0);
                    assert(g.collect.moved == 0);
                }
            }
            {   // испорчены все три: образ отклоняется целиком
                Game g;
                g.map_library();
                g.build_unitask();
                for (size_t f = 0; f < kFlowCount; ++f)
                    g.io.set<uint32_t>(g.base + site(g, f), brk.word);
                g.start_collect(4);
                Controller<FakeMemory> c(g.io);
                for (int i = 0; i < 5; ++i) frame(g, c, 1, true);
                assert(c.diagnostics().status == Status::Failed);
                assert(c.diagnostics().reason == brk.reason);
                assert(c.diagnostics().flows == 0);
                // Число к причине — точка, по которой отказ опознаётся на
                // устройстве: для чужого патча это сама точка задержки, для
                // изменившегося кода — RVA Run этого потока.
                assert(c.diagnostics().detail ==
                       (brk.off ? g.layout.flow[0].run : g.layout.flow[0].delay_site));
                assert(g.io.writes == 0);
                assert(g.collect.moved == 0);
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
        std::printf("image checks: unknown build, retired patch and broken Run all refused\n");
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

    // ---- 10. Memory errors are reported, never guessed around --------------
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
        // An out-of-range duration means the identification is not to be
        // trusted: refuse instead of writing.
        g.io.set<float>(g.collect.promise + kPromiseDuration, 1234.f);
        g.io.set<float>(g.collect.promise + kPromiseElapsed, 0.25f);
        c.tick(1, g.base, true);
        assert(c.diagnostics().status == Status::Failed);
        assert(c.diagnostics().reason == Reason::Timer);
        // В меню это видно как два float'а: duration в младшей половине, elapsed
        // в старшей — ровно те числа, которые контроллер прочитал перед отказом.
        assert((c.diagnostics().detail & 0xffffffffu) == 0x449A4000u);   // 1234.0f
        assert((c.diagnostics().detail >> 32) == 0x3E800000u);           // 0.25f
        std::printf("errors: failed write, failed read and a bogus duration all report Failed\n");
    }

    // ---- 11. Junk in the item list and an absurd tail are survived ---------
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

    // ---- 12. Empty namespace: nested and generic types report it that way --
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

    // ---- 13. A busy item list: foreign classes cannot park the search ------
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

    // ---- 14. The shipped tables describe themselves consistently -----------
    {
        for (const auto& layout : kLayouts) {
            assert(layout.helper_typeinfo > 0x100000);
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
            }
        }
        std::printf("layouts: %zu images, %zu flows each\n",
                    sizeof(kLayouts) / sizeof(kLayouts[0]), kFlowCount);
    }

    std::puts("loot: all instant-pickup controller checks passed");
    return 0;
}
