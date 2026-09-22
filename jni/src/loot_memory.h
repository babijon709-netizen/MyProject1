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
#include <cstring>

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
                                                "Cysharp.Threading.Tasks.UniTask",
                                                ""};   // вложенный тип: пусто
inline constexpr const char* kRunnerName     = "AsyncUniTask`1";
inline constexpr const char* kRunnerNs[]     = {"Cysharp.Threading.Tasks.CompilerServices",
                                                "Cysharp.Threading.Tasks",
                                                ""};   // generic-инстанция: пусто
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
    // Две инструкции задержки (`mov w0, #1` — флаг-аргумент, и
    // `bl WaitForSeconds`) — опознавательные знаки потока: их значения
    // читаются, но НИКОГДА не изменяются. Отличающееся содержимое — чужая
    // правка образа (хук, модифицированный клиент): поток отключается,
    // ничего поверх чужого не пишем.
    uint64_t flag_site, call_site;
    uint32_t flag_word, call_word;
};
struct Layout {
    std::array<uint8_t, 20> build_id;
    uint64_t helper_typeinfo;
    // Детект окна через DI: TypeInfo-слоты Zenject.ProjectContext (статики
    // хранят инстанс контекста → контейнер) и класса текста id окон (161
    // статический WindowId = указатели строк; ключи словарей открытых окон
    // совпадают с ними ПО УКАЗАТЕЛЮ, содержимое строк не читается).
    uint64_t projctx_typeinfo;
    uint64_t ids_typeinfo;
    const char* iface_name;     // интерфейс сервиса окон в ключе биндинга (ns "")
    const char* service_name;   // класс реализации сервиса окон (ns "")
    FlowSite flow[3];
};
// Run() of every AsyncUniTask`1 instantiation: take &stateMachine at runner+0x20
// and branch into MoveNext. The four fixed words plus the branch target prove the
// table when it is derived from an image (tools/loot/find_table.py,
// check_native.py). On the device the build-id already pins the table to the
// bytes, so a differing body is reported (Diagnostics::hooked) rather than
// refused: identification rests on class names and the delegate's method
// pointer, not on Run's instructions.
inline constexpr uint32_t kRunPrologue[4] = {0xf9401028u, 0x91008000u,
                                             0xf9406108u, 0xf9404101u};
inline constexpr uint32_t kBranchMask = 0xFC000000u;   // unconditional b
inline constexpr uint32_t kBranchOp   = 0x14000000u;
inline constexpr Layout kLayouts[] = {
    // release, libil2cpp.so build-id d85e63cd8dcbd481695012ef2e284f0eeb9ef307
    {{{0xd8,0x5e,0x63,0xcd,0x8d,0xcb,0xd4,0x81,0x69,0x50,
       0x12,0xef,0x2e,0x28,0x4f,0x0e,0xeb,0x9e,0xf3,0x07}},
     0xD9C52E0,
     0xD9C57A8,      // Zenject.ProjectContext
     0xD9CC4B0,      // ms: статические id окон (инстанс окна стоит в статиках)
     "mu", "mj",
     {{0xB403D10, 0x5B5C5BC, 0x5B5CAD4, 0xBD495100u, 0x5B5CACC, 0x5B5CAE4, 0x52800020u, 0x959F3174u},   // xw  loot window
      {0xB399854, 0x666147C, 0x6661768, 0xBD495100u, 0x6661760, 0x6661778, 0x52800020u, 0x95731E4Fu},   // eDE pickup zone
      {0xB403428, 0x5B5CE7C, 0x5B5D154, 0xBD495100u, 0x5B5D14C, 0x5B5D164, 0x52800020u, 0x959F2FD4u}}}, // xv  fast load
    // beta, libil2cpp.so build-id e10795349bd7c2cdc7d8527597c64395a4efed79
    {{{0xe1,0x07,0x95,0x34,0x9b,0xd7,0xc2,0xcd,0xc7,0xd8,
       0x52,0x75,0x97,0xc6,0x43,0x95,0xa4,0xef,0xed,0x79}},
     0xD9A3488,
     0xD9A3930,      // Zenject.ProjectContext
     0xD9AA7D0,      // qn: статические id окон
     "qK", "qo",
     {{0xB3E1170, 0x5AF47EC, 0x5AF4D04, 0xBD400100u, 0x5AF4CFC, 0x5AF4D14, 0x52800020u, 0x95A05A57u},   // vB  loot window
      {0xB330434, 0x65C3884, 0x65C3B70, 0xBD400100u, 0x65C3B68, 0x65C3B80, 0x52800020u, 0x95751EBCu},   // REd pickup zone
      {0xB3E1A58, 0x5AF5100, 0x5AF53D8, 0xBD400100u, 0x5AF53D0, 0x5AF53E8, 0x52800020u, 0x95A058A2u}}}, // vu  fast load
};
inline constexpr size_t kFlowCount = sizeof(kLayouts[0].flow) / sizeof(kLayouts[0].flow[0]);

