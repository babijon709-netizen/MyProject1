#include "aim_lead.h"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool near(float a, float b, float eps = 1e-5F) { return std::fabs(a - b) <= eps; }

int main() {
    // дрожь ниже порога: упреждения нет
    assert(aimlead::LeadFrames(0.055F, 0.2F, 0.19F, 1.F) == 0.F);
    // NaN не проходит
    assert(aimlead::LeadFrames(0.055F, 0.2F, NAN, 1.F) == 0.F);
    // устойчивое движение: k = base * (1 - leadMin/v)
    assert(near(aimlead::LeadFrames(0.055F, 0.2F, 0.4F, 1.F), 0.055F * 0.5F));
    // разворот: вдвое меньше при той же скорости
    assert(near(aimlead::LeadFrames(0.055F, 0.2F, 0.4F, -1.F), 0.055F * 0.5F * 0.5F));
    // кэп 1.5 кадра
    assert(near(aimlead::LeadFrames(10.F, 0.2F, 100.F, 1.F), aimlead::kMaxLeadFrames));
    std::printf("ALL AIM-LEAD TESTS PASS\n");
    return 0;
}
