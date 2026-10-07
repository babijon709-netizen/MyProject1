#pragma once
// Состав боксов на кадр: единственное место, где решается, кого рисовать.
//
// Задача, под которую это написано (жалоба 07.10: «у некоторых еспы то не
// работают, то пропадают вообще», и трупы остаются на экране):
//
//  * бокс убитого игрока обязан пропасть в тот же кадр;
//  * бокс вышедшего с сервера — тоже (его просто нет в реестре);
//  * одиночный сбой чтения у ОДНОГО игрока не должен гасить его бокс;
//  * но и «залипнуть» бокс не имеет права ни при каком сбое.
//
// Отсюда ровно два правила, без эвристик:
//
//  1. Здоровый кадр (реестр разобран) — источник правды. Наружу уходит ровно
//     то, что отдал конвейер. Ничего не удерживается: смерть и выход с сервера
//     видны в первый же здоровый кадр.
//  2. Рваный кадр (сбой чтения, а не факт ухода) — состав доводится до свежего
//     по id, а те, кто не пришёл, живут ещё hold_seconds от своего последнего
//     ЗДОРОВОГО появления. Дальше уходят. Верхняя граница по времени, а не
//     «до следующего здорового кадра» — именно безусловное удержание прошлого
//     полного снимка и оставляло трупы на экране вечно.
//
// Ключ записи — EspBox::id (PlayerManager*), поэтому запись никогда не
// «передаётся» другому игроку, переиспользовавшему адрес.
#include <cstddef>
#include <vector>
#include "game.h"

namespace espboxhold {

struct Entry {
    EspBox box{};
    double last_seen = 0.0;   // время последнего ЗДОРОВОГО появления
};

// hold — рабочее состояние между кадрами, out — снимок для отрисовки.
// Оба принадлежат вызывающему: функции не держат статики, поэтому проверяются
// хост-тестом (jni/tests/test_esp_box_hold.cpp).
inline void update(std::vector<Entry>& hold,
                   const std::vector<EspBox>& fresh,
                   bool transient,
                   double now,
                   double hold_seconds,
                   std::vector<EspBox>& out) {
    if (!transient) {
        // Правило 1. Состав — ровно конвейерный.
        hold.clear();
        hold.reserve(fresh.size());
        for (const EspBox& box : fresh) {
            Entry entry;
            entry.box = box;
            entry.last_seen = now;
            hold.push_back(entry);
        }
    } else {
        // Правило 2. Доводим до свежего по id, остальное держим по времени.
        for (const EspBox& box : fresh) {
            bool merged = false;
            for (Entry& entry : hold) {
                if (entry.box.id != box.id) continue;
                entry.box = box;
                entry.last_seen = now;
                merged = true;
                break;
            }
            if (merged) continue;
            Entry entry;
            entry.box = box;
            entry.last_seen = now;
            hold.push_back(entry);
        }
        for (std::size_t index = 0; index < hold.size();) {
            if (now - hold[index].last_seen > hold_seconds)
                hold.erase(hold.begin() + (std::ptrdiff_t)index);
            else
                ++index;
        }
    }

    out.clear();
    out.reserve(hold.size());
    for (const Entry& entry : hold) out.push_back(entry.box);
}

// То же для маркеров: у них нет стабильного id в общем случае, поэтому
// удержание идёт целиком по времени — снимок живёт не дольше hold_seconds от
// последнего здорового кадра. Отдельная функция, чтобы у маркеров и боксов
// правило было одинаковым и проверяемым.
inline bool marker_snapshot_is_fresh(double last_healthy_at, double now, double hold_seconds) {
    return now - last_healthy_at <= hold_seconds;
}

}  // namespace espboxhold
