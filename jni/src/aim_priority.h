#pragma once

#include <cmath>

// Правило выбора цели из круга FOV. Меньший score выигрывает.
//   0 «Умный»      — прицел и дальность, нормированные, вместе;
//   1 «К прицелу»  — строго ближайшая к перекрестию;
//   2 «Ко мне»     — строго ближайшая по дальности до игрока.
namespace aim_priority {

enum Rule : int { SMART = 0, TO_CROSSHAIR = 1, TO_ME = 2 };

inline bool valid_rule(int rule) { return rule >= SMART && rule <= TO_ME; }

// aimTerm: нормированное отклонение от перекрестия (0 = в центре, 1 = край круга).
// rangeTerm: дальность в единицах 120 м; при неизвестной дальности вызывающий
// передаёт 1.0 (то есть 120 м), чтобы цель без дистанции не тонула в сортировке.
inline float raw_score(int rule, float aimTerm, float rangeTerm) {
    switch (rule) {
        case TO_CROSSHAIR: return aimTerm;
        // Дальность доминирует; угол только разводит цели на одной дистанции.
        case TO_ME:        return rangeTerm * 4.f + aimTerm * 0.05f;
        default:           return aimTerm + rangeTerm * 0.8f;
    }
}

// Множитель score для текущей цели (меньше = сильнее держим).
// Для явных правил (crosshair/me) залипание — только гистерезис против дрожи
// на почти равных целях: 0.35 перебивало правило (цель на 3x дальше всё ещё
// «ближе» за счёт липкости). Для «Умного» оставлено прежнее значение.
inline float sticky_scale(int rule) {
    return rule == SMART ? 0.35f : 0.8f;
}

inline float apply_sticky(int rule, float score, bool sticky) {
    if (!sticky) return score;
    return score * sticky_scale(rule);
}

}  // namespace aim_priority
