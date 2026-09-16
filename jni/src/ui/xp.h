// ui/xp.h — строки интерфейса: XS_RU (русская строка как в коде) и XS
// (строка на текущем языке). Разрезано из jni/src/main.cpp.
//
// Зачем шифрование: русские подписи меню — это и есть строки, по которым в
// бинарнике видно, что за софт. Каждая хранится как XOR с ключом от номера
// символа и расшифровывается один раз на место вызова (static constexpr).
//
//   XS("Настройки")    — подпись меню на текущем языке (РУ по умолчанию);
//   XS_RU("Разное")    — русская строка как в коде: для ключей таблиц
//                        переводов и имён вкладок, которые не переводятся.

#pragma once

#include "lang.h"      // lang::text — подстановка языка в XS

#include <cstddef>
#include <cstdint>

namespace xp {

static constexpr uint64_t _ks0 = 0xC3A1F72B9D4E058BULL;
static constexpr uint64_t _ks1 = 0x2B7E3D48F1C96A30ULL;

static constexpr uint8_t _xk(size_t i) noexcept {
    uint64_t s = _ks0 ^ (_ks1 * (i + 1));
    s ^= s >> 33; s *= 0xff51afd7ed558ccdULL;
    s ^= s >> 33; s *= 0xc4ceb9fe1a85ec53ULL;
    s ^= s >> 33;
    return static_cast<uint8_t>(s & 0xFF);
}

template<size_t N>
struct _XS {
    char b[N]{};
    mutable char o[N]{};
    mutable bool done = false;
    constexpr _XS(const char (&s)[N]) noexcept {
        for (size_t i = 0; i < N; ++i) b[i] = static_cast<char>(static_cast<uint8_t>(s[i]) ^ _xk(i));
    }
    __attribute__((noinline)) const char* d() const noexcept {
        // Decode once and reuse: every UI label goes through here every
        // frame, and re-XORing ~125 strings each frame was pure waste.
        if (!done) {
            for (size_t i = 0; i < N; ++i) o[i] = static_cast<char>(static_cast<uint8_t>(b[i]) ^ _xk(i));
            done = true;
        }
        return o;
    }
};
template<size_t N> constexpr auto _mk(const char (&s)[N]) noexcept { return _XS<N>(s); }

}

// Русская строка ровно так, как она написана в коде (декодируется один раз на
// место вызова). Нужна там, где перевод не должен применяться: имена вкладок
// для счётчиков ширины и строка-ключ таблицы переводов.
#define XS_RU(s) ([]() noexcept -> const char* { static constexpr auto _x = xp::_mk(s); return _x.d(); }())

// Строка интерфейса на текущем языке (РУ по умолчанию, EN — по выбору в
// «Опциях»). Язык меняется на ходу, поэтому подстановка идёт здесь, а не в
// статических массивах — иначе меню осталось бы на языке запуска.
#define XS(s) (lang::text(XS_RU(s)))
