#pragma once
//
// Драйвер тач-аима: палец ходит ЦЕЛЫМИ СВАЙПАМИ, а не по чуть-чуть каждый такт.
//
// Зачем так. Прежний контроллер выдавал шаг каждый такт оверлея: доля остатка
// (k ~ 0.3) считалась от ошибки, которую камера ещё не отработала. Труба
// касания Android — 2-8 кадров ИГРЫ (на 12-15 фпс это до 0.5 с), поэтому за
// время полёта одного шага успевало уйти ещё несколько, и к моменту отклика
// камера получала сумму шагов больше остатка. Дальше контроллер видел перелёт,
// считал его заново и гнал палец обратно: прицел ходил маятником, а сам палец —
// рывками вместо одного движения. Учёт «в полёте» (поглощение по потоку камеры,
// окно приказов, такт с подтверждением) лечил симптомы ровно до следующей
// просадки ФПС, потому что при отсутствии настоящей оси камеры (штатный режим
// на устройстве: esp_aim_camera_angles -> false) подтверждать отклик нечем.
//
// Здесь заложено другое правило: НОВЫЙ СВАЙП НЕ ВЫДАЁТСЯ, ПОКА НЕ ОТРАБОТАН
// ПРЕДЫДУЩИЙ. Отсюда автоматически:
//
//   * палец ходит целыми движениями в одну сторону — «одним свайпом»;
//   * наложения шагов в трубе касания невозможны по построению — маятника нет;
//   * перелёт ограничен: шаг не больше остатка, а промах по коэффициенту
//     исправляется следующим свайпом ПОСЛЕ прилёта предыдущего.
//
// Каждый свайп кладёт свой заказ («на столько-то градусов палец попросил») в
// ОКНО ТРУБЫ и закрывает гейт. Разблокирует его одно из двух:
//
//   1. ОТКЛИК: остаток сдвинулся в сторону цели (сравнение с опорным остатком,
//      записанным при выдаче заказа) и сдвиг прекратился. Копится именно
//      ПРОДВИЖЕНИЕ, а не сумма положительных приращений: сумма набирается на
//      дрожи кости, продвижение — нет. Величину отклика не меряем: заказ в
//      градусах нашей оценки коэффициента, отклик в настоящих.
//   2. ОКНО ТРУБЫ: заказ прожил pipeline_seconds (или 8 тактов игры, если он
//      измерен — на 12 фпс труба в разы длиннее). Переоценить окно безопасно
//      (аим чуть медленнее), недооценить — значит выдать второй свайп поверх
//      недоехавшего, то есть перелёт.
//
// Ждать отклика у идущей цели обязательно: контроль по «остаток не меняется»
// срывался бы там, где цель сама едет. cycle_seconds — минимальный интервал
// между свайпами: при мгновенном отклике палец не должен строчить пачками,
// игра видит в этом дрожь, а не движение.
//
// Отработка свайпа (что закрывает такт раньше гейта): остаток ушёл почти на весь
// заказ либо перескочил ноль заметно в обратную сторону — перелёт от нашего
// шага (порог высокий: при сглаживании ввода камера движется ещё несколько
// кадров после отклика, и «половина» закрыла бы такт раньше времени).
//
// Коэффициент град/px берётся из настройки чувствительности игры (это множитель
// самой игры: верный по знаку и близкий по величине) или из замера по настоящей
// оси камеры, если она читается. Пока он не подтверждён откликом, шаг режется
// (kFirstSwipeFrac): если настройка занижает коэффициент, полный шаг перелетает
// кость. Подтверждённый отклик снимает запас, а систематически короткий (доехало
// меньше заказанного) — поднимает долю выше единицы: коэффициент из настройки
// завышен. Перелёт или перелив отклика, наоборот, опускают её.
//
// Модуль чистый: ни Android, ни состояния меню. Вход — остаток в градусах и
// конфигурация такта, выход — что сделать с пальцем. Поэтому он прогоняется на
// стенде (jni/tests/test_aim_touch_loop.cpp) настоящей петлёй «палец -> камера ->
// остаток» с задержкой трубы, а не копией логики.

#include <cmath>

#include "aim_control.h"

