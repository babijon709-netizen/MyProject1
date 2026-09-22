// Host stand for the take-all auto-tap (tools/loot/run.sh).
// The logic under test is frame-driven (loot_tap_memory.h): nothing here touches
// game memory, only the same event flow the device adapter would report.
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

#include "../../jni/src/loot_tap_memory.h"

using loot_tap::Diagnostics;

struct Recorder {
    struct Ev { char what; float x, y; uint64_t frame; };
    std::vector<Ev> log;
    loot_tap::Inject inject;

    Recorder() {
        inject.down = [](float, float) {};       // rebound in constructor loop workaround below
        inject.up = []() {};
        // Lambdas with captures cannot convert to function pointers: use statics.
        inject.down = &Recorder::static_down;
        inject.up = &Recorder::static_up;
        current = this;
    }
    static Recorder* current;
    static void static_down(float x, float y) {
        current->log.push_back({'D', x, y, current->frame_});
    }
    static void static_up() { current->log.push_back({'U', 0.f, 0.f, current->frame_}); }
    uint64_t frame_ = 0;
};
Recorder* Recorder::current = nullptr;

struct Step {
    bool enabled = true;
    int root = 0;
    bool flow = false;
    bool tap_edge = false;
    float tap_x = 0, tap_y = 0;
    int frames = 1;                 // сколько раз повторить это состояние
};

static void run(loot_tap::Tap& t, Recorder& r, const std::vector<Step>& steps) {
    for (const Step& s : steps)
        for (int i = 0; i < s.frames; ++i) {
            t.on_frame(s.enabled, s.root, s.flow, s.tap_edge, s.tap_x, s.tap_y,
                       r.frame_, r.inject);
            ++r.frame_;
        }
}

static void learn_button(loot_tap::Tap& t, Recorder& r, float x, float y, int repeats = 1) {
    for (int i = 0; i < repeats; ++i) {
        run(t, r, {{true, 1, false, false, 0, 0, 4},           // окно открыто
                   {true, 1, false, true, x, y, 1},            // жест игрока
                   {true, 1, false, false, 0, 0, 2},           // противный поток стартует
                   {true, 1, true,  false, 0, 0, 1}});
    }
}

int main() {
    {
        // Точка учится по двум близким жестам игрока при старте потока переноса.
        loot_tap::Tap t; Recorder r; t.reset();
        learn_button(t, r, 600, 320);
        run(t, r, {{true, 0, false, false, 0, 0, 10}});        // окно закрыто
        learn_button(t, r, 606, 324);
        assert(t.diagnostics().state == 1);
        assert(t.diagnostics().learn_events == 2);
        assert(t.diagnostics().point_x > 600 && t.diagnostics().point_x < 606.5);
        assert(r.log.empty());                                  // ещё ничего не сами нажимали
        std::printf("learn: two real gestures near the button arm the point (%.1f, %.1f)\n",
                    t.diagnostics().point_x, t.diagnostics().point_y);
    }
    {
        // После калибровки: новое открытие окна -> один тап после паузы отрисовки.
        loot_tap::Tap t; Recorder r; t.reset();
        learn_button(t, r, 600, 320, 2);
        run(t, r, {{true, 0, false, false, 0, 0, 200}});       // большой разрыв: кулдаун пройден
        const uint64_t opens = t.diagnostics().window_opens;
        run(t, r, {{true, 1, false, false, 0, 0, 10}});
        assert(t.diagnostics().window_opens == opens + 1);
        assert(t.diagnostics().taps == 1);
        assert(r.log.size() == 2 && r.log[0].what == 'D' && r.log[1].what == 'U');
        assert(r.log[1].frame - r.log[0].frame >= 3);          // удержание ~3 кадра
        // Следующие кадры того же окна: повторов нет.
        run(t, r, {{true, 1, false, false, 0, 0, 400}});
        assert(t.diagnostics().taps == 1);
        std::printf("fire: one tap after window redraw, no repeats on same window\n");
    }
    {
        // Не стрелять: пока идёт перенос / пока кулдаун / при сломанном детекте (-1).
        loot_tap::Tap t; Recorder r; t.reset();
        learn_button(t, r, 600, 320, 2);
        run(t, r, {{true, 0, false, false, 0, 0, 400}});
        run(t, r, {{true, 1, true,  false, 0, 0, 10}});        // поток уже идёт
        assert(t.diagnostics().taps == 0);
        run(t, r, {{true, 1, false, false, 0, 0, 1}});         // поток кончился, кулдауна нет:
        assert(t.diagnostics().taps == 1);                      // один поздний тап допустил
        run(t, r, {{true, 0, false, false, 0, 0, 5}});
        run(t, r, {{true, 1, false, false, 0, 0, 10}});        // быстрое переоткрытие: кулдаун
        assert(t.diagnostics().taps == 1);
        assert(t.diagnostics().gated > 0);
        run(t, r, {{true, 0, false, false, 0, 0, 200}});
        run(t, r, {{true, -1, false, false, 0, 0, 10}});       // детект сломан: молчим
        assert(t.diagnostics().taps == 1);
        std::printf("gates: flow active, cooldown and broken detection all block fire\n");
    }
    {
        // Дальние жесты не доксируют точку: новый кластер подтверждается заново.
        loot_tap::Tap t; Recorder r; t.reset();
        learn_button(t, r, 600, 320);
        learn_button(t, r, 900, 320);                          // далеко: пересмотр
        assert(t.diagnostics().state == 0);
        learn_button(t, r, 904, 322);
        assert(t.diagnostics().state == 1);
        assert(t.diagnostics().point_x > 900);
        std::printf("radius: a far gesture re-seeds the point, near ones confirm it\n");
    }
    {
        // Отключение тумблером во время удержания: палец поднят сразу.
        loot_tap::Tap t; Recorder r; t.reset();
        learn_button(t, r, 600, 320, 2);
        run(t, r, {{true, 0, false, false, 0, 0, 400}});
        run(t, r, {{true, 1, false, false, 0, 0, 7}});         // в процессе тапа (down-кадры)
        run(t, r, {{false, 1, false, false, 0, 0, 1}});        // тумблер выкл
        assert(!r.log.empty() && r.log.back().what == 'U');
        const size_t before = r.log.size();
        run(t, r, {{false, 1, false, false, 0, 0, 10}});
        assert(r.log.size() == before);
        std::printf("off: synthetic finger lifts at once, nothing more until re-enabled\n");
    }
    {
        // Танец счётчика окон без закрытия (2->1) не плодит повторный огонь.
        loot_tap::Tap t; Recorder r; t.reset();
        learn_button(t, r, 600, 320, 2);
        run(t, r, {{true, 0, false, false, 0, 0, 600}});
        run(t, r, {{true, 1, false, false, 0, 0, 8},           // окно открылось: тап
                   {true, 2, false, false, 0, 0, 8},           // рядом что-то оверлейное
                   {true, 1, false, false, 0, 0, 400}});
        assert(t.diagnostics().taps == 1);
        std::printf("edges: overlay count dancing around the open window fires once\n");
    }
    std::puts("loot-tap: all auto-tap gating checks passed");
    return 0;
}
