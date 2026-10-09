// Aim controller simulator.
//
// Runs the touch and memory controllers against a synthetic camera with input
// latency, game-frame bursts (low FPS), bone noise and a true/believed gain
// mismatch. The control math mirrors UpdateAimTouch / UpdateAimMemory in
// jni/src/aim.cpp. Keep the two in sync when either side changes: the
// constants and the step order below are copies, not shared code.
//
// build:  g++ -std=gnu++17 -O2 -Wall -Wextra -I jni/src tools/aim/aim_sim.cpp -o /tmp/aim_sim
// run:    /tmp/aim_sim [outdir]        (default /home/user/aim_sim_out)

#include "mem_aim_control.h"
#include "manes_aim_source.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

using mem_aim_control::wrap180;

// ---- constants copied from aim.cpp ------------------------------------------
constexpr float kTouchCamMovedDeg = 0.005f;
constexpr float kTouchFlightLife  = 0.13f;
constexpr float kTouchStallTime   = 1.00f;
constexpr float kTouchInflightBand = 2.5f;
constexpr float kTouchJitterBand  = 0.45f;
constexpr int   kTouchDriftStreak = 3;
constexpr float kTouchRampBand    = 2.5f;
constexpr float kTouchRampMin     = 0.40f;
constexpr float kStable           = 0.45f;
constexpr float kMemMaxSpeedDeg   = 72.0f;

// Knobs that are worth sweeping. Defaults equal the values in aim.cpp.
struct Knobs {
    float touchTau = 0.04f;
    float memTau = 0.05f;
    float memDeadband = 0.035f;
};
Knobs g_knobs;

// ---- screen model -----------------------------------------------------------
constexpr float kSw = 720.f, kSh = 1600.f;
constexpr float kCamFov = 60.f;
constexpr float kUnitsPerPx = 2.f;   // device units per screen px (unknown on device)
constexpr float kBeliefGain = 0.10f; // deg per px the controller believes
constexpr float kPtFracX = 0.74f, kPtFracY = 0.50f;

struct Vec2 { float y = 0.f, p = 0.f; };

// ---- synthetic game plant ---------------------------------------------------
// Touch: writes are camera deltas. Memory: writes are absolute accumulator
// values. Both reach the camera `latency` ticks later and the game only
// consumes pending writes every `gdiv` ticks (frame bursts at low FPS).
struct Plant {
    int  latency = 3;
    int  gdiv    = 1;
    bool absolute = false;
    struct Entry { int at; Vec2 v; };
    std::deque<Entry> q;
    Vec2 cam;

    void push(int tick, Vec2 v) { q.push_back({tick + latency, v}); }

    // returns the camera change applied on this tick
    Vec2 advance(int tick) {
        const Vec2 before = cam;
        if (tick % gdiv != 0) return Vec2{};
        bool any = false;
        Vec2 sum{}, last{};
        while (!q.empty() && q.front().at <= tick) {
            sum.y += q.front().v.y; sum.p += q.front().v.p;
            last = q.front().v; any = true;
            q.pop_front();
        }
        if (any) {
            if (absolute) cam = last;
            else { cam.y = wrap180(cam.y + sum.y); cam.p += sum.p; }
        }
        return Vec2{wrap180(cam.y - before.y), cam.p - before.p};
    }
};

// ---- scenarios --------------------------------------------------------------
enum TargetKind { kStatic = 0, kStrafe = 1, kApproach = 2 };

struct Scenario {
    const char* name;
    TargetKind  kind;
    float duration;    // s
    float fps;         // overlay ticks per second, equals game frames here
    int   latency;     // ticks from write to camera
    int   gdiv;        // game consumes pending writes every gdiv ticks
    float noise;       // bone point sigma, deg
    float gainErr;     // true touch gain / believed gain
    bool  haveCam;     // real camera axis readable
    bool  memory;      // memory mode is meaningful (needs camera axis)
    float yaw0, pitch0;
};

Vec2 targetAbs(const Scenario& s, float t) {
    Vec2 v{s.yaw0, s.pitch0};
    if (s.kind == kStrafe) v.y = s.yaw0 + 4.f * std::sin(2.f * 3.14159265f * 0.7f * t);
    if (s.kind == kApproach) v.y = s.yaw0 * (1.f - std::min(t / 1.5f, 1.f));
    return v;
}

