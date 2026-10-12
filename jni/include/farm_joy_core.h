#pragma once
// Логика выбора рабочего слоя ввода для памяти-джойстика автофарма: без
// обращений к памяти, чтобы её можно было прогнать на хосте (см.
// jni/tests/test_farm_joy_core.cpp). Память живёт в farm_joy.cpp, здесь —
// только решение «пробуем этот слой / он подтвердился / берём следующий».
//
// Как решается. Пока стик ведём ненулевой магнитудой и бот идёт к цели
// «чисто» (фаза подхода, курс вперёд), запускается проба длиной trial_time.
// За пробу смотрим дистанцию до точки подхода:
//   сблизились на trial_good и больше — слой подтверждён (закрепляем его);
//   ушли от цели на trial_back — слой ведёт не туда: помечаем отвергнутым,
//     чтобы бот не уезжал от цели;
//   иначе — просто провал, берём следующий доступный слой.
// Отвергнутые пропускаются; когда отвергнуты все — начинаем круг заново
// (состояние игры могло измениться: переспавн, другой режим управления).

#include <cmath>

namespace farmjoy {

struct JoyTrialParams {
    float trial_time = 0.45f;   // длина пробы, с
    float trial_good = 0.12f;   // сближение, достаточное для подтверждения, м
    float trial_back = 0.35f;   // уход от цели, после которого слой отвергается, м
    float clean_min  = 2.00f;   // ближе точки удара сближение меряется иначе, м
};

class TrialState {
public:
    enum Decision {
        NONE = 0,     // проба ещё идёт или условий для неё нет
        CONFIRMED,    // слой подтверждён сближением
        NEXT,         // слой не подтвердился: обнулить его и взять следующий
    };

    TrialState(int count, const JoyTrialParams& params = JoyTrialParams())
        : count_(count < 1 ? 1 : (count > kMaxCount ? kMaxCount : count)), params_(params) {
        for (int i = 0; i < kMaxCount; ++i) available_[i] = true;
    }

    // Стик ведём (и с какой магнитудой: strong — не меньше половины хода).
    void set_armed(bool armed, bool strong) {
        armed_ = armed;
        strong_ = strong;
    }

    // Какие слои вообще доступны (резолв сошёлся). Остальные пропускаются.
    void set_available(int candidate, bool ok) {
        if (candidate >= 0 && candidate < count_) available_[candidate] = ok;
    }

    void release() {
        armed_ = false;
        strong_ = false;
        trial_ = 0.0f;
        d0_ = 0.0f;
        dmin_ = 0.0f;
    }

    // Обратная связь кадра: dt — секунды, walk_dist — метры до точки подхода,
    // clean — «ходьба чистая» (фаза подхода, курс вперёд, не манёвр обхода).
    Decision observe(float dt, float walk_dist, bool clean) {
        const bool usable = clean && std::isfinite(walk_dist) && walk_dist > 0.0f;
        if (!usable) {
            trial_ = 0.0f;
            return NONE;
        }
        if (!armed_ || !strong_ || confirmed_ || walk_dist <= params_.clean_min) {
            trial_ = 0.0f;
            dmin_ = walk_dist;
            return NONE;
        }
        if (trial_ <= 0.0f) {
            d0_ = walk_dist;
            dmin_ = walk_dist;
        }
        trial_ += dt > 0.0f ? dt : 0.0f;
        if (walk_dist < dmin_) dmin_ = walk_dist;
        if (trial_ < params_.trial_time) return NONE;

        const float gain = d0_ - dmin_;         // сколько прошли к цели за пробу
        const float back = walk_dist - d0_;     // насколько ушли от неё к концу пробы
        trial_ = 0.0f;
        ++trials_;
        if (gain >= params_.trial_good) {
            confirmed_ = true;
            return CONFIRMED;
        }
        if (back >= params_.trial_back) rejected_[cur_] = true;   // ведёт не туда
        ++fails_[cur_];
        last_from_ = cur_;
        cur_ = next_candidate(cur_);
        d0_ = walk_dist;
        dmin_ = walk_dist;
        return NEXT;
    }

    int  current() const { return cur_; }
    bool armed() const { return armed_; }
    // Откуда ушли на последнем NEXT (кому обнулять память).
    int  last_from() const { return last_from_; }
    bool confirmed() const { return confirmed_; }
    int  trials() const { return trials_; }
    int  fails(int candidate) const {
        return (candidate >= 0 && candidate < count_) ? fails_[candidate] : 0;
    }
    bool rejected(int candidate) const {
        return (candidate >= 0 && candidate < count_) ? rejected_[candidate] : false;
    }

private:
    static const int kMaxCount = 16;

    int next_candidate(int from) const {
        for (int i = 1; i <= count_; ++i) {
            const int c = (from + i) % count_;
            if (!rejected_[c] && available_[c]) return c;
        }
        // Все отвергнуты — снимаем метки и идём по кругу со следующего.
        for (int i = 1; i <= count_; ++i) {
            const int c = (from + i) % count_;
            if (available_[c]) return c;
        }
        return from;   // доступных нет вовсе — остаёмся, где были
    }

    int      count_ = 1;
    JoyTrialParams params_;
    int      cur_ = 0;
    int      last_from_ = 0;
    bool     armed_ = false;
    bool     strong_ = false;
    bool     confirmed_ = false;
    bool     available_[kMaxCount] = {};
    bool     rejected_[kMaxCount] = {};
    int      fails_[kMaxCount] = {};
    int      trials_ = 0;
    float    trial_ = 0.0f;
    float    d0_ = 0.0f;
    float    dmin_ = 0.0f;
};

} // namespace farmjoy