namespace aim_touch {

// Что сделать с пальцем в этом такте.
enum class Action {
    None,   // ничего (палец не нужен или уже не нужен)
    Place,  // опустить палец в (x, y)
    Move,   // вести палец в (x, y)
    Hold,   // держать палец на месте (тач живой)
    Lift,   // поднять палец
};

struct Gains {
    float yaw = 0.f;      // град поворота камеры на пиксель пальца
    float pitch = 0.f;    // отдельно по тангажу: игра может инвертировать ось
    bool  yaw_known = false;
    bool  pitch_known = false;
    // Коэффициент не просто известен из настройки, а ИЗМЕРЕН на этой петле (по
    // повороту камеры или оценщиком aim_learn.h — в штатном режиме обычно нет).
    // По подтверждённому коэффициенту шаг выдаётся на весь остаток сразу; по
    // настройке — с запасом: если она занижает коэффициент, полный шаг перелетает
    // кость и цель приходится догонять обратным движением, то есть «качнуло».
    bool  yaw_verified = false;
    bool  pitch_verified = false;
};

struct Config {
    float point_x = 0.f, point_y = 0.f;              // выбранная точка взгляда, px
    float box_x0 = 0.f, box_x1 = 0.f;                // зона, за которую палец не уводим
    float box_y0 = 0.f, box_y1 = 0.f;
    float units_per_px = 1.f;                        // квант цифровера (единиц на пиксель)
    float speed = 5.f;                               // ползунок скорости 1..10
    float deadband_yaw = 0.f, deadband_pitch = 0.f;  // град: ближе не двигаем
    // Окно трубы касания: заказ, выданный внутри него, ещё в пути и держит гейт.
    // «2-8 кадров игры» это 0.03-0.53 с; по умолчанию — с запасом в худшую
    // сторону (переоценить окно безопасно: аим чуть медленнее; недооценить —
    // выдача второго свайпа поверх недоехавшего, то есть перелёт). Настоящее
    // окно считается как max(pipeline_seconds, 8 тактов игры) — см. tube_window.
    float pipeline_seconds = 0.45f;
    // Минимальный интервал между свайпами: даже при мгновенном отклике палец не
    // должен строчить пачками — игра видит в этом дрожь, а не движение.
    float cycle_seconds = 0.20f;
    // Такт игры (с), если он измерен. Нужен, чтобы окно трубы не оказалось
    // короче реальной задержки на просадке фпс: 0 — неизвестен.
    float game_period_seconds = 0.f;
};

struct Outcome {
    Action action = Action::None;
    float  x = 0.f, y = 0.f;
};

// Оценка коэффициента годится к работе, только если она того же знака, что и
// значение из настройки чувствительности (множитель самой игры — знак оси).
// Замер со сменой знака означает не инверсию оси, а мусор в нём (цель ушла,
// кость мигнула); с таким коэффициентом палец пойдёт от цели, и это ровно тот
// маятник, на который жалуются. Знак оси меняет игрок в настройках — и он же
// виден в m_Sensitivity-выводе.
inline bool gain_trusted(float measured, float fallback) {
    if (!std::isfinite(measured) || !std::isfinite(fallback)) return false;
    if (std::fabs(measured) < 1e-4f || std::fabs(fallback) < 1e-4f) return false;
    return (measured > 0.f) == (fallback > 0.f);
}

// Полоса дрожи: остаток меньше неё — не движение цели, а шум кости. Палец по
// такому остатку не ходит (и переносить его некуда).
inline constexpr float kJitterBand = 0.45f;

// Полоса покоя в квантах цифровера: отклик дробится по пикселям, и шаг в один-два
// кванта камера не отличит от удержания — палец от него только дрожит.
inline constexpr float kNoStepQuanta = 2.5f;

// Полоса покоя по оси, град: мёртвая зона прицела, но не уже дрожи кости и не
// уже kNoStepQuanta квантов цифровера. Она же — критерий «цель на прицеле»:
// внутри неё контроллер не двигает палец и перенос не делает.
inline float rest_band_degrees(float gain, float units_per_px, float deadband) {
    const bool grid_ok = units_per_px >= 0.25f && units_per_px <= 16.f &&
                         std::isfinite(units_per_px);
    const float quantum = std::fabs(gain) * (grid_ok ? 1.f / units_per_px : 1.f);
    float band = std::fmax(deadband, kJitterBand);
    return std::fmax(band, kNoStepQuanta * quantum);
}

// Потолок одного свайпа по градусам: ползунок скорости теперь задаёт РАЗМЕР
// свайпа, а не долю остатка за такт. 1 — короткие свайпы (плавно), 10 — один
// длинный свайп на весь остаток.
inline float swipe_cap_degrees(float speed) {
    if (!(speed >= 1.f)) speed = 1.f;
    if (speed > 10.f) speed = 10.f;
    return 6.0f + 2.2f * (speed - 1.f);
}

class Lock {
public:
    void reset() { *this = Lock{}; }

    bool finger_down() const { return finger_down_; }

    // Остаток в градусах (положительный yaw — цель правее прицела,
    // положительный pitch — выше). dt — время такта.
    Outcome step(float err_yaw, float err_pitch, float dt,
                 const Config& cfg, const Gains& gains) {
        Outcome out;
        if (!(dt > 0.f) || !std::isfinite(dt)) dt = 1.f / 60.f;
        if (dt > 0.25f) dt = 0.25f;
        if (!std::isfinite(err_yaw)) err_yaw = 0.f;
        if (!std::isfinite(err_pitch)) err_pitch = 0.f;
        tick_dt_ = dt;
        since_swipe_ += dt;

        age_orders(dt, tube_window(cfg));
        if (have_prev_err_) credit_orders(err_yaw, err_pitch);
        prev_err_yaw_ = err_yaw; prev_err_pitch_ = err_pitch; have_prev_err_ = true;
        update_bands(cfg, gains);

        if (!finger_down_) {
            out.action = Action::Place;
            out.x = place_x(cfg, err_yaw);
            out.y = place_y(cfg, err_pitch);
            fx_ = out.x; fy_ = out.y;
            finger_down_ = true;
            phase_ = Phase::Placed;
            hold_ticks_ = 0;
            dead_cycles_ = 0;
            clear_orders();
            return out;
        }

        // Такт после опускания: игра должна увидеть касание до сдвига.
        if (phase_ == Phase::Placed) {
            if (hold_ticks_ < 1) {
                ++hold_ticks_;
                out.action = Action::Hold; out.x = fx_; out.y = fy_;
                return out;
            }
            phase_ = Phase::Ready;
        }

        if (phase_ == Phase::Flying) return step_flying(out, err_yaw, err_pitch, cfg);
        return step_ready(out, err_yaw, err_pitch, cfg, gains);
    }

