#pragma once
// Авто-нажатие «Забрать всё»: логика без ввода-вывода и без записи памяти.
//
// Решение принято так, чтобы функция работала без патча библиотек, без
// дешифровки строк игры и без таблиц координат: точка нажатия САМО-КАЛИБРУЕТСЯ
// по реальному жесту игрока. Когда пользователь один-два раза сам нажал «Забрать
// всё» (при открытии лут-окна кнопка переноса штурмирует state-machine
// переноса — ту самую корутину, которую мы иначе ускоряем), координатная
// точка жеста запоминается; после подтверждения автоподгход: при ОТКРЫТИИ
// окна лута синтетический тап летит в ту точку спустя короткую паузу отрисовки.
//
// Вся строгость — в гейтах: детект окна «какое-то root-окно открыто» идёт по
// словарю Zenject-сервиса (лут-окно в этот список входит как root), а
// мошеннические нажатия отбрасываются тем, что
//   (а) огонь один раз за открытие окна;
//   (б) не огонь, пока перенос уже идёт (поток активен);
//   (в) не огонь без подтверждённой точки;
//   (г) пауза между окнами;
//   (д) отключение тумблером мгновенно поднимает синтетический палец.
//
// При неверной точке самое страшное — один касание по безобидному участку
// UI раз за открытие окна: приемлемый боевой-иск риск.

#include <cstdint>

namespace loot_tap {

inline constexpr int kLearnRadiusPx    = 40;   // жесты подтверждения должны быть рядом
inline constexpr int kLearnEvents      = 2;    // подтверждений точки до вооружения
inline constexpr int kTapDelayFrames   = 6;    // ~100 мс — окно дорисуется
inline constexpr int kTapDownFrames    = 3;    // длина удержания синтетического тапа
inline constexpr int kTapCooldown      = 150;  // ~2.5 с: никогда не тапает чаще
inline constexpr int kLearnGapFrames   = 16;   // тап должен предшествовать потоку не более чем на столько кадров
inline constexpr int kMaxRecentTaps    = 8;

struct Diagnostics {
    int state = 0;                 // 0 — нет точки, 1 — есть, подтверждённая
    float point_x = 0, point_y = 0;
    uint64_t learn_events = 0;     // сколько жестов игрока пошло в усреднение
    uint64_t taps = 0;             // сколько синтетических нажатий сделано
    uint64_t gated = 0;            // сколько раз огонь блокировали гейты
    uint64_t window_opens = 0;     // сколько открытий окна увидено
};

// Inject — две функции, которые делает адаптер (Touch_Down_N / Touch_Up_N).
struct Inject {
    void (*down)(float x, float y) = nullptr;
    void (*up)() = nullptr;
};

class Tap {
public:
    void reset() {
        point_x_ = point_y_ = 0.f;
        confirmed_ = false;
        learn_sum_x_ = learn_sum_y_ = 0.f;
        learn_count_ = 0;
        prev_window_ = prev_flow_ = false;
        window_open_frame_ = 0;
        fired_window_open_ = 0;
        down_left_ = 0;
        injecting_ = false;
        tap_frame_ = tap_x_ = tap_y_ = 0;          // последний жест игрока (undefined = 0)
        have_last_tap_ = false;
        diag_ = Diagnostics{};
    }

    const Diagnostics& diagnostics() const { return diag_; }