// ---- memory controller (mirror of UpdateAimMemory) --------------------------
float memAxisStep(float err, float dt, float gunStr) {
    if (!std::isfinite(err)) return 0.f;
    if (std::fabs(err) <= g_knobs.memDeadband) return 0.f;
    float speed = std::clamp(gunStr, 1.f, 10.f);
    const float smooth = 0.25f + (speed - 1.f) / 9.f * 0.75f;
    const float gain = manes_aim_source::frame_gain(smooth, dt);
    const float maxStep = kMemMaxSpeedDeg * dt;
    float step = err * gain;
    return std::clamp(step, -maxStep, maxStep);
}

struct MemCtl {
    // false: error against the camera (lags the writes by the pipe depth),
    // true: error against the accumulator (already holds every write in flight)
    bool useAcc = false;
    bool haveLast = false;
    float lastCamY = 0.f, lastCamP = 0.f;
    float lastOffY = 0.f, lastOffP = 0.f;
    float accY = 0.f, accP = 0.f;   // accumulator as the game holds it (our writes)
    mem_aim_control::AngleFilter filt;
    float gunStr = 5.f;
};

struct MemOut { bool wrote = false; float errY = 0.f, errP = 0.f; float stepY = 0.f, stepP = 0.f; };

MemOut memTick(MemCtl& c, float dt, float camY, float camP, float offY, float offP) {
    MemOut o;
    const bool haveObs = c.haveLast;
    const float obsY = haveObs ? wrap180(camY - c.lastCamY) : 0.f;
    const float obsP = haveObs ? camP - c.lastCamP : 0.f;
    c.filt.update(wrap180(camY + offY), camP + offP, dt, g_knobs.memTau);
    float errY = wrap180(c.filt.yaw - camY);
    float errP = c.filt.pitch - camP;
    if (c.useAcc) {
        errY = wrap180(c.filt.yaw - c.accY);
        errP = c.filt.pitch - c.accP;
    } else if (haveObs) {
        errY -= mem_aim_control::pending_part(c.lastOffY, obsY);
        errP -= mem_aim_control::pending_part(c.lastOffP, obsP);
    }
    c.lastCamY = camY; c.lastCamP = camP; c.haveLast = true;
    o.errY = errY; o.errP = errP;

    const float sY = memAxisStep(errY, dt, c.gunStr);
    const float sP = memAxisStep(errP, dt, c.gunStr);
    if (sY == 0.f && sP == 0.f) { c.lastOffY = c.lastOffP = 0.f; return o; }
    c.accY += sY; c.accP += sP;
    c.lastOffY = sY; c.lastOffP = sP;
    o.wrote = true; o.stepY = sY; o.stepP = sP;
    return o;
}

// ---- touch controller (mirror of UpdateAimTouch, no lead, gain known) -------
struct Meas {
    bool  haveCam = true;
    float camYaw = 0.f, camPitch = 0.f;
    bool  flowOk = false;
    float flowY = 0.f, flowP = 0.f;     // camera flow this tick (boxes)
    float offY = 0.f, offP = 0.f;       // measured offset from camera to bone
    float worldDist = 40.f;
};

struct TouchOut {
    bool  held = false;
    bool  lifted = false;
    int   reason = 0;       // 0 move, 1 ack wait, 2 dead zone, 3 jitter gate, 4 start, 5 lifted
    float dx = 0.f, dy = 0.f;   // finger move, px
};

struct Ord { float y, p, age; };

struct TouchCtl {
    float gunStr = 5.f;
    float degPerPx = kCamFov / kSh;
    bool  down = false;
    int   hold = 0;
    float fx = 0.f, fy = 0.f;
    float pendDx = 0.f, pendDy = 0.f, pendTime = 0.f;
    bool  camMovedPrev = false;
    int   ackTimeouts = 0;
    float ackDisabled = 0.f;
    bool  haveLast = false;
    float lastCamYaw = 0.f, lastCamPitch = 0.f;
    mem_aim_control::AngleFilter filt;
    float carryX = 0.f, carryY = 0.f;
    float flowCamYaw = 0.f, flowCamPitch = 0.f;
    float acquire = 0.f;
    int   streakY = 0, streakP = 0;
    float flipDamp = 1.f;
    float prevCtlY = 0.f, prevCtlP = 0.f;
    bool  haveCtl = false;
    std::vector<Ord> flight;
    float absorbY = 0.f, absorbP = 0.f, stallY = 0.f, stallP = 0.f;
    float prevRawY = 0.f, prevRawP = 0.f;
    bool  havePrevRaw = false;
};

