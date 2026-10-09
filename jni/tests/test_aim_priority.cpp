#include "aim_priority.h"

#include <cassert>
#include <cmath>

int main() {
    using namespace aim_priority;

    assert(valid_rule(SMART) && valid_rule(TO_CROSSHAIR) && valid_rule(TO_ME));
    assert(!valid_rule(-1) && !valid_rule(3));

    // «К прицелу»: ближе к перекрестию всегда лучше, дальность не важна.
    assert(raw_score(TO_CROSSHAIR, 0.2f, 0.1f) < raw_score(TO_CROSSHAIR, 0.3f, 0.01f));
    assert(raw_score(TO_CROSSHAIR, 0.2f, 5.0f) < raw_score(TO_CROSSHAIR, 0.3f, 0.01f));

    // «Ко мне»: ближе по дистанции всегда лучше, угол — только тай-брейк.
    assert(raw_score(TO_ME, 0.9f, 0.20f) < raw_score(TO_ME, 0.0f, 0.30f));
    assert(raw_score(TO_ME, 0.1f, 0.25f) < raw_score(TO_ME, 0.2f, 0.25f));

    // «Умный»: баланс угла и дальности.
    assert(raw_score(SMART, 0.5f, 0.0f) < raw_score(SMART, 0.0f, 1.0f));

    // Гистерезис: для явных правил залипание не перебивает правило сильнее 20%.
    const float crossNear = raw_score(TO_CROSSHAIR, 0.20f, 0.f);
    const float crossFar  = raw_score(TO_CROSSHAIR, 0.10f, 0.f);
    assert(apply_sticky(TO_CROSSHAIR, crossNear, true) > crossFar);
    assert(apply_sticky(TO_ME, raw_score(TO_ME, 0.f, 0.5f), true) >
           raw_score(TO_ME, 0.f, 0.25f));
    assert(apply_sticky(TO_ME, raw_score(TO_ME, 0.f, 0.5f), false) ==
           raw_score(TO_ME, 0.f, 0.5f));
    assert(sticky_scale(SMART) == 0.35f);
    assert(sticky_scale(TO_CROSSHAIR) > 0.5f && sticky_scale(TO_CROSSHAIR) < 1.f);

    return 0;
}
