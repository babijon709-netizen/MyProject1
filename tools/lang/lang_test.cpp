// Стенд переключателя языка: линкует НАСТОЯЩИЙ jni/src/lang.cpp (с таблицами
// jni/src/lang_tables.inc) и проверяет то, чего не видно на глаз в коде меню.
//
//   sh tools/lang/run.sh
//
// Что проверяется:
//   * русский — по умолчанию, и он же возвращает исходную строку;
//   * переключение на английский подменяет и строки меню, и подписи визуалов;
//   * неизвестная строка не пропадает, а остаётся как есть (важно для подписей
//     предметов, которых нет в таблице имён: пустой подписи в ESP быть не должно);
//   * бинарный поиск реально находит все пары таблиц, а не только середину —
//     проверка по каждой паре обеих таблиц (иначе ошибка сортировки таблицы
//     вылезла бы только на устройстве, и только у части подписей);
//   * пустая строка и nullptr не роняют поиск (вызывается из отрисовки кадра).

#include "lang.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lang_test_tables.inc"   // kUiEn/kVisualEn — те же пары, что в сборке

static int g_fail = 0;

static void check(bool ok, const char* what) {
    if (!ok) { printf("  ПРОВАЛ: %s\n", what); ++g_fail; }
}

static void expectEq(const char* got, const char* want, const char* what) {
    if (!got || strcmp(got, want) != 0) {
        printf("  ПРОВАЛ: %s — получили «%s», ждали «%s»\n", what,
               got ? got : "(null)", want);
        ++g_fail;
    }
}

int main() {
    // ---- русский по умолчанию ------------------------------------------------
    check(lang::current() == lang::LANG_RU, "язык по умолчанию — русский");
    check(!lang::english(), "english() по умолчанию false");
    expectEq(lang::text("Противники"), "Противники", "русский не подменяется");
    expectEq(lang::visual("Дерево"), "Дерево", "подписи визуалов в русском как были");

    // ---- переключение на английский -----------------------------------------
    lang::set(lang::LANG_EN);
    check(lang::english(), "english() после переключения");
    expectEq(lang::text("Противники"), "Enemies", "пункт меню переведён");
    expectEq(lang::text("Опции"), "Options", "вкладка переведена");
    expectEq(lang::text("Тёмная тема"), "Dark theme", "строка настроек переведена");
    expectEq(lang::text("%.0f м"), "%.0f m", "единицы измерения в формате переведены");
    expectEq(lang::visual("Дерево"), "Wood", "ресурс переведён");
    expectEq(lang::visual("АК-47"), "AK-47", "оружие переведено именем игры");
    expectEq(lang::visual("Шипованная дубина"), "Wooden Spiked Club",
             "оружие из таблицы игры (en) переведено");
    expectEq(lang::visual("Дигл"), "Deagle", "короткая подпись по id предмета переведена");
    // Общая подпись: и пункт меню, и маркер ESP берут один перевод.
    expectEq(lang::text("Ящики"), "Crates", "общая подпись меню");
    expectEq(lang::visual("Ящики"), "Crates", "общая подпись визуала");

    // ---- то, чего нет в таблице, остаётся как есть ---------------------------
    expectEq(lang::visual("Магический ящик"), "Магический ящик",
             "неизвестная подпись не пропадает");
    expectEq(lang::text(""), "", "пустая строка возвращается как есть");
    check(lang::text(nullptr) == nullptr, "nullptr не попадает в поиск");

    // ---- обе таблицы находятся целиком (бинарный поиск по сортировке) --------
    // Ошибка сортировки таблицы не видна по одной строке: бинарный поиск
    // промахивается мимо части пар, и меню выглядит наполовину переведённым.
    // Поэтому здесь сверяются ВСЕ пары обеих таблиц.
    int reached = 0, total = 0;
    for (const auto& p : kUiEn) {
        ++total;
        const char* got = lang::text(p.ru);
        if (got && p.en && strcmp(got, p.en) == 0) { ++reached; continue; }
        printf("  ПРОВАЛ: меню «%s» -> «%s», а ждали «%s»\n", p.ru, got ? got : "(null)", p.en);
        ++g_fail;
    }
    for (const auto& p : kVisualEn) {
        ++total;
        const char* got = lang::visual(p.ru);
        if (got && p.en && strcmp(got, p.en) == 0) { ++reached; continue; }
        printf("  ПРОВАЛ: визуал «%s» -> «%s», а ждали «%s»\n", p.ru, got ? got : "(null)", p.en);
        ++g_fail;
    }
    check(reached == total, "поиск находит все пары таблиц");

    // ---- возврат на русский --------------------------------------------------
    lang::set(lang::LANG_RU);
    expectEq(lang::text("Противники"), "Противники", "возврат на русский");
    expectEq(lang::visual("Дерево"), "Дерево", "визуалы вернулись на русский");

    printf("пар в таблицах: меню %d, визуалов %d; найдено поиском %d\n",
           (int)(sizeof(kUiEn) / sizeof(kUiEn[0])),
           (int)(sizeof(kVisualEn) / sizeof(kVisualEn[0])), reached);
    if (g_fail) { printf("ПРОВАЛОВ: %d\n", g_fail); return 1; }
    printf("язык: все проверки пройдены\n");
    return 0;
}