    // --- диагностика (меню/лог/стенд) ---
    int   swipes() const { return swipes_; }
    int   reversals() const { return reversals_; }
    bool  flying() const { return phase_ == Phase::Flying; }
    float last_swipe_px() const { return last_swipe_px_; }
    float last_swipe_deg() const { return last_swipe_deg_; }
    float pending_yaw() const { return pending_yaw_; }
    float pending_pitch() const { return pending_pitch_; }
    int   orders_in_flight() const { return order_n_; }
    float fx() const { return fx_; }
    float fy() const { return fy_; }

private:
    enum class Phase { Idle, Placed, Ready, Flying };

    // Полосы на такт: квант цифровера (единица координаты пальца) и полоса
    // покоя. Полоса покоя = мёртвая зона прицела, но не уже дрожи кости и не
    // уже двух с половиной квантов: остаток такого порядка камерой уже не
    // отработать (шаг в 1-2 пикселя игра не отличит от удержания), а палец от
    // него только дрожит — внешне это «дёргается» у самой цели.
    void update_bands(const Config& cfg, const Gains& gains) {
        const bool grid_ok = cfg.units_per_px >= 0.25f && cfg.units_per_px <= 16.f &&
                             std::isfinite(cfg.units_per_px);
        const float grid_u = grid_ok ? 1.f / cfg.units_per_px : 1.f;
        q_yaw_   = std::fabs(gains.yaw)   * grid_u;
        q_pitch_ = std::fabs(gains.pitch) * grid_u;
        rest_yaw_   = rest_band_degrees(gains.yaw,   cfg.units_per_px, cfg.deadband_yaw);
        rest_pitch_ = rest_band_degrees(gains.pitch, cfg.units_per_px, cfg.deadband_pitch);
    }

    // ---- окно трубы -------------------------------------------------------
    // Каждому заказу — возраст. Живые (моложе окна) считаются «в пути» и держат
    // гейт: следующий свайп не выдаётся, пока заказ не отработает. Возраст
    // важнее отклика: у идущей цели остаток едет вместе с целью, и по нему
    // «доехало / не доехало» не отличить, а по времени — можно.
    //
    // Окно трубы = max(pipeline_seconds, kTubeFrames тактов игры): труба касания
    // меряется в КАДРАХ ИГРЫ (2-8), и на 12 фпс её окно в полтора раза длиннее,
    // чем на 60. Без этой поправки на просадке фпс мы выдавали бы второй свайп,
    // пока первый ещё не дошёл до игры.
    float tube_window(const Config& cfg) const {
        float window = cfg.pipeline_seconds > 0.f ? cfg.pipeline_seconds : 0.45f;
        if (cfg.game_period_seconds > 0.f && std::isfinite(cfg.game_period_seconds)) {
            window = std::fmax(window, kTubeFrames * cfg.game_period_seconds);
        }
        return window;
    }
    void age_orders(float dt, float window) {
        if (!(window > 0.f) || !std::isfinite(window)) window = 0.45f;
        float yaw = 0.f, pitch = 0.f;
        int keep = 0;
        for (int i = 0; i < order_n_; ++i) {
            orders_[i].age += dt;
            if (orders_[i].age > window) {
                // Окно истекло. Если к этому моменту отклик по заказу ещё ИДЁТ
                // (при сглаживании ввода игра отдаёт сдвиг сильно позже окна),
                // ждём его до жёсткого предела: иначе следующий свайп ляжет
                // поверх недоехавшего. Если отклика не было вовсе, ждать нечего —
                // либо труба длиннее окна, либо ввод потерян (палец перехвачен
                // игрой, экран перекрыт), это и есть мёртвый такт.
                if (orders_[i].responded && still_ticks_ < kJitterTicks &&
                    orders_[i].age < kMaxOrderLife) {
                    orders_[keep++] = orders_[i];
                    yaw += orders_[i].yaw;
                    pitch += orders_[i].pitch;
                    continue;
                }
                if (!orders_[i].responded) ++dead_cycles_;
                continue;
            }
            orders_[keep++] = orders_[i];
            yaw += orders_[i].yaw;
            pitch += orders_[i].pitch;
        }
        order_n_ = keep;
        pending_yaw_ = yaw;
        pending_pitch_ = pitch;
    }

    void push_order(float yaw_deg, float pitch_deg) {
        if (order_n_ >= kOrderSlots) {   // окно переполнено — старейший больше не в счёт
            for (int i = 1; i < order_n_; ++i) orders_[i - 1] = orders_[i];
            --order_n_;
        }
        orders_[order_n_++] = Order{yaw_deg, pitch_deg, 0.f, false, ref_err_yaw_, ref_err_pitch_};
        pending_yaw_ += yaw_deg;
        pending_pitch_ += pitch_deg;
    }

    bool orders_empty() const { return order_n_ == 0; }

    void clear_orders() {
        order_n_ = 0;
        pending_yaw_ = pending_pitch_ = 0.f;
        peak_rate_ = 0.f;
        still_ticks_ = 0;
    }

