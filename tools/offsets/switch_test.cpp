// Стенд переключателя версий (релиз/бета) — без NDK и без устройства.
//
// Проверяет ровно то, что нельзя увидеть глазами: при выбор версии активные
// оффсеты действительно меняются все разом, возврат к релизу возвращает
// прежние значения, а «бета недоступна» действительно ничего не подменяет.
//
//   sh tools/offsets/run.sh
#include <cstdio>
#include <cinttypes>

#include "game_offsets_active.h"

static int fails = 0;

#define CHECK(cond, ...)                              \
    do {                                              \
        if (!(cond)) {                                \
            printf("  [ПРОВАЛ] ");                    \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
            ++fails;                                  \
        }                                             \
    } while (0)

static void check_all_equal(const go::Field* f, std::size_t n, bool beta, const char* what) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t want = beta ? f[i].beta : f[i].release;
        CHECK(*f[i].slot == want,
              "%s: %s = 0x%" PRIX64 ", а ожидалось 0x%" PRIX64,
              what, f[i].name, (std::uint64_t)*f[i].slot, want);
    }
}

int main() {
    std::size_t n = 0;
    const go::Field* f = go::Fields(n);
    printf("полей в таблице переключателя: %zu\n", n);
    printf("бета доступна: %s\n", go::BetaAvailable() ? "да" : "нет");
    printf("  источник: %s\n", go::BetaSource());
    if (!go::BetaAvailable()) printf("  причина:  %s\n", go::BetaReason());

    // Таблица не должна быть пустой: иначе «переключение» — пустышка.
    CHECK(n >= 150, "в таблице всего %zu полей — переключатель не покрывает оффсеты", n);

    // 1. Релиз: активные значения совпадают с релизным заголовком.
    go::SelectBuild(go::Build::Release);
    CHECK(go::CurrentBuild() == go::Build::Release, "после выбора релиза текущая версия — не релиз");
    check_all_equal(f, n, false, "релиз");

    // 2. Бета: значения подменяются, но только если файл беты собран из дампа.
    go::SelectBuild(go::Build::Beta);
    if (go::BetaAvailable()) {
        CHECK(go::CurrentBuild() == go::Build::Beta, "бета доступна, но не выбралась");
        check_all_equal(f, n, true, "бета");
    } else {
        CHECK(go::CurrentBuild() == go::Build::Release,
              "бета недоступна, но версия переключилась — оффсеты могли стать чужими");
        check_all_equal(f, n, false, "бета (недоступна -> должен остаться релиз)");
    }

    // 3. Возврат к релизу — значения обязаны вернуться к релизным.
    go::SelectBuild(go::Build::Release);
    check_all_equal(f, n, false, "возврат к релизу");

    // 4. Хотя бы одно отличие беты от релиза при доступной бете: если её файл
    //    собран из дампа, но совпал с релизом во всём, это почти наверняка
    //    признак, что генератор отработал вхолостую.
    if (go::BetaAvailable()) {
        std::size_t diff = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (f[i].beta != f[i].release) ++diff;
        printf("отличий беты от релиза: %zu из %zu\n", diff, n);
        CHECK(diff > 0, "файл беты собран из дампа, но не отличается от релиза ни одним полем — "
                        "проверь tools/offsets/beta_offsets.py --apply");
    }

    if (fails) {
        printf("\nстенд переключателя версий: ПРОВАЛ (%d)\n", fails);
        return 1;
    }
    printf("\nстенд переключателя версий: ПРОЙДЕН\n");
    return 0;
}
