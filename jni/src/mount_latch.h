#pragma once
// Защита бокса на транспорте от мерцания «трансформ щёлкнул обратно в точку
// посадки». Вынесено из game.cpp, чтобы решение можно было прогнать на хосте:
// именно здесь 08.10 в 23:09:07 два пассажира одного транспорта (ид=509113)
// ушли в вечное удержание — удержаний=59/с, отстал рос 20.3 м/с до 173 м, пока
// оба не вылетели из реестра.
#include <cmath>

#include "Vector.h"

struct MountLatch {
    int  unmounted_streak = 0;
    int  saved_still = 0;
    bool engaged = false;
    Vec3 last{};
    bool last_ok = false;
    Vec3 prev_saved{};
    bool have_saved = false;
    // Счётчики для лога «бокс отстаёт на транспорте»: сколько раз за окно лога
    // защита удерживала бокс на прежнем месте, сколько кадров подряд она его
    // держит сейчас (аварийный выход) и сколько кадров подряд не было ни
    // одного живого отсчёта позиции.
    int  hold_count = 0;
    int  hold_streak = 0;
    int  miss_streak = 0;
};

namespace mountlatch {

// Оба порога защиты имеют смысл только пока lastSavedPosition заморожен в
// точке посадки: тогда «свежий отсчёт лёг рядом с saved» действительно
// означает щелчок трансформа назад. 8 кадров — тот же порог, по которому
// защёлка вообще engages (pos_mounted).
constexpr int kSavedFrozenFrames = 8;

// Удержание — фильтр одиночного щелчка, а не режим. Без потолка latch.last
// замирает, расстояние до него только растёт, и условие защиты становится
// вечным: latch.last обновляется только когда защита не сработала.
constexpr int kMaxHoldStreak = 20;   // ~0.33 с при 60 fps

inline float horiz2(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

// Свежий отсчёт live уже выбран (have_live == true). Возвращает true, если
// бокс оставлен на прежнем месте; feet и состояние защёлки меняются на месте.
// saved_still — сколько кадров подряд lastSavedPosition не двигался.
inline bool apply_hold(MountLatch& latch, const Vec3& saved, const Vec3& live,
                       float live_h2, int saved_still, Vec3& feet) {
    latch.miss_streak = 0;
    if (!latch.last_ok) {
        latch.last = live;
        latch.last_ok = true;
        latch.hold_streak = 0;
        feet = live;
        return false;
    }

    const float last_h2 = horiz2(latch.last, saved);
    const bool frozen = saved_still >= kSavedFrozenFrames;
    bool guard = false;
    // Rendered transform snapped back to the frozen boarding point.
    if (frozen && live_h2 < 1.25F * 1.25F && last_h2 > 2.5F * 2.5F) {
        guard = true;
    }
    // Same flicker, but the bad sample did not land exactly on lastSaved.
    else if (frozen && last_h2 > 4.0F && horiz2(live, latch.last) > 4.0F * 4.0F &&
             live_h2 < last_h2 * 0.25F) {
        guard = true;
    }

    if (guard && latch.hold_streak >= kMaxHoldStreak) guard = false;

    if (guard) {
        ++latch.hold_count;
        ++latch.hold_streak;
        feet = latch.last;
        return true;
    }
    latch.last = live;
    latch.last_ok = true;
    latch.hold_streak = 0;
    feet = live;
    return false;
}

// Отставание бокса от свежего отсчёта в метрах, как его печатает лог.
inline float lag_meters(const MountLatch& latch, const Vec3& live) {
    return std::sqrt(horiz2(latch.last, live));
}

}  // namespace mountlatch