    // Сверка окна с остатком. Остаток — единственный источник правды: если он
    // уже в другую сторону от заказа (или цель на прицеле), значит этот заказ
    // либо доехал, либо уведёт камеру за цель. Держать такой заказ в окне нельзя:
    // именно из-за него контроллер выдавал ОБРАТНЫЙ свайп на пустом остатке —
    // то самое качание. Заказы в сторону остатка остаются и вычитаются.
    void reconcile_orders(float err_yaw, float err_pitch, float rest_yaw, float rest_pitch) {
        if (order_n_ == 0) return;
        if (std::fabs(err_yaw) <= rest_yaw && std::fabs(err_pitch) <= rest_pitch) {
            clear_orders();   // цель на прицеле — в трубе ничего не нужно
            return;
        }
        float yaw = 0.f, pitch = 0.f;
        int keep = 0;
        for (int i = 0; i < order_n_; ++i) {
            // Оси разводятся отдельно: заказ по рысканью может быть ещё в силе,
            // когда по тангажу остаток уже закрыт или ушёл в другую сторону.
            if (orders_[i].yaw != 0.f && orders_[i].yaw * err_yaw <= 0.f) orders_[i].yaw = 0.f;
            if (orders_[i].pitch != 0.f && orders_[i].pitch * err_pitch <= 0.f) orders_[i].pitch = 0.f;
            if (orders_[i].yaw == 0.f && orders_[i].pitch == 0.f) continue;
            orders_[keep++] = orders_[i];
            yaw += orders_[i].yaw;
            pitch += orders_[i].pitch;
        }
        order_n_ = keep;
        pending_yaw_ = yaw;
        pending_pitch_ = pitch;
    }

    // Кредит заказу: отклик пришёл и УЛЁГСЯ — заказ отработал, окно отпускает
    // гейт. Считается не сумма положительных приращений (её набирает дрожь
    // кости), а чистое продвижение от опорного остатка, записанного при выдаче
    // заказа: остаток стал меньше опорного на kCreditDeg — значит, доехало.
    // Величину отклика не меряем: заказ в градусах нашей оценки коэффициента, а
    // отклик приходит в настоящих, и путать их нельзя. Направление не врёт.
    void credit_orders(float err_yaw, float err_pitch) {
        if (order_n_ == 0) {
            peak_rate_ = 0.f;
            still_ticks_ = 0;
            return;
        }
        // Скорость отклика: пока она близка к пику, камера ещё едет (сглаживание
        // ввода растягивает отклик на много тактов). Улеглась — заказ закрыт.
        const float d_yaw = prev_err_yaw_ - err_yaw;
        const float d_pitch = prev_err_pitch_ - err_pitch;
        const float rate = std::fmax(d_yaw, d_pitch);
        if (rate > peak_rate_) peak_rate_ = rate;
        if (rate > 0.f && rate >= kSettlePeakFrac * peak_rate_) still_ticks_ = 0;
        else ++still_ticks_;
        const bool quiet = still_ticks_ >= kJitterTicks;

        float yaw = 0.f, pitch = 0.f;
        int keep = 0;
        for (int i = 0; i < order_n_; ++i) {
            const Order& o = orders_[i];
            const bool done_yaw   = o.yaw   == 0.f ||
                                    o.ref_yaw   - err_yaw   >= kCreditDeg;
            const bool done_pitch = o.pitch == 0.f ||
                                    o.ref_pitch - err_pitch >= kCreditDeg;
            if (done_yaw || done_pitch) dead_cycles_ = 0;   // отклик был — ввод жив
            if (quiet && done_yaw && done_pitch) continue;  // отработал — из окна
            Order o2 = o;
            if (done_yaw)   o2.responded = true;
            if (done_pitch) o2.responded = true;
            orders_[keep++] = o2;
            yaw += o2.yaw;
            pitch += o2.pitch;
        }
        order_n_ = keep;
        pending_yaw_ = yaw;
        pending_pitch_ = pitch;
        if (order_n_ == 0) { peak_rate_ = 0.f; still_ticks_ = 0; }
    }

    // Разбор отклика заказа: сколько от него доехало и что это говорит про ось.
    //   ratio < 0.30  — отклика почти не было: долю не трогаем (путь «ввод потерян»);
    //   ratio ≤ 1.15  — отклик настоящий: ось подтверждена, шаг можно на весь
    //                   остаток (verified) — тогда длинная наводка идёт одним
    //                   движением, а не ступеньками;
    //   ratio > 1.15  — доехало БОЛЬШЕ заказанного: коэффициент из настройки
    //                   чувствительности занижен, полный шаг перелетит цель —
    //                   долю уменьшаем.
    void adapt_gain(float& gain, int& short_cycles, float progress, float ordered,
                    float residual_ordered) {
        if (!(std::fabs(ordered) > 0.1f)) return;
        const float ratio = progress / ordered;
        if (ratio > kMaxCleanResponse) {
            // Доехало больше заказанного: коэффициент из настройки занижен, полный
            // шаг перелетит цель — долю вниз. (Это же и снимает подтверждение.)
            gain = std::fmax(kMinStepFrac, gain * kOverShrink);
            short_cycles = 0;
            return;
        }
        // Остаток на выдаче был заметный, а доехало заметно меньше заказанного —
        // значит шаг мал (коэффициент из настройки завышен). Два таких такта
        // подряд — долю вверх; предел kMaxStepFrac держит перелёт за один шаг.
        if (ratio >= kMinResponse && ratio <= 0.85f &&
            std::fabs(residual_ordered) > kBigResidualDeg) {
            if (++short_cycles >= 2 && gain < kMaxStepFrac) {
                gain = std::fmin(kMaxStepFrac, gain + kFracGrow);
                short_cycles = 0;
            }
        } else {
            short_cycles = 0;
        }
    }

