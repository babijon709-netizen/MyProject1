#pragma once
// Активные оффсеты: релиз или бета — переключается на ходу.
//
// Зачем так. Игра выходит в двух версиях (релиз и бета), и раскладка структур у
// них разная. Держать два набора констант можно было бы двумя сборками, но
// клиент один, а версия выбирается в меню при запуске. Значит имена констант
// должны остаться прежними (иначе править пришлось бы каждый вызов чтения
// памяти), а значения — стать переменными.
//
// Как устроено:
//
//   game_offsets.h        — релизные значения, эталон (inline constexpr).
//   game_offsets_beta.h   — те же имена со значениями беты; собрано
//                           tools/offsets/beta_offsets.py из dump_beta.7z.
//   game_offsets_active.inc — сгенерированный список всех констант
//                           (GO_FIELD — смещения, GO_CONST — остальное).
//
//   namespace go_active   — те же имена, но переменные; именно их видят
//                           game.cpp/main.cpp (`using namespace go_active;`).
//   go::SelectBuild()     — подставляет значения выбранной версии во все
//                           переменные разом.
//
// Умолчание — релиз: go_active инициализируется релизными значениями, так что
// без единого вызова SelectBuild() поведение и цифры ровно те же, что были до
// появления беты.
//
// Цена: смещения читаются из памяти, а не подставляются в код числом. Один
// ldr на обращение — на фоне сотен process_vm_readv в кадре это шум.

#include <cstdint>
#include <cstddef>

#include "game_offsets.h"

#if defined(__has_include)
#  if __has_include("game_offsets_beta.h")
#    include "game_offsets_beta.h"
#    define GO_HAVE_BETA 1
#  else
#    define GO_HAVE_BETA 0
#  endif
#else
#  define GO_HAVE_BETA 0
#endif

// Файла беты может не быть (её ещё не собирали из дампа) — тогда «бета» это
// релиз: имена берём через using-директиву, а доступность отвечает false.
#if !GO_HAVE_BETA
namespace game_offsets_beta {
inline constexpr bool        kFromDump    = false;
inline constexpr bool        kRvaFromDump = false;
inline constexpr const char* kReason      = "Бета недоступна: файл оффсетов не собран";
inline constexpr const char* kSource      = "файл беты не собран";
using namespace game_offsets;
}
#endif

namespace go_active {

// Энумы и прочие типы зависят не от раскладки памяти, а от кода игры: у релиза
// и беты они совпадают, поэтому просто пробрасываем их в активный набор —
// иначе `using namespace go_active` перестал бы их находить.
using game_offsets::ToolPurpose;
using game_offsets::MineableEntityType;

#define GO_FIELD(name, beta_value) inline std::uint64_t name = game_offsets::name;
#define GO_CONST(name, beta_value) inline float         name = game_offsets::name;
#include "game_offsets_active.inc"
#undef GO_FIELD
#undef GO_CONST

}  // namespace go_active

namespace go {

enum class Build : int { Release = 0, Beta = 1 };

// Таблица переключения: имя (для отчётов), адрес активного значения и значение
// каждой версии. Релизное значение берётся прямо из game_offsets.h, поэтому
// правка релизного заголовка не требует пересборки таблицы — только смены дампа
// беты (её значения вписаны числами в game_offsets_beta.h).
struct Field {
    const char*      name;
    std::uint64_t*   slot;
    std::uint64_t    release;
    std::uint64_t    beta;
};

inline const Field* Fields(std::size_t& count) {
    static const Field kFields[] = {
#define GO_FIELD(name, beta_value) {#name, &go_active::name, \
                                    (std::uint64_t)game_offsets::name, \
                                    (std::uint64_t)(beta_value)},
#define GO_CONST(name, beta_value)
#include "game_offsets_active.inc"
#undef GO_FIELD
#undef GO_CONST
    };
    count = sizeof(kFields) / sizeof(kFields[0]);
    return kFields;
}

inline Build& CurrentRef() {
    static Build b = Build::Release;
    return b;
}

// Бета доступна, только если из её дампа взято ВСЁ, без чего она не работает:
// сами смещения (kFromDump) и RVA классов (kRvaFromDump) — по релизным RVA
// бета-клиент классов не находит, и ESP/аим молча ничего не видят. Такую бету
// лучше не предлагать вовсе, чем показывать пустой экран.
inline bool BetaAvailable() {
    return game_offsets_beta::kFromDump && game_offsets_beta::kRvaFromDump;
}

// Почему бету нельзя выбрать (пустая строка — можно). Показывается в меню вместо
// источника оффсетов: «не собран файл» и «не пересчитаны RVA» лечатся разным.
inline const char* BetaReason() {
#if GO_HAVE_BETA
    return game_offsets_beta::kReason;
#else
    return "Бета недоступна: файл оффсетов не собран";
#endif
}

// Откуда взяты бета-оффсеты (строчка для меню): дамп и его отпечаток.
inline const char* BetaSource() {
#if GO_HAVE_BETA
    return game_offsets_beta::kSource;
#else
    return "файл беты не собран";
#endif
}

inline void SelectBuild(Build b) {
    if (b == Build::Beta && !BetaAvailable()) b = Build::Release;
    std::size_t n = 0;
    const Field* f = Fields(n);
    for (std::size_t i = 0; i < n; ++i)
        *f[i].slot = (b == Build::Beta) ? f[i].beta : f[i].release;
    CurrentRef() = b;
}

inline Build CurrentBuild() { return CurrentRef(); }

}  // namespace go