    // Кадр: root_windows — результат DI-детекта (-1 = детект-цепочка сломана,
    // gating: ведёт себя как «окна нет»); collect_window_flow — жив ли поток
    // переноса с открытого окна (диагностика контроллера); tap_x/tap_y — жест
    // ИГРОКА именно на этом кадре (0,0=false; кадровый репит удержания не
    // считается жестом) в координатах inject; frame — номер кадра что тикаем.
    void on_frame(bool enabled, int root_windows, bool collect_window_flow,
                  bool user_tap_edge, float tap_x, float tap_y,
                  uint64_t frame, const Inject& ij) {
        const bool window_open = root_windows > 0;
        if (!enabled) {
            if (injecting_) { if (ij.up) ij.up(); injecting_ = false; }
            down_left_ = 0;
            prev_window_ = window_open;
            prev_flow_ = collect_window_flow;
            have_last_tap_ = false;
            return;
        }
        // Рисунок открытия окна: зафиксировать номер кадра только на переднем фронте.
        if (window_open && !prev_window_) {
            window_open_frame_ = frame;
            ++diag_.window_opens;
        }
        if (user_tap_edge) {
            tap_frame_ = frame; tap_x_ = tap_x; tap_y_ = tap_y;
            have_last_tap_ = true;
        }
        // Калибровка: передний фронт потока переноса при открытом окне, и жест
        // был незадолго до него.
        if (collect_window_flow && !prev_flow_ && window_open && have_last_tap_ &&
            frame - tap_frame_ <= static_cast<uint64_t>(kLearnGapFrames)) {
            if (learn_count_ == 0) {
                learn_sum_x_ = tap_x_; learn_sum_y_ = tap_y_;
                learn_count_ = 1;
                point_x_ = tap_x_; point_y_ = tap_y_;   // кандидат на точку
            } else {
                const float cx = learn_sum_x_ / learn_count_;
                const float cy = learn_sum_y_ / learn_count_;
                const int dx = static_cast<int>(tap_x_ - cx < 0 ? cx - tap_x_ : tap_x_ - cx);
                const int dy = static_cast<int>(tap_y_ - cy < 0 ? cy - tap_y_ : tap_y_ - cy);
                if (dx < kLearnRadiusPx && dy < kLearnRadiusPx) {
                    learn_sum_x_ += tap_x_; learn_sum_y_ += tap_y_;
                    ++learn_count_;
                } else {
                    // Совсем другое место (например настоящий пункт меню окна):
                    // не подходим под калибровку кнопки — пересчёт с этой точки.
                    learn_sum_x_ = tap_x_; learn_sum_y_ = tap_y_;
                    learn_count_ = 1;
                    point_x_ = tap_x_; point_y_ = tap_y_;
                    confirmed_ = false;
                }
            }
            if (learn_count_ >= kLearnEvents) {
                point_x_ = learn_sum_x_ / learn_count_;
                point_y_ = learn_sum_y_ / learn_count_;
                confirmed_ = true;
            }
            diag_.state = confirmed_ ? 1 : 0;
            diag_.point_x = point_x_;
            diag_.point_y = point_y_;
            ++diag_.learn_events;
            have_last_tap_ = false;
        }
        // Синтетический тап: завершить удержание приоритетно.
        if (down_left_ > 0) {
            if (--down_left_ == 0 && injecting_) { if (ij.up) ij.up(); injecting_ = false; }
        }
        // Кандидат на огонь: окно дорисовалось, точка подтверждена, в это открытие
        // ещё не стреляли, поток не идёт, кулдаун выдержан.
        const bool can_fire =
            confirmed_ && window_open &&
            frame >= window_open_frame_ + static_cast<uint64_t>(kTapDelayFrames) &&
            window_open_frame_ != fired_window_open_ &&
            !collect_window_flow && down_left_ == 0 &&
            frame - last_tap_frame_ >= static_cast<uint64_t>(kTapCooldown);
        if (can_fire) {
            if (ij.down) ij.down(point_x_, point_y_);
            injecting_ = true;
            down_left_ = kTapDownFrames;
            fired_window_open_ = window_open_frame_;
            ++diag_.taps;
            last_tap_frame_ = frame;
        } else if (confirmed_ && window_open &&
                   frame >= window_open_frame_ + static_cast<uint64_t>(kTapDelayFrames) &&
                   window_open_frame_ != fired_window_open_ &&
                   down_left_ == 0) {
            ++diag_.gated;
        }
        prev_window_ = window_open;
        prev_flow_ = collect_window_flow;
    }

private:
    float point_x_ = 0.f, point_y_ = 0.f;
    bool confirmed_ = false;
    float learn_sum_x_ = 0.f, learn_sum_y_ = 0.f;
    int learn_count_ = 0;
    bool prev_window_ = false, prev_flow_ = false;
    uint64_t window_open_frame_ = 0;
    uint64_t fired_window_open_ = 0;
    int down_left_ = 0;
    bool injecting_ = false;
    uint64_t tap_frame_ = 0;
    float tap_x_ = 0.f, tap_y_ = 0.f;
    bool have_last_tap_ = false;
    uint64_t last_tap_frame_ = 0;
    Diagnostics diag_{};
};

} // namespace loot_tap
