#pragma once
// Instant pickup: testable memory controller. No instruction writes, no
// library-data writes, no hooks, no in-process code.
//
// IO contract (see game_loot.cpp for the real one):
//   read<T> / ptr        may come from the one-frame block cache: used for
//                        SEARCHING (the runners array, the item list, klass
//                        pointers). A stale entry here is rejected by the
//                        identity chain below, never written to.
//   read_fresh<T> /
//   ptr_fresh            always a fresh read of the game: used for the identity
//                        chain and for the two timer floats around the write,
//                        where one frame of staleness could mean writing into
//                        an object the pool has already handed to someone else.
//   klass_named(klass, name, namespaze)  exact Il2CppClass name+namespace match.
//   write_heap<T>        the only writer, behind the anonymous-heap gate.
//
// How a live delay is found (every step is a read, and every step must pass):
//
//   libil2cpp.so build-id                  -> which layout table row to trust
//   PlayerLoopHelper (TypeInfo by RVA)     -> static_fields+0x30 = runners[]
//   PlayerLoopRunner.tail / .loopItems     -> the live IPlayerLoopItem list
//   item klass name                        -> UniTask+DelayIgnoreTimeScalePromise
//   promise+0x50 core, core+0x20 state     -> the continuation delegate
//   delegate.method_ptr == base + Run RVA  -> WHICH coroutine owns this delay
//   delegate.m_target klass name           -> AsyncUniTask`1 (the pooled runner)
//
// `Run` is unique per generic instantiation, so the method-pointer comparison
// is what ties a delay to "collect all items" and not to some UI animation that
// happens to await the same 0.35 s. Only then is `elapsed` (promise+0x1C) set
// to `delayFrameTimeSpan` (promise+0x18), which the game's own
// DelayIgnoreTimeScalePromise.MoveNext turns into a completion on its next tick:
//
//   ldr s8,[x0,#0x1c] ; bl Time.get_unscaledDeltaTime ; fadd s0,s8,s0
//   ldr s1,[x0,#0x18] ; fcmp s0,s1 ; str s0,[x0,#0x1c] ; b.ge -> core.TrySetResult
//
// Nothing is restored on disable because there is nothing to restore: the game
// clears both floats itself when the promise goes back to its pool
// (TryReturn: `str xzr,[x0,#0x18]`), and we never hold the object longer than
// one tick.
#include "loot.h"
#include "viewmodel.h"          // finite(): the Android build uses -ffast-math
#include "game_offsets_active.h"
#include <array>
#include <cstdint>