    // Фильтр дрожи: пропускает остаток только устойчивый по знаку.
    static float jitter_gate(float err, int& streak, float band) {
        if (err == 0.f || !std::isfinite(err) || !(band > 0.f)) { streak = 0; return 0.f; }
        const int sign = err > 0.f ? 1 : -1;
        if (std::fabs(err) >= band) streak = (streak * sign > 0) ? streak + sign : sign;
        else streak = 0;
        if (std::fabs(err) >= 2.f * band) return err;   // явный уход — не ждём
        return (streak * sign >= kJitterTicks) ? err : 0.f;
    }

    // Точка постановки пальца: со сдвигом от выбранной точки в сторону,
    // ПРОТИВОПОЛОЖНУЮ остатку (пропорционально остатку). Так у свайпа есть
    // место в нужную сторону: без этого длинная наводка начиналась из центра
    // зоны, упиралась в её край и уходила в «поднять-перенести» — то самое
    // дёрганье. На малом остатке (ведение у цели) сдвига нет.
    static float place_x(const Config& cfg, float err_yaw) {
        return place_axis(cfg.point_x, cfg.box_x0, cfg.box_x1, err_yaw);
    }
    static float place_y(const Config& cfg, float err_pitch) {
        // Ось Y: положительный остаток тангажа — цель выше, палец пойдёт вверх
        // (в меньшие y), значит запас нужен снизу — знак обратный оси X.
        return place_axis(cfg.point_y, cfg.box_y0, cfg.box_y1, -err_pitch);
    }
    static float place_axis(float point, float lo, float hi, float err_deg) {
        if (!(hi > lo) || !std::isfinite(err_deg)) return point;
        const float room = (hi - lo) * 0.5f;
        float t = std::fabs(err_deg) / kPlaceBiasFullDeg;
        if (t > 1.f) t = 1.f;
        float bias = 0.f;
        if (err_deg > 0.f) bias = -room * kPlaceBiasFrac * t;   // остаток вправо/вверх — встаём левее/ниже
        else if (err_deg < 0.f) bias = room * kPlaceBiasFrac * t;
        float v = point + bias;
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        return v;
    }

    // Признак «свайп отработал» по остатку: он ушёл в сторону цели не меньше чем
    // на 85% заказанного. При отсутствии настоящей оси камеры это единственный
    // быстрый выход из такта; при неполном отклике такт закроется по времени.
    static bool progress_done(float progress, float pending) {
        if (pending == 0.f) return true;
        return progress * pending >= kProgressDone * pending * pending;
    }

    Outcome step_flying(Outcome& out, float err_yaw, float err_pitch, const Config& cfg) {
        const float dt = tick_dt_;
        fly_time_ += dt;
        const float prog_yaw   = swipe_err_yaw_   - err_yaw;
        const float prog_pitch = swipe_err_pitch_ - err_pitch;

        // Свайп доехал: остаток ушёл почти на весь заказ, вошёл в полосу покоя,
        // либо перескочил ноль заметно в обратную сторону (перелёт от нашего
        // шага). Плюс жёсткий такт: в полёте дольше cycle_seconds не ждём — у
        // идущей цели «доехал» не наступает никогда, и палец стоял бы, пока
        // камера уезжает.
        const bool flip_yaw = swipe_err_yaw_ * err_yaw < 0.f &&
                              std::fabs(err_yaw) > std::fmax(cfg.deadband_yaw, kJitterBand);
        const bool flip_pitch = swipe_err_pitch_ * err_pitch < 0.f &&
                                std::fabs(err_pitch) > std::fmax(cfg.deadband_pitch, kJitterBand);
        const bool flip = flip_yaw || flip_pitch;
        const bool settled = std::fabs(err_yaw) <= rest_yaw_ &&
                             std::fabs(err_pitch) <= rest_pitch_;
        const bool done = progress_done(prog_yaw, pending_yaw_) &&
                          progress_done(prog_pitch, pending_pitch_) &&
                          (std::fabs(prog_yaw) > kStillEps || std::fabs(prog_pitch) > kStillEps);
        const bool cycle_due = fly_time_ >= (cfg.cycle_seconds > 0.f ? cfg.cycle_seconds : 0.2f);

        if (settled || done || flip || cycle_due) {
            // Отклик: остаток пошёл В СТОРОНУ ЦЕЛИ хотя бы на треть заказанного, но
            // и не больше чем на полтора заказанного. Первое — ось живая, второе —
            // коэффициент из настройки не занижен, полный шаг не перелетит.
            // Такой отклик снимает запас первого свайпа (доля -> 1).
            const float ratio_yaw   = progress_ratio(prog_yaw,   exp_yaw_);
            const float ratio_pitch = progress_ratio(prog_pitch, exp_pitch_);
            if (ratio_yaw >= kMinResponse && ratio_yaw <= kMaxCleanResponse && !flipped_yaw_)
                step_gain_yaw_ = std::fmax(step_gain_yaw_, 1.f);
            if (ratio_pitch >= kMinResponse && ratio_pitch <= kMaxCleanResponse && !flipped_pitch_)
                step_gain_pitch_ = std::fmax(step_gain_pitch_, 1.f);

            // Доля шага по оси: перелёт — шаг был велик, откатываем долю и снова
            // копим доверие. Полный отклик (заказ доехал целиком) три такта
            // подряд — можно шагать смелее. Так настройка чувствительности, от
            // которой взят коэффициент, проверяется по факту, а не на веру, и
            // перелёт ограничен одним неполным шагом вместо качания.
            if (flip_yaw) {
                flipped_yaw_ = true;
                step_gain_yaw_ = std::fmax(kMinStepFrac, step_gain_yaw_ * kFlipShrink);
            } else {
                adapt_gain(step_gain_yaw_, short_cycles_yaw_, prog_yaw, exp_yaw_, swipe_err_yaw_);
            }
            if (flip_pitch) {
                flipped_pitch_ = true;
                step_gain_pitch_ = std::fmax(kMinStepFrac, step_gain_pitch_ * kFlipShrink);
            } else {
                adapt_gain(step_gain_pitch_, short_cycles_pitch_, prog_pitch, exp_pitch_, swipe_err_pitch_);
            }

            // Ввод потерян уже несколько тактов подряд (заказы дожили до конца
            // окна без отклика) — переносим палец, меняем точку касания.
            if (dead_cycles_ >= kDeadCycles) {
                out.action = Action::Lift;
                finger_down_ = false;
                phase_ = Phase::Idle;
                clear_orders();
                dead_cycles_ = 0;
                return out;
            }

            out.action = Action::Hold;
            out.x = fx_; out.y = fy_;
            phase_ = Phase::Ready;
            return out;
        }

        out.action = Action::Hold;
        out.x = fx_; out.y = fy_;
        return out;
    }