// Детект окна через Zenject DI. Все смещения выведены только из дампов и
// образов обоих билдов (FAST_LOOT.md §Детект окна), чтение, записи нет.
// Классы Zenject.PlayerContext/DiContainer в образе необфусцированы и
// раскладка одинакова release↔beta; имена сервиса обфусцированы, поэтому
// лежат в таблице раскладок, а совпадение окон с id — по УКАЗАТЕЛЮ строки
// (статики id-класса), не по содержимому, которое игрой зашифровано.
inline constexpr uint64_t   kDiProjCtxInstance  = 0x20; // ProjectContext statics: _instance
inline constexpr uint64_t   kDiProjCtxContainer = 0x60; // ProjectContext: _container
inline constexpr uint64_t   kDiContainerProviders = 0x18; // DiContainer: _providers
inline constexpr uint64_t   kDiDictEntries   = 0x18;    // Dictionary<*,*>: _entries
inline constexpr uint64_t   kDiDictCount     = 0x20;    // Dictionary<*,*>: _count (int32)
inline constexpr uint64_t   kDiArrayLen      = 0x18;    // managed array: length (int32)
inline constexpr uint64_t   kDiArrayData     = 0x20;    // managed array: элементы
// Dictionary<BindingId, List<ProviderInfo>>: entry {hash,s32; next,s32;
// BindingId{type*,ident*}; value*}; typeof(BindingId) берётся из ключа.
inline constexpr uint64_t   kDiBindStride    = 0x20;
inline constexpr uint64_t   kDiBindKeyType   = 0x08;    // BindingId._type (System.Type*)
inline constexpr uint64_t   kDiBindValue     = 0x18;    // List<ProviderInfo>*
inline constexpr uint64_t   kDiTypeImpl      = 0x10;    // System.Type: _impl.value → Il2CppType*
inline constexpr uint64_t   kDiListItems     = 0x10;    // List<T>: _items
inline constexpr uint64_t   kDiListSize      = 0x18;    // List<T>: _size (int32)
inline constexpr uint64_t   kDiProviderInfo  = 0x20;    // ProviderInfo.Provider
inline constexpr uint64_t   kDiProvInstances = 0x18;    // CachedProvider._instances (List<object>)
// Dictionary<WindowId, Entry*>: entry {hash,s32; next,s32; WindowId{string*}; value*}
inline constexpr uint64_t   kDiWinStride     = 0x18;
inline constexpr uint64_t   kDiWinKey        = 0x08;    // встроенный WindowId = указ. строки
inline constexpr uint64_t   kDiServiceDictA  = 0x50;    // открытые корневые окна
inline constexpr uint64_t   kDiServiceDictB  = 0x58;    // открытые оверлейные окна
inline constexpr uint32_t   kDiIdsCount      = 161;     // id окон в статиках id-класса
inline constexpr uint32_t   kDiMaxBindings   = 4096;
// Имена id окон: WindowId = value-структура из одного поля _Id (System_String*).
// Статика id-класса лежит в образе нулями — содержимое строк живёт только в
// управляемой памяти процесса, поэтому имена читаются только на устройстве и
// только по живым ключам словаря открытых окон. В статическую таблицу имён мы
// не верим: сначала снимаем подписи открытых окон в диагностику.
inline constexpr uint64_t   kDiStrLen        = 0x10;    // System.String: length (int32)
inline constexpr uint64_t   kDiStrData       = 0x14;    // System.String: chars (UTF-16)
inline constexpr uint32_t   kDiNameMaxLen    = 32;      // больше не верим: обычно "loot"
inline constexpr uint32_t   kDiMaxNames      = 8;       // столько имён выносим наружу
inline constexpr uint32_t   kDiScanFrames    = 30;      // частота перечитывания словарей
inline constexpr uint32_t   kDiResolveFrames = 60;      // повтор поиска сервиса