namespace loot {

// ---- IL2CPP / UniTask layout ---------------------------------------------
// Runtime layout of IL2CPP and of this UniTask version. Verified byte for byte
// in BOTH shipped game images (release and beta): those builds differ in
// obfuscated names and RVAs, not in these offsets.
inline constexpr uint64_t kClassStaticFields  = 0xB8;  // Il2CppClass::static_fields
inline constexpr uint64_t kHelperRunners      = 0x30;  // PlayerLoopHelper.runners
inline constexpr uint64_t kRunnerTiming       = 0x10;  // PlayerLoopRunner.timing
inline constexpr uint64_t kRunnerTail         = 0x30;  // PlayerLoopRunner.tail
inline constexpr uint64_t kRunnerItems        = 0x38;  // PlayerLoopRunner.loopItems
inline constexpr uint64_t kPromiseDuration    = 0x18;  // delayFrameTimeSpan, s
inline constexpr uint64_t kPromiseElapsed     = 0x1C;  // elapsed, s
inline constexpr uint64_t kPromiseCore        = 0x50;  // UniTaskCompletionSourceCore
inline constexpr uint64_t kCoreContinuation   = 0x18;  // core.continuation
inline constexpr uint64_t kCoreState          = 0x20;  // core.continuationState
inline constexpr uint64_t kDelegateMethodPtr  = 0x10;  // System_Delegate.method_ptr
inline constexpr uint64_t kDelegateMethodCode = 0x40;  // System_Delegate.method_code
inline constexpr uint64_t kBuildIdOffset      = 0x318; // GNU build-id note

// PlayerLoopTiming.Update — where WaitForSeconds(..., ignoreTimeScale) lands.
// Used to look there first and to report it; never as a hard filter, so a game
// update that moves the timing costs a wider scan instead of silent nothing.
inline constexpr int32_t kUpdateTiming = 8;

// Nested IL2CPP types report the declaring type's namespace, and dumpers spell
// that differently, so each name is accepted with every namespace the runtime
// may produce for it. The names themselves are unique in this assembly.
inline constexpr const char* kPromiseName    = "DelayIgnoreTimeScalePromise";
inline constexpr const char* kPromiseNs[]    = {"Cysharp.Threading.Tasks",
                                                "Cysharp.Threading.Tasks.UniTask"};
inline constexpr const char* kRunnerName     = "AsyncUniTask`1";
inline constexpr const char* kRunnerNs[]     = {"Cysharp.Threading.Tasks.CompilerServices",
                                                "Cysharp.Threading.Tasks"};
inline constexpr const char* kHelperName     = "PlayerLoopHelper";
inline constexpr const char* kHelperNs[]     = {"Cysharp.Threading.Tasks"};
inline constexpr const char* kLoopRunnerName = "PlayerLoopRunner";
inline constexpr const char* kLoopRunnerNs[] = {"Cysharp.Threading.Tasks.Internal",
                                                "Cysharp.Threading.Tasks"};

// ---- per-image constants --------------------------------------------------
// One flow = one stock "collect everything" coroutine. `run` is
// AsyncUniTask`1<state machine>::Run, the target of the Action that the
// coroutine hands to its awaiter; `move_next` is the state machine it tail-calls;
// `delay_site`/`delay_word` is the 0.35 literal load inside that state machine —
// the site the retired instruction patch used to occupy. It is READ, never
// written: a process still carrying that patch is refused.
struct FlowSite {
    uint64_t run, move_next, delay_site;
    uint32_t delay_word;
};
struct Layout {
    std::array<uint8_t, 20> build_id;
    uint64_t helper_typeinfo;
    FlowSite flow[3];
};
// Run() of every AsyncUniTask`1 instantiation: take &stateMachine at runner+0x20
// and branch into MoveNext. The four fixed words plus the branch target prove
// both the RVA and the +0x20 offset the identity chain relies on.
inline constexpr uint32_t kRunPrologue[4] = {0xf9401028u, 0x91008000u,
                                             0xf9406108u, 0xf9404101u};
inline constexpr uint32_t kBranchMask = 0xFC000000u;   // unconditional b
inline constexpr uint32_t kBranchOp   = 0x14000000u;

inline constexpr Layout kLayouts[] = {
    // release, libil2cpp.so build-id d85e63cd8dcbd481695012ef2e284f0eeb9ef307
    {{{0xd8,0x5e,0x63,0xcd,0x8d,0xcb,0xd4,0x81,0x69,0x50,
       0x12,0xef,0x2e,0x28,0x4f,0x0e,0xeb,0x9e,0xf3,0x07}},
     0xD9C52E0,
     {{0xB403D10, 0x5B5C5BC, 0x5B5CAD4, 0xBD495100u},   // xw  loot window
      {0xB399854, 0x666147C, 0x6661768, 0xBD495100u},   // eDE pickup zone
      {0xB403428, 0x5B5CE7C, 0x5B5D154, 0xBD495100u}}}, // xv  fast load
    // beta, libil2cpp.so build-id e10795349bd7c2cdc7d8527597c64395a4efed79
    {{{0xe1,0x07,0x95,0x34,0x9b,0xd7,0xc2,0xcd,0xc7,0xd8,
       0x52,0x75,0x97,0xc6,0x43,0x95,0xa4,0xef,0xed,0x79}},
     0xD9A3488,
     {{0xB3E1170, 0x5AF47EC, 0x5AF4D04, 0xBD400100u},   // vB  loot window
      {0xB330434, 0x65C3884, 0x65C3B70, 0xBD400100u},   // REd pickup zone
      {0xB3E1A58, 0x5AF5100, 0x5AF53D8, 0xBD400100u}}}, // vu  fast load
};
inline constexpr size_t kFlowCount = sizeof(kLayouts[0].flow) / sizeof(kLayouts[0].flow[0]);

// Bounds. A scan is bounded three ways (runners, items per runner, items per
// scan) because the game mutates loopItems under its own lock while we read it:
// a lying tail or a grown array must not turn into unbounded work.
inline constexpr uint32_t kMaxRunners  = 64;
inline constexpr uint32_t kMaxItems    = 512;
// Сколько элементов списка просматривается за один проход. Это не счётчик
// чтений: на элемент приходится одно чтение указателя, а проверка имени класса
// (до четырёх чтений строк) платится только за класс, которого ещё нет в мемо.
inline constexpr uint32_t kScanItems   = 2048;
// Проверок имени класса за проход. В живом списке ожидания много чужих классов
// (анимации интерфейса и подобное), поэтому бюджет считается на проход, а не на
// кадр. Он не может «запереть» поиск: мемо класс→вердикт переживает проход и
// заменяется по кругу, а исчерпанный бюджет отменяет паузу повтора, так что
// каждый следующий проход проверяет классы, до которых прошлый не дошёл.
// Найденное однажды больше не требует ни одной проверки (known_klass_).
inline constexpr uint32_t kNameChecks  = 64;
inline constexpr size_t   kMemoSlots   = 64;
inline constexpr unsigned kRetryFrames = 15;    // backoff while nothing is live
inline constexpr unsigned kHotFrames   = 10;    // rescan at once right after a hit
inline constexpr float    kMaxDelay    = 10.f;  // sane loot delay, seconds
inline constexpr float    kMinDelay    = 0.0001f;

template<class IO, size_t N>
bool named_one_of(IO& io, uint64_t klass, const char* name, const char* const (&ns)[N]) {
    for (size_t i = 0; i < N; ++i) if (io.klass_named(klass, name, ns[i])) return true;
    return false;
}

// Accept an image only when the build-id is known AND every code site we rely on
// still holds its original instructions. nullptr for an unknown build and for a
// process that still carries the retired delay patch.
template<class IO> const Layout* layout_for(IO& io, uint64_t base) {
    if (base < 0x10000) return nullptr;
    std::array<uint8_t, 20> id{};
    if (!io.read(base + kBuildIdOffset, id)) return nullptr;
    for (const auto& layout : kLayouts) {
        if (layout.build_id != id) continue;
        for (const auto& flow : layout.flow) {
            uint32_t word = 0;
            if (!io.read(base + flow.delay_site, word) || word != flow.delay_word) return nullptr;
            std::array<uint32_t, 5> code{};
            if (!io.read(base + flow.run, code)) return nullptr;
            for (int i = 0; i < 4; ++i) if (code[i] != kRunPrologue[i]) return nullptr;
            if ((code[4] & kBranchMask) != kBranchOp) return nullptr;
            const int32_t imm = (static_cast<int32_t>(code[4] << 6)) >> 6;   // imm26
            const int64_t target = static_cast<int64_t>(base + flow.run) +
                                   static_cast<int64_t>(imm) * 4;
            if (target != static_cast<int64_t>(base + flow.move_next)) return nullptr;
        }
        return &layout;
    }
    return nullptr;
}

template<class IO> class Controller {
    IO& io;
    uint64_t session_ = 0, base_ = 0;
    const Layout* layout_ = nullptr;
    uint64_t runners_ = 0;
    uint32_t runners_len_ = 0;
    uint64_t loop_runner_klass_ = 0, promise_klass_ = 0;
    // The promise class once proven by name. Survives drop(): re-finding the
    // next slot of the same container must not pay for name reads again.
    uint64_t known_klass_ = 0;
    // klass -> "is it the delay promise" memo: a scan reads one pointer per
    // item, and only an unseen klass pays for a name read.
    struct Memo { uint64_t klass = 0; bool promise = false; };
    std::array<Memo, kMemoSlots> memo_{};
    size_t memo_next_ = 0;
    bool name_budget_spent_ = false;
    // Live binding: the promise we expire, its continuation delegate, and the
    // pooled runner that owns the state machine.
    uint64_t promise_ = 0, delegate_ = 0, runner_ = 0;
    Kind kind_ = Kind::None;
    int timing_ = -1;
    unsigned retry_ = 0, hot_ = 0;
    Diagnostics diag_{};

    bool is_promise(uint64_t object, uint32_t& name_checks) {
        const uint64_t klass = io.ptr(object);
        if (klass < 0x10000) return false;
        if (klass == known_klass_) return true;          // proven before: free
        for (const auto& m : memo_) if (m.klass == klass) return m.promise;
        if (!name_checks) { name_budget_spent_ = true; return false; }
        --name_checks;
        const bool yes = named_one_of(io, klass, kPromiseName, kPromiseNs);
        if (yes && !known_klass_) known_klass_ = klass;
        memo_[memo_next_] = {klass, yes};
        memo_next_ = (memo_next_ + 1) % memo_.size();
        return yes;
    }

    bool runners_ready() {
        if (runners_) return true;
        const uint64_t klass = io.ptr(base_ + layout_->helper_typeinfo);
        if (klass < 0x10000 || !named_one_of(io, klass, kHelperName, kHelperNs)) return false;
        const uint64_t statics = io.ptr(klass + kClassStaticFields);
        if (statics < 0x10000) return false;
        const uint64_t array = io.ptr(statics + kHelperRunners);
        uint64_t length = 0;
        if (array < 0x10000 || !io.read(array + go_active::IL2CPP_ARRAY_LENGTH, length) ||
            !length || length > kMaxRunners) return false;
        runners_ = array;
        runners_len_ = static_cast<uint32_t>(length);
        return true;
    }

    bool loop_runner(uint64_t object, int32_t& timing) {
        const uint64_t klass = io.ptr(object);
        if (klass < 0x10000) return false;
        if (klass != loop_runner_klass_) {
            if (loop_runner_klass_ || !named_one_of(io, klass, kLoopRunnerName, kLoopRunnerNs))
                return false;
            loop_runner_klass_ = klass;
        }
        return io.read(object + kRunnerTiming, timing);
    }

    // Full identity chain for one candidate. Only called for items whose klass
    // is already known to be the delay promise, so it is a handful of reads —
    // all of them fresh, because this is what the write decision rests on.
    bool match(uint64_t item, Kind& kind, uint64_t& delegate, uint64_t& runner) {
        const uint64_t state = io.ptr_fresh(item + kPromiseCore + kCoreState);
        if (state < 0x10000) return false;
        uint64_t entry = 0;
        if (!io.read_fresh(state + kDelegateMethodPtr, entry)) return false;
        int found = -1;
        for (size_t i = 0; i < kFlowCount; ++i)
            if (entry == base_ + layout_->flow[i].run) found = static_cast<int>(i);
        if (found < 0) {
            // method_code carries the same code address on some delegate shapes.
            if (!io.read_fresh(state + kDelegateMethodCode, entry)) return false;
            for (size_t i = 0; i < kFlowCount; ++i)
                if (entry == base_ + layout_->flow[i].run) found = static_cast<int>(i);
        }
        if (found < 0) return false;
        const uint64_t continuation = io.ptr_fresh(item + kPromiseCore + kCoreContinuation);
        const uint64_t target = io.ptr_fresh(state + go_active::DELEGATE_TARGET);
        if (continuation < 0x10000 || target < 0x10000) return false;
        const uint64_t klass = io.ptr_fresh(target);
        if (klass < 0x10000 || !named_one_of(io, klass, kRunnerName, kRunnerNs)) return false;
        kind = static_cast<Kind>(found);
        delegate = state;
        runner = target;
        return true;
    }

    bool scan() {
        ++diag_.scans;
        name_budget_spent_ = false;
        uint32_t budget = kScanItems, name_checks = kNameChecks;
        // Update first: that is where the stock delays are registered.
        for (int pass = 0; pass < 2 && !promise_; ++pass) {
            for (uint32_t r = 0; r < runners_len_ && budget && !promise_; ++r) {
                const uint64_t object = io.ptr(runners_ + go_active::IL2CPP_ARRAY_FIRST_ELEMENT +
                                               static_cast<uint64_t>(r) * 8);
                if (object < 0x10000) continue;
                int32_t timing = -1;
                if (!loop_runner(object, timing)) continue;
                if ((pass == 0) != (timing == kUpdateTiming)) continue;
                const uint64_t items = io.ptr(object + kRunnerItems);
                uint64_t capacity = 0;
                int32_t tail = 0;
                if (items < 0x10000 || !io.read(items + go_active::IL2CPP_ARRAY_LENGTH, capacity) ||
                    !capacity || !io.read(object + kRunnerTail, tail) || tail <= 0) continue;
                uint64_t count = static_cast<uint64_t>(tail);
                if (count > capacity) count = capacity;
                if (count > kMaxItems) count = kMaxItems;
                for (uint64_t i = 0; i < count && budget; ++i) {
                    --budget;
                    const uint64_t item = io.ptr(items + go_active::IL2CPP_ARRAY_FIRST_ELEMENT + i * 8);
                    if (item < 0x10000 || !is_promise(item, name_checks)) continue;
                    Kind kind = Kind::None;
                    uint64_t delegate = 0, runner = 0;
                    if (!match(item, kind, delegate, runner)) continue;
                    promise_ = item; delegate_ = delegate; runner_ = runner;
                    kind_ = kind; timing_ = timing; promise_klass_ = io.ptr_fresh(item);
                    return true;
                }
            }
        }
        return false;
    }

    // Cheap re-check of a cached binding: the promise is pooled, so between two
    // ticks it can be completed and handed to an unrelated subsystem.
    bool bound() {
        if (!promise_ || !delegate_ || !runner_ || !promise_klass_) return false;
        if (io.ptr_fresh(promise_) != promise_klass_) return false;
        if (io.ptr_fresh(promise_ + kPromiseCore + kCoreState) != delegate_) return false;
        if (io.ptr_fresh(delegate_ + go_active::DELEGATE_TARGET) != runner_) return false;
        if (io.ptr_fresh(runner_) == 0) return false;
        uint64_t entry = 0;
        return io.read_fresh(delegate_ + kDelegateMethodPtr, entry) &&
               entry == base_ + layout_->flow[static_cast<size_t>(kind_)].run;
    }

    void drop() {
        promise_ = delegate_ = runner_ = promise_klass_ = 0;
        kind_ = Kind::None;
        timing_ = -1;
    }

    // The one write: elapsed = duration, so the game's own next PlayerLoop tick
    // resumes the coroutine instead of waiting the rest of the 0.35 s.
    bool expire() {
        float duration = 0.f, elapsed = 0.f;
        if (!io.read_fresh(promise_ + kPromiseDuration, duration) ||
            !io.read_fresh(promise_ + kPromiseElapsed, elapsed)) return false;
        if (!viewmodel::finite(duration) || duration < kMinDelay || duration > kMaxDelay) return false;
        if (!viewmodel::finite(elapsed) || elapsed < 0.f) return false;
        if (elapsed >= duration) return true;   // already due: resumes this frame
        if (!io.write_heap(promise_ + kPromiseElapsed, duration)) return false;
        ++diag_.expired;
        hot_ = kHotFrames;
        // The pool can hand this object to another awaiter between our read and
        // our write. Re-check; if it moved on, put back what the game's own
        // Create() leaves there (elapsed = 0) so an unrelated delay is not
        // shortened by our stale value.
        float now = 0.f;
        if (!io.read_fresh(promise_ + kPromiseElapsed, now) || now != duration) { drop(); return true; }
        if (bound()) return true;
        // Lost the identity race: if the object is still a pending delay of the
        // same class but with somebody else's continuation, it was recycled by
        // the pool — give it back the zero its own Create() would have left.
        if (io.ptr_fresh(promise_) == promise_klass_ &&
            io.ptr_fresh(promise_ + kPromiseCore + kCoreState) >= 0x10000) {
            if (io.write_heap(promise_ + kPromiseElapsed, 0.f)) ++diag_.races;
        }
        drop();
        return true;
    }

public:
    explicit Controller(IO& memory) : io(memory) {}
    const Diagnostics& diagnostics() const { return diag_; }
    bool has_binding() const { return promise_ != 0; }

    // Forget every address. Nothing is written: there is no residue to undo.
    void reset() {
        layout_ = nullptr;
        runners_ = 0; runners_len_ = 0;
        loop_runner_klass_ = promise_klass_ = known_klass_ = 0;
        memo_ = {};
        memo_next_ = 0;
        name_budget_spent_ = false;
        drop();
        retry_ = hot_ = 0;
        diag_ = {};
    }

    void tick(uint64_t session, uint64_t base, bool enabled) {
        if (session != session_ || base != base_) { reset(); session_ = session; base_ = base; }
        if (!enabled) {
            // Stop touching the game at once. A delay expired on the previous
            // tick is the game's own pending transfer; there is nothing to undo.
            drop();
            retry_ = hot_ = 0;
            diag_.status = Status::Off;
            diag_.kind = Kind::None;
            diag_.timing = -1;
            return;
        }
        diag_.status = Status::Waiting;
        diag_.kind = Kind::None;
        diag_.timing = -1;
        if (!base) return;
        if (!layout_) {
            layout_ = layout_for(io, base);
            if (!layout_) { diag_.status = Status::Failed; return; }
        }
        if (!runners_ready()) { diag_.status = Status::Failed; return; }
        if (promise_ && !bound()) drop();
        if (!promise_) {
            if (retry_) { --retry_; return; }
            if (!scan()) {
                if (hot_) --hot_;
                // Исчерпанный бюджет имён — не «ничего нет», а «дошли не до
                // всех»: мемо за этот проход вырос, поэтому повторяем сразу.
                retry_ = (hot_ || name_budget_spent_) ? 0u : kRetryFrames;
                return;
            }
            retry_ = 0;
        }
        diag_.kind = kind_;
        diag_.timing = timing_;
        diag_.status = expire() ? Status::Active : Status::Failed;
    }
};

} // namespace loot
