#pragma once
// Часы ожидания (steady clock) — общий счётчик секунд для таймеров сканов,
// кешей и сторожей кадра ESP.
//
// Было: static double mono_seconds() внутри монолита game.cpp. Стало: inline в
// заголовке, потому что функцию зовут почти все модули ESP, и держать её в
// одном из них — значит платить лишней связью ради одной строки кода.
//
// Монотонные (steady_clock), а не системные: сторож кадра и периоды сканов
// сравнивают разности, и перевод системных часов на устройстве не должен
// выглядеть как «скана не было 6 часов».
#include <chrono>

inline double mono_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}