// Bounds. A scan is bounded three ways (runners, items per runner, items per
// scan) because the game mutates loopItems under its own lock while we read it:
// a lying tail or a grown array must not turn into unbounded work.
inline constexpr uint32_t kMaxRunners  = 64;
// Сколько последних элементов одного раннера просматривается за проход.
// `tail` — это upper bound, а не число живых элементов: завершившиеся оставляют
// в массиве дыры, и в долгой сессии список растёт. Просмотр идёт с конца, поэтому
// за пределами окна остаётся старьё, а не свежее ожидание переноса.
inline constexpr uint32_t kMaxItems    = 1024;
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

inline uint32_t float_bits(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

template<class IO, size_t N>
bool named_one_of(IO& io, uint64_t klass, const char* name, const char* const (&ns)[N]) {
    for (size_t i = 0; i < N; ++i) if (io.klass_named(klass, name, ns[i])) return true;
    return false;
}

// Проверка одного потока. Жёсткие только два случая: точку задержки изменил
// прежний патч (значит поток уже «ускорен» чужой правкой инструкций, а duration
// там, скорее всего, ноль) и код вообще не читается. Отличающееся тело `Run` —
// не отказ: build-id уже привязал таблицу к образу, принадлежность ожидания
// доказывается именами классов и указателем метода делегата, а не инструкциями
// `Run`, а единственная запись идёт в поле `elapsed` объекта кучи. Чужой хук или
// модифицированный клиент таким образом не выключают подбор, но и не прячутся.
enum class FlowCheck { Ok, Unreadable, DelayPatched, CodePatched, RunModified };

template<class IO>
FlowCheck flow_check(IO& io, uint64_t base, const FlowSite& flow,
                     std::array<uint32_t, 5>& code) {
    uint32_t word = 0;
    if (!io.read(base + flow.delay_site, word)) return FlowCheck::Unreadable;
    if (word != flow.delay_word) return FlowCheck::DelayPatched;
    // Опознавательные знаки задержки: только оригинал. Любое отличие — чужая
    // правка образа (хук, мод): поток отключается, ничего поверх не пишем —
    // записей в код у этого контроллера нет вообще.
    uint32_t flag = 0, call = 0;
    if (!io.read(base + flow.flag_site, flag) || !io.read(base + flow.call_site, call))
        return FlowCheck::Unreadable;
    if (flag != flow.flag_word || call != flow.call_word) return FlowCheck::CodePatched;
    code = {};
    if (!io.read(base + flow.run, code)) return FlowCheck::Unreadable;
    bool same = true;
    for (int i = 0; i < 4; ++i)
        if (code[i] != kRunPrologue[i]) same = false;
    if ((code[4] & kBranchMask) != kBranchOp) {
        same = false;
    } else {
        const int32_t imm = (static_cast<int32_t>(code[4] << 6)) >> 6;   // imm26
        const int64_t target = static_cast<int64_t>(base + flow.run) +
                               static_cast<int64_t>(imm) * 4;
        if (target != static_cast<int64_t>(base + flow.move_next)) same = false;
    }
    return same ? FlowCheck::Ok : FlowCheck::RunModified;
}

struct ImageCheck {
    const Layout* layout = nullptr;
    uint32_t flows = 0;      // потоки, которые можно использовать
    uint32_t hooked = 0;     // из них — с изменённым телом Run
    uint64_t hooked_run = 0;
    std::array<uint32_t, 5> hooked_code{};
    Reason reason = Reason::Image;
    uint64_t detail = 0;
};

// Образ принимается по build-id; `flows` собирает потоки, у которых точка
// задержки цела. layout == nullptr, если build-id не из таблицы или не уцелел
// ни один поток; `reason`/`detail` несут первый отказ и число к нему.
template<class IO> ImageCheck check_image(IO& io, uint64_t base) {
    ImageCheck out;
    out.detail = 0;
    if (base < 0x10000) return out;
    std::array<uint8_t, 20> id{};
    if (!io.read(base + kBuildIdOffset, id)) return out;
    uint64_t head = 0;
    for (int i = 7; i >= 0; --i) head = (head << 8) | id[static_cast<size_t>(i)];
    out.detail = head;                    // first 8 build-id bytes, little endian
    for (const auto& layout : kLayouts) {
        if (layout.build_id != id) continue;
        out.detail = 0;
        Reason first = Reason::None;
        uint64_t first_detail = 0;
        for (size_t f = 0; f < kFlowCount; ++f) {
            std::array<uint32_t, 5> code{};
            const FlowCheck check = flow_check(io, base, layout.flow[f], code);
            if (check == FlowCheck::Ok || check == FlowCheck::RunModified) {
                out.flows |= 1u << f;
                if (check == FlowCheck::RunModified) {
                    out.hooked |= 1u << f;
                    if (!out.hooked_run) { out.hooked_run = layout.flow[f].run; out.hooked_code = code; }
                }
                continue;
            }
            if (first == Reason::None) {
                first = (check == FlowCheck::DelayPatched || check == FlowCheck::CodePatched)
                            ? Reason::Patched : Reason::Code;
                first_detail = check == FlowCheck::DelayPatched  ? layout.flow[f].delay_site
                             : check == FlowCheck::CodePatched   ? layout.flow[f].call_site
                                                                 : layout.flow[f].run;
            }
        }
        if (!out.flows) { out.reason = first; out.detail = first_detail; return out; }
        out.layout = &layout;
        out.reason = Reason::None;
        return out;
    }
    return out;
}

template<class IO> class Controller {
    IO& io;
    uint64_t session_ = 0, base_ = 0;
    const Layout* layout_ = nullptr;
    // Какие потоки переноса прошли проверку кода (bit i — Kind i): поток с чужим
    // патчем отключается сам, остальные продолжают работать.
    uint32_t flows_ok_ = 0;
    // Потоки с изменённым телом Run: не отказ, а показ в меню.
    uint32_t hooked_ = 0;
    uint64_t hooked_run_ = 0;
    std::array<uint32_t, 5> hooked_code_{};
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
    // Детект окна: найденный сервис окон (Zenject DI), блок статиков id-класса
    // окон и расписание сканирований. Всё только читается.
    uint64_t di_service_ = 0;
    uint64_t di_ids_ = 0;                 // статики id-класса окон (161 указ.)
    unsigned di_resolve_ = 0, di_scan_ = 0;
    Diagnostics diag_{};

    // Только чтение с отказом вместо догадки: любой неизвестный указатель
    // или выход за бюджет — код отказа в диагностику, сервис не принимается.
    bool di_read(uint64_t addr, uint64_t& v) {
        v = io.ptr(addr);
        return v >= 0x10000;
    }
    bool di_read_i32(uint64_t addr, int32_t& v) {
        return io.read(addr, v);
    }

    // Найти инстанс сервиса окон через контейнер:
    //   ProjectContext._instance -> _container -> _providers:
    //   Dictionary<BindingId, List<ProviderInfo>> с ключом BindingId.Type ==
    //   интерфейс сервиса; единственный провайдер — CachedProvider (singleton
    //   project-биндинга), его первый инстанс и есть сервис. Провайдер другой
    //   формы или класс не тот — отказ, ничего не берём на веру.
    bool di_resolve() {
        uint64_t klass = 0, statics = 0, instance = 0, container = 0, providers = 0;
        if (!di_read(base_ + layout_->projctx_typeinfo, klass)) { diag_.di_fail = 1; return false; }
        if (!di_read(klass + 0xB8, statics)) { diag_.di_fail = 1; return false; }
        diag_.di_stage = 1;
        if (!di_read(statics + kDiProjCtxInstance, instance)) { diag_.di_fail = 2; return false; }
        if (!di_read(instance + kDiProjCtxContainer, container)) { diag_.di_fail = 3; return false; }
        diag_.di_stage = 2;
        if (!di_read(container + kDiContainerProviders, providers)) { diag_.di_fail = 4; return false; }
        int32_t count = 0;
        if (!di_read_i32(providers + kDiDictCount, count) || count < 1 ||
            static_cast<uint32_t>(count) > kDiMaxBindings) { diag_.di_fail = 5; return false; }
        uint64_t entries = 0;
        if (!di_read(providers + kDiDictEntries, entries)) { diag_.di_fail = 5; return false; }
        int32_t capacity = 0;
        if (!di_read_i32(entries + kDiArrayLen, capacity) || capacity < 1 ||
            static_cast<uint32_t>(capacity) > kDiMaxBindings * 4) { diag_.di_fail = 5; return false; }
        diag_.di_stage = 3;
        diag_.di_fail = 6;                 // интерфейс не найден, если цикл не выйдет
        int32_t visited = 0;
        for (int32_t i = 0; i < capacity && visited < count; ++i) {
            int32_t hash = 0;
            const uint64_t entry = entries + kDiArrayData + static_cast<uint64_t>(i) * kDiBindStride;
            if (!di_read_i32(entry, hash) || hash < 0) continue;   // свободная запись
            ++visited;
            uint64_t type_ = 0;
            if (!di_read(entry + kDiBindKeyType, type_)) continue;
            const uint64_t tclass = io.ptr(io.ptr(type_ + kDiTypeImpl));
            if (tclass < 0x10000) continue;
            if (!io.klass_named(tclass, layout_->iface_name, "")) continue;
            // Нашёлся биндинг сервиса. Ровно один провайдер, формы CachedProvider.
            uint64_t list = 0;
            if (!di_read(entry + kDiBindValue, list)) { diag_.di_fail = 7; return false; }
            uint64_t items = 0;
            if (!di_read(list + kDiListItems, items)) { diag_.di_fail = 7; return false; }
            int32_t size = 0;
            if (!di_read_i32(list + kDiListSize, size) || size != 1) { diag_.di_fail = 7; return false; }
            const uint64_t info = io.ptr(items + kDiArrayData);
            uint64_t provider = 0;
            if (info < 0x10000 || !di_read(info + kDiProviderInfo, provider)) { diag_.di_fail = 7; return false; }
            if (!io.klass_named(io.ptr(provider), "CachedProvider", "Zenject")) { diag_.di_fail = 8; return false; }
            uint64_t insts = 0, iitems = 0;
            if (!di_read(provider + kDiProvInstances, insts)) { diag_.di_fail = 8; return false; }
            if (!di_read(insts + kDiListItems, iitems)) { diag_.di_fail = 8; return false; }
            int32_t isize = 0;
            if (!di_read_i32(insts + kDiListSize, isize) || isize < 1) { diag_.di_fail = 8; return false; }
            const uint64_t svc = io.ptr(iitems + kDiArrayData);
            if (svc < 0x10000) { diag_.di_fail = 9; return false; }   // ещё не инстанцирован
            if (!io.klass_named(io.ptr(svc), layout_->service_name, "")) { diag_.di_fail = 10; return false; }
            uint64_t ids_klass = 0, ids = 0;
            if (!di_read(base_ + layout_->ids_typeinfo, ids_klass)) { diag_.di_fail = 11; return false; }
            if (!di_read(ids_klass + 0xB8, ids)) { diag_.di_fail = 11; return false; }
            di_service_ = svc;
            di_ids_ = ids;
            diag_.di_stage = 4;
            diag_.di_fail = 0;
            return true;
        }
        return false;
    }

    // Строка имени уже заполнена вызывающим; оставляем её, только если имя
    // целиком ASCII-печатное. Иначе делаем её пустой — скан пропустит имя.
    static void di_name_sanitize(char* out, int32_t len) {
        if (len < 1 || static_cast<uint32_t>(len) > kDiNameMaxLen) { out[0] = 0; return; }
        for (int32_t i = 0; i < len; ++i)
            if (out[i] < 0x20 || out[i] > 0x7e) { out[0] = 0; return; }
        out[len] = 0;
    }

    // Посчитать открытые окна в одном словаре: сколько записей живо, сколько
    // их ключей не входит в таблицу известных id (такого быть не должно), и
    // выгрузить первые kDiMaxNames имён ключей с их индексами в статиках.
    bool di_scan_dict(uint64_t dict, uint32_t& open, uint32_t& unknown,
                      bool overlay) {
        open = unknown = 0;
        if (dict < 0x10000) return false;
        int32_t count = 0;
        if (!di_read_i32(dict + kDiDictCount, count) || count < 0 ||
            static_cast<uint32_t>(count) > 256) return false;
        uint64_t entries = 0;
        if (!di_read(dict + kDiDictEntries, entries)) return false;
        int32_t capacity = 0;
        if (!di_read_i32(entries + kDiArrayLen, capacity) || capacity < count ||
            capacity > 1024) return false;
        int32_t visited = 0;
        for (int32_t i = 0; i < capacity && visited < count; ++i) {
            int32_t hash = 0;
            const uint64_t entry = entries + kDiArrayData + static_cast<uint64_t>(i) * kDiWinStride;
            if (!di_read_i32(entry, hash) || hash < 0) continue;
            ++visited;
            const uint64_t key = io.ptr(entry + kDiWinKey);
            int32_t known_at = -1;
            for (uint32_t k = 0; k < kDiIdsCount; ++k)
                if (io.ptr(di_ids_ + k * 8) == key) { known_at = static_cast<int32_t>(k); break; }
            if (known_at < 0) ++unknown;
            else if (diag_.di_names_n < kDiMaxNames) {
                // Имя ключа: UTF-16 в управляемой строке процесса; точный id —
                // его индекс в статиках id-класса (таблица имён намеренно не
                // зашита, см. FAST_LOOT.md §«Детект окна через DI»).
                char name[kDiNameMaxLen + 1];
                int32_t len = 0;
                name[0] = 0;
                if (io.read_fresh(key + kDiStrLen, len) && len > 0 &&
                    static_cast<uint32_t>(len) <= kDiNameMaxLen) {
                    for (int32_t c = 0; c < len; ++c) {
                        uint16_t wc = 0;
                        if (!io.read_fresh(key + kDiStrData + static_cast<uint64_t>(c) * 2, wc))
                            { name[0] = 0; break; }
                        name[c] = static_cast<char>(wc & 0xff);
                    }
                    name[len] = 0;
                    di_name_sanitize(name, len);
                }
                if (name[0]) {
                    const uint32_t slot = diag_.di_names_n++;
                    std::memcpy(diag_.di_names[slot].data(), name, static_cast<size_t>(len) + 1);
                    diag_.di_idindex[slot] = known_at;
                    diag_.di_overlay_win[slot] = overlay ? 1 : 0;
                }
            }
            ++open;
        }
        return true;
    }

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

    bool runners_ready(Reason& reason, uint64_t& detail) {
        if (runners_) return true;
        // Длина массива IL2CPP — 32 бита: так её читает весь проект и так её
        // читает сам игрок (`ldr w9,[x8,#0x18]` в PlayerLoopHelper.AddAction).
        // 8-байтовое чтение взяло бы мусор из соседних байт и отбрило образ.
        reason = Reason::Helper;
        const uint64_t klass = io.ptr(base_ + layout_->helper_typeinfo);
        detail = klass;
        if (klass < 0x10000 || !named_one_of(io, klass, kHelperName, kHelperNs)) return false;
        const uint64_t statics = io.ptr(klass + kClassStaticFields);
        if (statics < 0x10000) return false;
        reason = Reason::Runners;
        const uint64_t array = io.ptr(statics + kHelperRunners);
        detail = array;
        if (array < 0x10000) return false;
        int32_t length = 0;
        if (!io.read(array + go_active::IL2CPP_ARRAY_LENGTH, length)) return false;
        detail = static_cast<uint64_t>(static_cast<uint32_t>(length));
        if (length <= 0 || length > static_cast<int32_t>(kMaxRunners)) return false;
        runners_ = array;
        runners_len_ = static_cast<uint32_t>(length);
        reason = Reason::None;
        detail = 0;
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
            if ((flows_ok_ & (1u << i)) && entry == base_ + layout_->flow[i].run)
                found = static_cast<int>(i);
        if (found < 0) {
            // method_code carries the same code address on some delegate shapes.
            if (!io.read_fresh(state + kDelegateMethodCode, entry)) return false;
            for (size_t i = 0; i < kFlowCount; ++i)
                if ((flows_ok_ & (1u << i)) && entry == base_ + layout_->flow[i].run)
                    found = static_cast<int>(i);
        }
        if (found < 0) return false;
        const uint64_t continuation = io.ptr_fresh(item + kPromiseCore + kCoreContinuation);
        const uint64_t target = io.ptr_fresh(state + go_active::DELEGATE_TARGET);
        if (continuation < 0x10000 || target < 0x10000) return false;
        const uint64_t klass = io.ptr_fresh(target);
        if (klass < 0x10000 || !named_one_of(io, klass, kRunnerName, kRunnerNs)) return false;
        // Завершившийся await: TryReturn зануляет duration/elapsed одной записью
        // xzr (образ: str xzr,[obj,#0x18] на выходе в пул), а bound() этого не
        // видит. Такой объект ещё кадр-два валяется в списке раннера с целым
        // делегатом — не цепляемся за него повторно.
        float duration = 0.f, elapsed = 0.f;
        if (!io.read_fresh(item + kPromiseDuration, duration) ||
            !io.read_fresh(item + kPromiseElapsed, elapsed)) return false;
        if (!viewmodel::finite(duration) || duration < kMinDelay || duration > kMaxDelay) return false;
        if (!viewmodel::finite(elapsed) || elapsed < 0.f) return false;
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
                int32_t capacity = 0, tail = 0;
                if (items < 0x10000 || !io.read(items + go_active::IL2CPP_ARRAY_LENGTH, capacity) ||
                    capacity <= 0 || !io.read(object + kRunnerTail, tail) || tail <= 0) continue;
                uint64_t count = static_cast<uint64_t>(tail);
                if (count > static_cast<uint64_t>(capacity)) count = static_cast<uint64_t>(capacity);
                // Окно просмотра — последние kMaxItems записей, с конца к началу:
                // AddAction дописывает ожидание в хвост, а уплотнение в Run
                // переносит последние живые элементы на место завершённых,
                // поэтому свежее ожидание переноса почти всегда ближе к концу.
                // Обрезается старьё, а не хвост, в каком бы длинном списке он ни
                // оказался — иначе ожидание за пределами окна не нашлось бы.
                const uint64_t begin = count > kMaxItems ? count - kMaxItems : 0;
                for (uint64_t i = count; i > begin && budget; --i) {
                    --budget;
                    const uint64_t item = io.ptr(items + go_active::IL2CPP_ARRAY_FIRST_ELEMENT +
                                                 (i - 1) * 8);
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
    bool expire(Reason& reason, uint64_t& detail) {
        reason = Reason::Read;
        detail = promise_;
        float duration = 0.f, elapsed = 0.f;
        if (!io.read_fresh(promise_ + kPromiseDuration, duration) ||
            !io.read_fresh(promise_ + kPromiseElapsed, elapsed)) return false;
        reason = Reason::Timer;
        detail = (static_cast<uint64_t>(float_bits(elapsed)) << 32) | float_bits(duration);
        if (!viewmodel::finite(duration) || duration < kMinDelay || duration > kMaxDelay) return false;
        if (!viewmodel::finite(elapsed) || elapsed < 0.f) return false;
        reason = Reason::None;
        detail = 0;
        if (elapsed >= duration) return true;   // already due: resumes this frame
        reason = Reason::Write;
        detail = promise_ + kPromiseElapsed;
        if (!io.write_heap(promise_ + kPromiseElapsed, duration)) return false;
        reason = Reason::None;
        detail = 0;
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
        flows_ok_ = hooked_ = 0;
        hooked_run_ = 0;
        hooked_code_ = {};
        runners_ = 0; runners_len_ = 0;
        loop_runner_klass_ = promise_klass_ = known_klass_ = 0;
        memo_ = {};
        memo_next_ = 0;
        name_budget_spent_ = false;
        drop();
        retry_ = hot_ = 0;
        di_service_ = di_ids_ = 0;
        di_resolve_ = di_scan_ = 0;
        diag_ = {};
    }

    // Детект окна: независимый информационный канал в диагностику. Никогда
    // не влияет на status/flows и не пишет.
    void di_update() {
        if (!layout_) { diag_.di_stage = 0; diag_.di_fail = 0; return; }
        if (!di_service_) {
            if (di_resolve_) { --di_resolve_; return; }
            di_resolve_ = kDiResolveFrames;
            if (!di_resolve()) return;
        }
        if (di_scan_) { --di_scan_; return; }
        di_scan_ = kDiScanFrames;
        diag_.di_names_n = 0;              // имена переснимаются целиком раз в скан
        uint32_t a = 0, b = 0, ka = 0, kb = 0;
        const uint64_t da = io.ptr(di_service_ + kDiServiceDictA);
        const uint64_t dbb = io.ptr(di_service_ + kDiServiceDictB);
        const bool oka = di_scan_dict(da, a, ka, false);
        const bool okb = di_scan_dict(dbb, b, kb, true);
        if (!oka && !okb) { diag_.di_fail = 12; return; }   // сервис умер: не обнуляем указатель сразу
        diag_.di_root = oka ? a : 0;
        diag_.di_overlay = okb ? b : 0;
        diag_.di_unknown = (oka ? ka : 0) + (okb ? kb : 0);
        diag_.di_stage = 5;
        diag_.di_fail = 0;
        ++diag_.di_scans;
    }

    void tick(uint64_t session, uint64_t base, bool enabled) {
        if (session != session_ || base != base_) { reset(); session_ = session; base_ = base; }
        if (!enabled) {
            // Stop touching the game at once. A delay expired on the previous
            // tick is the game's own pending transfer; there is nothing to undo.
            drop();
            retry_ = hot_ = 0;
            // Включение перезапускает детект окна сразу, без кадров разогрева.
            di_scan_ = di_resolve_ = 0;
            diag_.status = Status::Off;
            diag_.kind = Kind::None;
            diag_.reason = Reason::None;
            diag_.detail = 0;
            diag_.flows = 0;
            diag_.hooked = 0;
            diag_.timing = -1;
            diag_.di_stage = 0;
            diag_.di_fail = 0;
            diag_.di_root = diag_.di_overlay = diag_.di_unknown = 0;
            return;
        }
        diag_.status = Status::Waiting;
        diag_.kind = Kind::None;
        diag_.reason = Reason::None;
        diag_.detail = 0;
        diag_.flows = flows_ok_;
        diag_.hooked = hooked_;
        diag_.hooked_run = hooked_run_;
        diag_.hooked_code = hooked_code_;
        diag_.timing = -1;
        if (!base) return;
        if (!layout_) {
            const ImageCheck image = check_image(io, base);
            if (!image.layout) {
                diag_.status = Status::Failed;
                diag_.reason = image.reason;
                diag_.detail = image.detail;
                return;
            }
            layout_ = image.layout;
            flows_ok_ = image.flows;
            hooked_ = image.hooked;
            hooked_run_ = image.hooked_run;
            hooked_code_ = image.hooked_code;
            diag_.flows = flows_ok_;
            diag_.hooked = hooked_;
            diag_.hooked_run = hooked_run_;
            diag_.hooked_code = hooked_code_;
        }
        diag_.flows = flows_ok_;
        di_update();
        {
            Reason reason = Reason::None;
            uint64_t detail = 0;
            if (!runners_ready(reason, detail)) {
                diag_.status = Status::Failed;
                diag_.reason = reason;
                diag_.detail = detail;
                return;
            }
        }
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
        {
            Reason reason = Reason::None;
            uint64_t detail = 0;
            const bool done = expire(reason, detail);
            if (!done && reason == Reason::Timer) {
                // Это не отказ функции, а хвост её же работы: корутина дошла до
                // конца, TryReturn занулил пару float'ов (str xzr,[obj,#0x18] на
                // выходе в пул), а объект ещё висит в списке раннера. Bound() на
                // гонку не успевает: отцепляемся и пересканируем на следующем
                // кадре, тоста «не применено» здесь быть не должно.
                drop();
                diag_.status = Status::Waiting;
                diag_.reason = Reason::None;
                diag_.detail = 0;
                return;
            }
            diag_.status = done ? Status::Active : Status::Failed;
            diag_.reason = reason;
            diag_.detail = detail;
        }
    }
};

} // namespace loot