    Outcome step_ready(Outcome& out, float err_yaw, float err_pitch,
                       const Config& cfg, const Gains& gains) {
        // У цели: дрожь кости гонять пальцем нельзя. Остаток выше полосы покоя
        // должен продержаться несколько тактов подряд (или сразу перевалить
        // вдвое) — иначе это шум позиции кости, а не уход цели.
        err_yaw   = jitter_gate(err_yaw,   streak_yaw_,   rest_yaw_);
        err_pitch = jitter_gate(err_pitch, streak_pitch_, rest_pitch_);

        // Полоса покоя: мёртвая зона плюс полтора кванта цифровера. Мёртвая зона
        // считается от полпикселя и кванта, но шум кости (десятые доли градуса)
        // её перекрывает: без запаса контроллер гонял бы палец на один пиксель
        // влево-вправо за шумом с частотой кадров — микро-дрожь, которая
        // читается как «дёргается».
        const float q_yaw = q_yaw_, q_pitch = q_pitch_;
        const float rest_yaw = rest_yaw_, rest_pitch = rest_pitch_;

        // Сверка окна с остатком: заказ, который остаток уже не подтверждает,
        // из окна вылетает (см. reconcile_orders).
        reconcile_orders(err_yaw, err_pitch, rest_yaw, rest_pitch);

        // Второй заказ в трубу не кладём: пока предыдущий не отработал (нет
        // отклика по остатку) и пока труба держит его (окно), палец ждёт. Именно
        // наложение шагов давало перелёт и обратный свайп — маятник.
        if (order_n_ > 0 || since_swipe_ < cfg.cycle_seconds) {
            out.action = Action::Hold;
            out.x = fx_; out.y = fy_;
            return out;
        }

        const float want_yaw   = err_yaw;
        const float want_pitch = err_pitch;

        const bool inside = std::fabs(err_yaw) <= rest_yaw &&
                            std::fabs(err_pitch) <= rest_pitch;
        if (inside || (err_yaw == 0.f && err_pitch == 0.f)) {
            out.action = Action::Hold;   // стоим у кости (или ждём прилёта) и держим тач живым
            out.x = fx_; out.y = fy_;
            return out;
        }

        // Град/px. Неизвестен — шаг по этой оси не считаем: цель просто не ведём
        // (лучше стоять, чем угадывать сторону и получить маятник).
        const float gy = gains.yaw_known   && std::fabs(gains.yaw)   > 1e-4f ? gains.yaw   : 0.f;
        const float gp = gains.pitch_known && std::fabs(gains.pitch) > 1e-4f ? gains.pitch : 0.f;

        float dx = 0.f, dy = 0.f;
        if (gy != 0.f) dx =  err_yaw   / gy;
        if (gp != 0.f) dy = -err_pitch / gp;

        // Доля шага по оси. Коэффициент из настройки чувствительности — только
        // оценка: он верен по знаку, но может врать по величине, а полный шаг
        // тогда перелетает цель (обратное движение после перелёта — это и есть
        // «качнуло»). Поэтому шаг режется, пока ось не подтверждена настоящим
        // замером (ось камеры, gains.*_verified), и подстраивается по факту:
        // перелёт или перелив отклика — доля вниз, подтверждённый отклик — до
        // единицы, систематически короткий — выше единицы (коэффициент завышен).
        // Полный шаг — только если ось подтверждена (замером по настоящей оси
        // камеры или собственной петлёй) и перелёта на ней ещё не было. Иначе —
        // накопленная доля: она переживает перелёты и возвращается к единице
        // медленно, по подтверждённым тактам.
        const float fy = gains.yaw_verified   ? 1.f : step_gain_yaw_;
        const float fp = gains.pitch_verified ? 1.f : step_gain_pitch_;
        if (gy != 0.f) dx *= fy;
        if (gp != 0.f) dy *= fp;

        // Потолок свайпа по градусам (ползунок скорости).
        const float cap_deg = swipe_cap_degrees(cfg.speed);
        if (gy != 0.f && std::fabs(dx) * std::fabs(gy) > cap_deg)
            dx = (dx > 0.f ? cap_deg : -cap_deg) / std::fabs(gy);
        if (gp != 0.f && std::fabs(dy) * std::fabs(gp) > cap_deg)
            dy = (dy > 0.f ? cap_deg : -cap_deg) / std::fabs(gp);

        // Поправка против предыдущего свайпа (палец идёт обратно из-за промаха)
        // режется: разгонять её незачем, это не наводка.
        if (have_last_swipe_) {
            if (dx != 0.f && last_swipe_dx_ != 0.f && (dx > 0.f) != (last_swipe_dx_ > 0.f) &&
                std::fabs(err_yaw) > rest_yaw * 4.f) {
                dx *= kFlipDamp;
                ++reversals_;
            }
            if (dy != 0.f && last_swipe_dy_ != 0.f && (dy > 0.f) != (last_swipe_dy_ > 0.f) &&
                std::fabs(err_pitch) > rest_pitch * 4.f) {
                dy *= kFlipDamp;
            }
        }

        // Точная посадка: остаток меньше трёх квантов цифровера — прыгаем в
        // ближайшую достижимую точку. Сглаживание тут только заставляет ползти
        // или перелетать на целый квант.
        if (gy != 0.f && std::fabs(err_yaw) < 3.f * q_yaw) dx = err_yaw / gy;
        if (gp != 0.f && std::fabs(err_pitch) < 3.f * q_pitch) dy = -err_pitch / gp;

        const float dx_wanted = dx, dy_wanted = dy;
        float nx = fx_ + dx, ny = fy_ + dy;
        if (nx < cfg.box_x0) nx = cfg.box_x0;
        if (nx > cfg.box_x1) nx = cfg.box_x1;
        if (ny < cfg.box_y0) ny = cfg.box_y0;
        if (ny > cfg.box_y1) ny = cfg.box_y1;
        nx = snap(nx, cfg.units_per_px);
        ny = snap(ny, cfg.units_per_px);
        dx = nx - fx_;
        dy = ny - fy_;

        if (dx == 0.f && dy == 0.f) {
            // Ноль из-за края зоны (остаток есть, а места в нужную сторону нет) —
            // переносим палец сразу: ждать здесь целый такт нечего. Ноль из-за
            // кванта — просто стоим: двигать палец «на пол-единицы» нельзя, игра
            // такого сдвига не увидит, а мы посчитаем его отработкой.
            const bool at_edge = (nx <= cfg.box_x0 && dx_wanted < 0.f) ||
                                 (nx >= cfg.box_x1 && dx_wanted > 0.f) ||
                                 (ny <= cfg.box_y0 && dy_wanted < 0.f) ||
                                 (ny >= cfg.box_y1 && dy_wanted > 0.f);
            const bool want_step = std::fabs(err_yaw) > rest_yaw + q_yaw ||
                                   std::fabs(err_pitch) > rest_pitch + q_pitch;
            if (at_edge && want_step) {
                out.action = Action::Lift;
                finger_down_ = false;
                phase_ = Phase::Idle;
                clear_orders();
                return out;
            }
            out.action = Action::Hold;
            out.x = fx_; out.y = fy_;
            return out;
        }

        fx_ = nx; fy_ = ny;
        out.action = Action::Move;
        out.x = fx_; out.y = fy_;

        // Свайп пошёл: считаем, что он отдаст камере, и кладём заказ в окно.
        swipe_err_yaw_ = want_yaw; swipe_err_pitch_ = want_pitch;
        exp_yaw_   = dx * gy;
        exp_pitch_ = -dy * gp;
        ref_err_yaw_ = want_yaw; ref_err_pitch_ = want_pitch;
        push_order(exp_yaw_, exp_pitch_);
        last_swipe_px_ = std::sqrt(dx * dx + dy * dy);
        last_swipe_deg_ = std::fabs(exp_yaw_) > std::fabs(exp_pitch_) ? std::fabs(exp_yaw_) : std::fabs(exp_pitch_);
        if (std::fabs(dx) > 1e-4f) { last_swipe_dx_ = dx; have_last_swipe_ = true; }
        if (std::fabs(dy) > 1e-4f) { last_swipe_dy_ = dy; }
        ++swipes_;
        fly_time_ = 0.f;
        since_swipe_ = 0.f;
        phase_ = Phase::Flying;
        return out;
    }