void clearFlight(TouchCtl& c) {
    c.flight.clear();
    c.absorbY = c.absorbP = 0.f;
    c.stallY = c.stallP = 0.f;
    c.havePrevRaw = false;
    c.streakY = c.streakP = 0;
}

float snapGrid(float v) { return std::round(v * kUnitsPerPx) / kUnitsPerPx; }

float minmag(float a, float b) { return std::fabs(a) < std::fabs(b) ? a : b; }

TouchOut touchTick(TouchCtl& c, float dt, bool switched, const Meas& m) {
    TouchOut out;
    const float ptX = kPtFracX * kSw, ptY = kPtFracY * kSh;
    if (switched) {
        c.filt.reset(); c.carryX = c.carryY = 0.f;
        c.haveLast = false;
        c.streakY = c.streakP = 0;
        clearFlight(c);
    }

    // target filter: absolute angle, camera axis if present, else integrated flow
    float errY = m.offY, errP = m.offP;
    {
        float refY = 0.f, refP = 0.f;
        bool  ok = false;
        if (m.haveCam) { refY = m.camYaw; refP = m.camPitch; ok = true; }
        else if (m.flowOk) {
            c.flowCamYaw = wrap180(c.flowCamYaw + m.flowY);
            c.flowCamPitch += m.flowP;
            refY = c.flowCamYaw; refP = c.flowCamPitch; ok = true;
        }
        if (ok) {
            c.filt.update(wrap180(refY + m.offY), refP + m.offP, dt, g_knobs.touchTau);
            errY = wrap180(c.filt.yaw - refY);
            errP = c.filt.pitch - refP;
        } else {
            c.filt.reset();
        }
    }

    // camera answer detection (ack gate)
    bool camMoved = false;
    if (m.haveCam && c.haveLast) {
        const float dY = wrap180(m.camYaw - c.lastCamYaw);
        const float dP = m.camPitch - c.lastCamPitch;
        camMoved = std::fabs(dY) > kTouchCamMovedDeg || std::fabs(dP) > kTouchCamMovedDeg;
    }
    if (m.haveCam) { c.lastCamYaw = m.camYaw; c.lastCamPitch = m.camPitch; c.haveLast = true; }
    else { c.haveLast = false; c.pendDx = c.pendDy = 0.f; c.pendTime = 0.f; }

    const bool camAnswered = camMoved || c.camMovedPrev;
    c.camMovedPrev = camMoved;
    if (c.ackDisabled > 0.f) c.ackDisabled -= dt;
    const bool ackGate = c.ackDisabled <= 0.f;
    if (ackGate && c.down && !camAnswered &&
        (std::fabs(c.pendDx) >= 1.f || std::fabs(c.pendDy) >= 1.f)) {
        c.pendTime += dt;
        if (c.pendTime < 0.25f) { out.held = true; out.reason = 1; return out; }
        c.pendDx = c.pendDy = 0.f; c.pendTime = 0.f;
        if (++c.ackTimeouts >= 3) { c.ackTimeouts = 0; c.ackDisabled = 5.f; }
    } else if (camAnswered) {
        c.ackTimeouts = 0;
    }

    // in-flight model, only without a camera axis
    const bool useFlight = !m.haveCam;
    bool flowOkTick = false;
    if (useFlight) {
        if (c.havePrevRaw && m.flowOk) {
            flowOkTick = true;
            auto drain = [](float& absorb, float flow) {
                if (absorb > 0.f && flow > 0.f)
                    absorb = (flow >= absorb) ? 0.f : absorb - flow;
                else if (absorb < 0.f && flow < 0.f)
                    absorb = (-flow >= -absorb) ? 0.f : absorb - flow;
            };
            const float wasY = c.absorbY, wasP = c.absorbP;
            drain(c.absorbY, m.flowY);
            drain(c.absorbP, m.flowP);
            if (c.absorbY != wasY) c.stallY = 0.f;
            if (c.absorbP != wasP) c.stallP = 0.f;
            if (c.prevRawY * errY < 0.f) c.absorbY = 0.f;
            if (c.prevRawP * errP < 0.f) c.absorbP = 0.f;
        }
        c.prevRawY = errY; c.prevRawP = errP; c.havePrevRaw = true;
        if (c.absorbY != 0.f) { c.stallY += dt; if (c.stallY > kTouchStallTime) c.absorbY = 0.f; }
        if (c.absorbP != 0.f) { c.stallP += dt; if (c.stallP > kTouchStallTime) c.absorbP = 0.f; }
        if (flowOkTick) {
            if (c.absorbY > 0.f && c.absorbY > errY) c.absorbY = (errY > 0.f) ? errY : 0.f;
            if (c.absorbY < 0.f && c.absorbY < errY) c.absorbY = (errY < 0.f) ? errY : 0.f;
            if (c.absorbP > 0.f && c.absorbP > errP) c.absorbP = (errP > 0.f) ? errP : 0.f;
            if (c.absorbP < 0.f && c.absorbP < errP) c.absorbP = (errP < 0.f) ? errP : 0.f;
        }
        for (auto& o : c.flight) o.age += dt;
        c.flight.erase(std::remove_if(c.flight.begin(), c.flight.end(),
                       [](const Ord& o) { return o.age >= kTouchFlightLife; }),
                       c.flight.end());
        float winY = 0.f, winP = 0.f;
        for (const auto& o : c.flight) { winY += o.y; winP += o.p; }
        const bool noFlow = !flowOkTick;
        const float inflY = (!noFlow && std::fabs(errY) > kTouchInflightBand)
                          ? c.absorbY : minmag(c.absorbY, winY);
        const float inflP = (!noFlow && std::fabs(errP) > kTouchInflightBand)
                          ? c.absorbP : minmag(c.absorbP, winP);
        const float origY = errY, origP = errP;
        errY -= inflY; errP -= inflP;
        if (origY * errY < 0.f) errY = 0.f;
        if (origP * errP < 0.f) errP = 0.f;
    } else {
        c.havePrevRaw = false;
    }

    // dead zone
    const float gridPx = 1.f / kUnitsPerPx;
    const float qY = kBeliefGain * gridPx, qP = kBeliefGain * gridPx;
    float deadBase = c.degPerPx * 1.5f;
    if (m.worldDist > 1.f) {
        const float d = std::atan(0.03f / m.worldDist) * 180.f / 3.14159265f;
        if (d < deadBase) deadBase = d;
    }
    const float deadY = std::max(deadBase, qY * 2.f);
    const float deadP = std::max(deadBase, qP * 2.f);
    if (std::fabs(errY) < deadY && std::fabs(errP) < deadP) {
        c.pendDx = c.pendDy = 0.f; c.pendTime = 0.f;
        out.held = true; out.reason = 2;
        return out;
    }

    // jitter gate: sub-band error must persist in one direction
    auto jitterGate = [&](float eff, int& streak) {
        if (eff == 0.f) { streak = 0; return 0.f; }
        if (std::fabs(eff) >= kTouchJitterBand) { streak = 0; return eff; }
        const int s = (eff > 0.f) ? 1 : -1;
        streak = (streak * s > 0) ? streak + s : s;
        return (streak * s >= kTouchDriftStreak) ? eff : 0.f;
    };
    errY = jitterGate(errY, c.streakY);
    errP = jitterGate(errP, c.streakP);
    if (errY == 0.f && errP == 0.f) { out.held = true; out.reason = 3; return out; }

    if (!c.down) {
        c.fx = snapGrid(ptX); c.fy = snapGrid(ptY);
        c.down = true;
        c.pendDx = c.pendDy = 0.f; c.pendTime = 0.f;
        c.hold = 0; c.acquire = 0.f;
        c.carryX = c.carryY = 0.f;
        clearFlight(c);
        out.held = true; out.reason = 4;
        return out;
    }
    if (c.hold < 1) { ++c.hold; out.held = true; out.reason = 4; return out; }

    // controller
    float sm = std::clamp(c.gunStr, 1.f, 10.f);
    const float frac = 0.25f + (sm - 1.f) / 9.f * 0.75f;
    float k = manes_aim_source::frame_gain(frac, dt);
    const float fracMid = 0.25f + (5.f - 1.f) / 9.f * 0.75f;
    if (k > kStable) k = kStable;
    if (frac < fracMid) k *= frac / fracMid;
    if (k > 1.f) k = 1.f;
    if (k < 0.05f) k = 0.05f;

    const bool flipY = c.haveCtl && c.prevCtlY * errY < 0.f &&
                       std::fabs(errY) > std::fabs(c.prevCtlY) * 0.35f;
    const bool flipP = c.haveCtl && c.prevCtlP * errP < 0.f &&
                       std::fabs(errP) > std::fabs(c.prevCtlP) * 0.35f;
    if (flipY || flipP) c.flipDamp = 0.45f;
    else if (c.flipDamp < 1.f) c.flipDamp = std::min(1.f, c.flipDamp + (1.f - c.flipDamp) * dt * 2.5f);
    c.prevCtlY = errY; c.prevCtlP = errP; c.haveCtl = true;
    k *= c.flipDamp;

    c.acquire = std::min(1.f, c.acquire + dt / 0.20f);
    k *= 0.3f + 0.7f * c.acquire;

    {
        const float maxEff = std::max(std::fabs(errY), std::fabs(errP));
        float ramp = std::clamp(maxEff / kTouchRampBand, kTouchRampMin, 1.f);
        k *= ramp;
    }

    const float gy = kBeliefGain, gp = kBeliefGain;
    float dx = errY * k / gy;
    float dy = -errP * k / gp;
    if (std::fabs(errY) < qY * 3.f)
        dx = std::round((errY / gy) * kUnitsPerPx) / kUnitsPerPx;
    if (std::fabs(errP) < qP * 3.f)
        dy = std::round((-errP / gp) * kUnitsPerPx) / kUnitsPerPx;
    if (std::fabs(dx * gy) > std::fabs(errY)) dx = errY / gy;
    if (std::fabs(dy * gp) > std::fabs(errP)) dy = -errP / gp;
    const float maxStep = kSh * 0.05f;
    dx = std::clamp(dx, -maxStep, maxStep);
    dy = std::clamp(dy, -maxStep, maxStep);

    // input quantum with carried remainder
    const float stepUnitsX = mem_aim_control::quantize_units(dx * kUnitsPerPx, c.carryX);
    const float stepUnitsY = mem_aim_control::quantize_units(dy * kUnitsPerPx, c.carryY);
    const float nx = c.fx + stepUnitsX / kUnitsPerPx;
    const float ny = c.fy + stepUnitsY / kUnitsPerPx;
    dx = nx - c.fx; dy = ny - c.fy;
    if (dx == 0.f && dy == 0.f) {
        c.pendDx = c.pendDy = 0.f; c.pendTime = 0.f;
        out.held = true; out.reason = 0;
        return out;
    }

    // finger area: outside it the finger is lifted and re-placed
    float minX = ptX - 0.18f * kSw, maxX = ptX + 0.23f * kSw;
    float minY = ptY - 0.38f * kSh, maxY = ptY + 0.38f * kSh;
    minX = std::max(minX, kSw * 0.02f); maxX = std::min(maxX, kSw * 0.98f);
    minY = std::max(minY, kSh * 0.02f); maxY = std::min(maxY, kSh * 0.98f);
    if (nx < minX || nx > maxX || ny < minY || ny > maxY) {
        c.down = false;
        c.fx = snapGrid(ptX); c.fy = snapGrid(ptY);
        c.pendDx = c.pendDy = 0.f; c.pendTime = 0.f;
        clearFlight(c);
        c.haveLast = false;
        out.lifted = true; out.reason = 5;
        return out;
    }

    c.fx = nx; c.fy = ny;
    c.pendDx += dx; c.pendDy += dy;
    if (useFlight) {
        const float ordY = dx * gy, ordP = -dy * gp;
        c.flight.push_back({ordY, ordP, 0.f});
        c.absorbY += ordY; c.stallY = 0.f;
        c.absorbP += ordP; c.stallP = 0.f;
    }
    out.dx = dx; out.dy = dy; out.reason = 0;
    return out;
}

