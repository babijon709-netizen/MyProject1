// Хост-тест таблицы имён оружия (jni/src/weapon_name.h).
//
// Проверяется ровно то, из-за чего были жалобы: имя префаба обязано превращаться
// в человеческую подпись на обоих языках, а неизвестное оружие — оставаться
// вычищенным именем, а не исчезать. Чтение памяти игры (какой именно объект
// лежит в руках) здесь недоступно и не проверяется.
#include "weapon_name.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace weaponname;

namespace {

// Подпись после канонизации в выбранном языке. Буфер с запасом: строки таблицы
// — UTF-8, и резать их по байту нельзя (за этим и CopyTextUtf8).
const char* label_of(const char* raw, bool english) {
    static char buf[96];
    snprintf(buf, sizeof(buf), "%.90s", raw);
    canonicalize(buf, sizeof(buf), english);
    return buf;
}

}  // namespace

int main() {
    // -- Двустволка: имя префаба «DoubleBarrel» раньше уходило в интерфейс как есть.
    assert(!strcmp(label_of("DoubleBarrel", false), "Двустволка"));
    assert(!strcmp(label_of("DoubleBarrel", true), "Double Barrel Shotgun"));
    assert(!strcmp(label_of("shotgun.double", false), "Двустволка"));
    assert(!strcmp(label_of("07_DefaultDoubleBarrel (Clone)", false),
                   label_of("DoubleBarrel", false)));   // decoration скина не важно

    // -- Тот же ствол из трёх источников читается одинаково.
    assert(!strcmp(label_of("AssaultRifle", false), label_of("assault.rifle", false)));
    assert(!strcmp(label_of("AssaultRiffle", false), label_of("AssaultRifle", false)));

    // -- «Арбалет»: более длинный ключ обязан выигрывать у «bow», иначе арбалет
    //    подписывался луком.
    assert(!strcmp(label_of("Crossbow", false), "Арбалет"));
    assert(!strcmp(label_of("CustomCrossbow", false), "Арбалет"));
    assert(!strcmp(label_of("HuntingBow", false), "Охотничий лук"));
    assert(!strcmp(label_of("Bow", false), "Лук"));

    // -- У оружия без русской пары подпись берётся английская, а не пустая.
    assert(!strcmp(label_of("MBR7", false), "MBR7"));
    assert(!strcmp(label_of("MBR7", true), "MBR7"));

    // -- Неизвестное оружие остаётся как есть: пустая подпись хуже кривой.
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "MysteryGun");
        assert(!canonicalize(buf, sizeof(buf), false));
        assert(!strcmp(buf, "MysteryGun"));
    }

    // -- Ключи таблицы обязаны быть в нормализованной форме (только [a-z0-9]),
    //    иначе строка никогда не совпадёт точно и молча деградирует до подстроки.
    for (const Name& entry : kNames) {
        char norm[80];
        normalize(entry.key, norm, sizeof(norm));
        assert(!strcmp(norm, entry.key));
        assert(entry.key[0] && entry.en && entry.en[0]);
    }

    // -- Предметы «в руках помимо оружия»: именно они подменяли подпись, когда
    //    оставались ребёником холдера раньше настоящего ствола.
    assert(is_held_utility("Torch"));
    assert(is_held_utility("01_DefaultTorch"));
    assert(is_held_utility("Flashlight"));
    assert(is_held_utility("Medkit"));
    assert(is_held_utility("Building Plan"));
    assert(!is_held_utility("Crossbow"));
    assert(!is_held_utility("AssaultRifle"));
    assert(!is_held_utility(""));
    assert(!is_held_utility("Shotgun"));

    std::printf("test_weapon_name: ok\n");
    return 0;
}