    static float snap(float v, float units_per_px) {
        if (!(units_per_px >= 0.25f && units_per_px <= 16.f) || !std::isfinite(units_per_px)) return v;
        return std::round(v * units_per_px) / units_per_px;
    }

    static float residual_after_pending(float err, float pending) {
        if (pending == 0.f) return err;
        const float rest = err - pending;
        // Перескок знака: заказ закрывает цель с запасом — считаем, что остатка нет.
        if (rest * err <= 0.f) return 0.f;
        return rest;
    }

    static float progress_ratio(float progress, float pending) {
        if (pending == 0.f) return 1.f;
        const float r = progress / pending;
        return r < 0.f ? 0.f : r;
    }

    // Сдвиг остатка, который ещё не считается движением (шум кости/чтения).
    static constexpr float kStillEps     = 0.02f;   // град
    // Доля заказа, с которой свайп считается доехавшим.
    static constexpr float kProgressDone = 0.85f;
    // Столько тактов цикла подряд без единого градуса отклика — считаем, что
    // ввод потерян (палец перехвачен игрой, экран перекрыт), и переносим палец.
    static constexpr int   kDeadCycles   = 3;
    // Меньше этой доли заказа в отклике — считаем, что отклика не было.
    static constexpr float kMinResponse     = 0.30f;
    // Больше этой — доехало сверх заказанного: коэффициент из настройки занижен.
    static constexpr float kMaxCleanResponse = 1.15f;
    // Адаптация доли шага: перелёт или перелив отклика — долю вниз (но не ниже
    // kMinStepFrac). Вверх доля возвращается только через подтверждение отклика.
    static constexpr float kFlipShrink     = 0.6f;
    static constexpr float kOverShrink     = 0.85f;
    static constexpr float kMinStepFrac    = 0.30f;
    static constexpr float kMaxStepFrac    = 1.50f;
    static constexpr float kFracGrow       = 0.25f;
    // Остаток, с которого «доехало меньше заказанного» считается признаком
    // завышенного коэффициента, а не шумом кости.
    static constexpr float kBigResidualDeg = 2.0f;
    // Отклик считается улёгшимся, когда его скорость упала во столько раз от пика.
    static constexpr float kSettlePeakFrac = 0.2f;
    // Жёсткий предел жизни заказа с откликом: окно + запас на сглаживание.
    static constexpr float kMaxOrderLife   = 1.20f;
    // Заказов в окне трубы. Свайп выдаётся только на пустое окно, так что
    // глубоко оно не забивается — запаса хватает.
    static constexpr int   kOrderSlots   = 8;
    // Труба касания Android — 2-8 кадров ИГРЫ; берём верхнюю границу.
    static constexpr float kTubeFrames    = 8.f;
    // Уменьшение остатка, с которого заказ считается отработанным (в градусах
    // отклика — измерять его нашей оценкой коэффициента нельзя).
    static constexpr float kCreditDeg     = 0.9f;
    // Порог устойчивого дрейфа у цели — как в мемори-локе.
    static constexpr int   kJitterTicks  = 3;
    static constexpr float kFlipDamp     = 0.5f;
    // Запас первого (неизмеренного) свайпа по оси: 0.7 перекрывает занижение
    // коэффициента из настройки чувствительности до 1.4 раза.
    static constexpr float kFirstSwipeFrac = 0.7f;
    // Смещение точки постановки: на 25 град остатка — на 90% комнаты зоны.
    static constexpr float kPlaceBiasFullDeg = 25.f;
    static constexpr float kPlaceBiasFrac    = 0.9f;