// ---- run --------------------------------------------------------------------
enum Mode { kMemory = 0, kTouch = 1, kMemAcc = 2 };

struct Row {
    float t, tgtY, tgtP, camY, camP, errY, errP, measY, ctlY, ctlP, stepY, stepP;
    float fingerX, fingerY;
    int held, lifted, reason, wrote;
};

struct Summary {
    float acqT = -1.f;          // first time the true error is inside 0.3 deg
    float peakAfterAcq = 0.f;   // worst true error after acquisition
    float overshoot = 0.f;      // worst error past zero after acquisition
    float rmsTail = 0.f;        // rms of true error over the last 2 s
    int   reversalsTail = 0;    // sign flips of the commanded yaw step, last 2 s
    float maxStepDeg = 0.f;     // largest commanded yaw step per tick
    int   holds = 0;            // ticks spent holding after acquisition
};

std::vector<Row> run(const Scenario& s, Mode mode, Summary& sum) {
    std::vector<Row> rows;
    const float dt = 1.f / s.fps;
    const int N = int(s.duration * s.fps + 0.5f);
    std::mt19937 rng(12345u + unsigned(s.kind) * 97u + unsigned(mode) * 7u + unsigned(s.latency));
    std::normal_distribution<float> nz(0.f, 1.f);
    std::normal_distribution<float> flowNz(0.f, 0.01f);

    Plant plant;
    plant.latency = s.latency;
    plant.gdiv = s.gdiv;
    plant.absolute = (mode != kTouch);

    MemCtl mc; mc.gunStr = 5.f; mc.useAcc = (mode == kMemAcc);
    TouchCtl tc; tc.gunStr = 5.f;
    const float gainTrue = kBeliefGain * s.gainErr;
    const float ptX = kPtFracX * kSw, ptY = kPtFracY * kSh;
    float fingerX = snapGrid(ptX), fingerY = snapGrid(ptY);

    for (int i = 0; i < N; ++i) {
        const float t = i * dt;
        const Vec2 camStep = plant.advance(i);
        const Vec2 tgt = targetAbs(s, t);
        const Vec2 cam = plant.cam;
        const float noiseY = s.noise * nz(rng), noiseP = s.noise * nz(rng);
        const float measY = tgt.y + noiseY - cam.y;
        const float measP = tgt.p + noiseP - cam.p;

        Row r{};
        r.t = t; r.tgtY = tgt.y; r.tgtP = tgt.p; r.camY = cam.y; r.camP = cam.p;
        r.measY = measY;

        if (mode != kTouch) {
            const MemOut o = memTick(mc, dt, cam.y, cam.p, measY, measP);
            if (o.wrote) {
                plant.push(i, Vec2{mc.accY, mc.accP});
                r.stepY = o.stepY; r.stepP = o.stepP;
            }
            r.ctlY = o.errY; r.ctlP = o.errP;
            r.wrote = o.wrote;
        } else {
            Meas m;
            m.haveCam = s.haveCam;
            m.camYaw = cam.y; m.camPitch = cam.p;
            m.flowOk = true;
            m.flowY = camStep.y + flowNz(rng);
            m.flowP = camStep.p + flowNz(rng);
            m.offY = measY; m.offP = measP;
            const TouchOut o = touchTick(tc, dt, i == 0, m);
            if (o.dx != 0.f || o.dy != 0.f) {
                plant.push(i, Vec2{gainTrue * o.dx, -gainTrue * o.dy});
                r.stepY = gainTrue * o.dx;
                r.stepP = -gainTrue * o.dy;
                r.wrote = 1;
            }
            if (o.lifted) r.lifted = 1;
            if (tc.down) { fingerX = tc.fx; fingerY = tc.fy; }
            r.held = o.held ? 1 : 0;
            r.reason = o.reason;
            r.ctlY = tc.prevCtlY; r.ctlP = tc.prevCtlP;
        }
        r.errY = tgt.y - cam.y;
        r.errP = tgt.p - cam.p;
        r.fingerX = fingerX; r.fingerY = fingerY;
        rows.push_back(r);
    }

    // summary. Overshoot means crossing the target after it stopped moving:
    // while a target is moving, the camera trails it by the pipe depth, and
    // that trail is lag, not overshoot.
    const float tailFrom = s.duration - 2.f;
    const float stopT = (s.kind == kApproach) ? 1.5f : 0.f;
    float acqErrCount = 0.f, sq = 0.f;
    float errSign = 0.f;
    bool acquired = false;
    for (const Row& r : rows) {
        const float e = std::hypot(r.errY, r.errP);
        if (!acquired && e < 0.3f) {
            acquired = true; sum.acqT = r.t;
        }
        if (acquired && r.t >= stopT) {
            // approach side: the side the camera is on once the target has stopped
            if (errSign == 0.f && std::fabs(r.errY) > 0.05f) errSign = (r.errY >= 0.f) ? 1.f : -1.f;
            sum.peakAfterAcq = std::max(sum.peakAfterAcq, e);
            if (r.errY * errSign < 0.f) sum.overshoot = std::max(sum.overshoot, std::fabs(r.errY));
        }
        if (acquired && r.held) ++sum.holds;
        sum.maxStepDeg = std::max(sum.maxStepDeg, std::fabs(r.stepY));
        if (r.t >= tailFrom) { sq += e * e; acqErrCount += 1.f; }
    }
    sum.rmsTail = acqErrCount > 0.f ? std::sqrt(sq / acqErrCount) : 0.f;
    float prevStep = 0.f;
    for (const Row& r : rows) {
        if (r.t < tailFrom) continue;
        if (std::fabs(r.stepY) < 0.02f) continue;
        if (prevStep != 0.f && (r.stepY > 0.f) != (prevStep > 0.f)) ++sum.reversalsTail;
        prevStep = r.stepY;
    }
    return rows;
}

