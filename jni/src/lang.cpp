#include "lang.h"

#include <cstring>

namespace lang {
namespace {

struct LangPair { const char* ru; const char* en; };

// Таблицы переводов лежат отдельным файлом: подписи правятся часто, и держать
// их рядом с кодом переключателя незачем.
#include "lang_tables.inc"

// Таблицы отсортированы по байтам UTF-8 (strcmp), поэтому поиск — бинарный:
// он вызывается на каждую подпись каждый кадр, и линейный перебор 200+ пар
// на кадр был бы заметен. Строку, которой в таблице нет, возвращаем как есть:
// так непереведённая подпись просто останется русской, а не пропадёт.
const char* lookup(const LangPair* table, size_t count, const char* key) {
    if (!key || !key[0]) return key;
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int cmp = strcmp(key, table[mid].ru);
        if (cmp == 0) return table[mid].en;
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return key;
}

Lang g_lang = LANG_RU;

} // namespace

void set(Lang value) { g_lang = (value == LANG_EN) ? LANG_EN : LANG_RU; }
Lang current() { return g_lang; }
bool english() { return g_lang == LANG_EN; }

namespace {

// Подписи, общие у меню и визуалов («Дерево», «Оружие», «Ящики»), лежат в одной
// из двух таблиц. Чтобы у такой строки был один перевод, а не два, которые
// могут разойтись, поиск идёт сначала по своей таблице, а потом по соседней.
const char* lookupBoth(const LangPair* first, size_t firstCount,
                       const LangPair* second, size_t secondCount,
                       const char* key) {
    const char* hit = lookup(first, firstCount, key);
    if (hit != key) return hit;
    return lookup(second, secondCount, key);
}

constexpr size_t kUiCount     = sizeof(kUiEn) / sizeof(kUiEn[0]);
constexpr size_t kVisualCount = sizeof(kVisualEn) / sizeof(kVisualEn[0]);

} // namespace

const char* text(const char* ru) {
    if (!english()) return ru;
    return lookupBoth(kUiEn, kUiCount, kVisualEn, kVisualCount, ru);
}

const char* visual(const char* ru) {
    if (!english()) return ru;
    return lookupBoth(kVisualEn, kVisualCount, kUiEn, kUiCount, ru);
}

} // namespace lang