    Phase phase_ = Phase::Idle;
    bool  finger_down_ = false;
    float fx_ = 0.f, fy_ = 0.f;
    int   hold_ticks_ = 0;

    // Окно трубы: заказы, которым ещё не время доехать. Их сумма вычитается из
    // остатка при расчёте следующего свайпа.
    struct Order {
        float yaw, pitch;      // заказ в градусах нашей оценки коэффициента
        float age;             // сколько живёт в окне, с
        bool  responded;       // по заказу уже пошло движение остатка
        float ref_yaw, ref_pitch;   // остаток в момент выдачи заказа
    };
    Order orders_[kOrderSlots];
    int   order_n_ = 0;
    float pending_yaw_ = 0.f, pending_pitch_ = 0.f;
    float peak_rate_ = 0.f;      // пиковая скорость отклика текущего заказа, град/такт
    int   still_ticks_ = 0;      // тактов подряд отклик не идёт
    float ref_err_yaw_ = 0.f, ref_err_pitch_ = 0.f;   // опорный остаток для новых заказов

    bool  have_prev_err_ = false;
    float prev_err_yaw_ = 0.f, prev_err_pitch_ = 0.f;
    float exp_yaw_ = 0.f, exp_pitch_ = 0.f;
    float fly_time_ = 0.f;
    int   dead_cycles_ = 0;
    float tick_dt_ = 1.f / 60.f;
    float since_swipe_ = 999.f;
    float swipe_err_yaw_ = 0.f, swipe_err_pitch_ = 0.f;

    // Подтверждение отклика собственной петлёй (см. step_flying).
    bool  verified_yaw_ = false, verified_pitch_ = false;
    // Полосы и кванты текущего такта (см. update_bands).
    float q_yaw_ = 0.f, q_pitch_ = 0.f;
    float rest_yaw_ = 0.f, rest_pitch_ = 0.f;
    // Доля шага по оси и счётчик честных тактов подряд (см. адаптацию в step_flying).
    float step_gain_yaw_ = kFirstSwipeFrac, step_gain_pitch_ = kFirstSwipeFrac;
    bool  flipped_yaw_ = false, flipped_pitch_ = false;
    int   short_cycles_yaw_ = 0, short_cycles_pitch_ = 0;
    int   streak_yaw_ = 0, streak_pitch_ = 0;
    bool  have_last_swipe_ = false;
    float last_swipe_dx_ = 0.f, last_swipe_dy_ = 0.f;
    float last_swipe_px_ = 0.f, last_swipe_deg_ = 0.f;
    int   swipes_ = 0, reversals_ = 0;
};

}  // namespace aim_touch