void writeCsv(const std::string& path, const std::vector<Row>& rows) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { std::perror(path.c_str()); return; }
    std::fprintf(f, "t,tgt_yaw,tgt_pitch,cam_yaw,cam_pitch,err_yaw,err_pitch,meas_yaw,"
                    "ctl_yaw,ctl_pitch,step_yaw,step_pitch,finger_x,finger_y,held,lifted,reason,wrote\n");
    for (const Row& r : rows)
        std::fprintf(f, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%.1f,%.1f,%d,%d,%d,%d\n",
                     r.t, r.tgtY, r.tgtP, r.camY, r.camP, r.errY, r.errP, r.measY,
                     r.ctlY, r.ctlP, r.stepY, r.stepP, r.fingerX, r.fingerY,
                     r.held, r.lifted, r.reason, r.wrote);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    std::string outDir = "/home/user/aim_sim_out";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        float v = 0.f;
        if (std::sscanf(a.c_str(), "--touch-tau=%f", &v) == 1) g_knobs.touchTau = v;
        else if (std::sscanf(a.c_str(), "--mem-tau=%f", &v) == 1) g_knobs.memTau = v;
        else if (std::sscanf(a.c_str(), "--mem-deadband=%f", &v) == 1) g_knobs.memDeadband = v;
        else outDir = a;
    }
    std::filesystem::create_directories(outDir);
    std::printf("knobs: touch-tau=%.3f mem-tau=%.3f mem-deadband=%.3f\n",
                g_knobs.touchTau, g_knobs.memTau, g_knobs.memDeadband);

    const Scenario scenarios[] = {
        {"static",   kStatic,   6.f, 60.f, 3, 1, 0.15f, 1.0f, true,  true,  6.f, 2.f},
        {"strafe",   kStrafe,   8.f, 60.f, 3, 1, 0.15f, 1.0f, true,  true,  6.f, 0.f},
        {"lowfps",   kStatic,   6.f, 15.f, 4, 1, 0.15f, 1.0f, true,  true,  6.f, 2.f},
        {"mismatch", kStatic,   6.f, 60.f, 3, 1, 0.15f, 1.5f, true,  true,  6.f, 2.f},
        {"nocam",    kStatic,   6.f, 60.f, 3, 1, 0.15f, 1.0f, false, false, 6.f, 2.f},
        {"approach", kApproach, 4.f, 60.f, 3, 1, 0.15f, 1.0f, true,  true, 30.f, 0.f},
    };

    std::printf("%-9s %-6s %7s %9s %9s %9s %6s %7s %6s\n",
                "scenario", "mode", "acq_s", "peak_deg", "overshoot", "rms_tail", "revs", "max_step", "holds");
    for (const Scenario& s : scenarios) {
        for (Mode mode : {kMemory, kMemAcc, kTouch}) {
            if (mode != kTouch && !s.memory) continue;
            Summary sum;
            const std::vector<Row> rows = run(s, mode, sum);
            const char* modeName = (mode == kMemory) ? "memory" : (mode == kMemAcc) ? "memacc" : "touch";
            writeCsv(outDir + "/" + s.name + "_" + modeName + ".csv", rows);
            std::printf("%-9s %-6s %7.2f %9.3f %9.3f %9.3f %6d %7.3f %6d\n",
                        s.name, modeName, sum.acqT, sum.peakAfterAcq, sum.overshoot,
                        sum.rmsTail, sum.reversalsTail, sum.maxStepDeg, sum.holds);
        }
    }
    std::printf("csv written to %s\n", outDir.c_str());
    return 0;
}
