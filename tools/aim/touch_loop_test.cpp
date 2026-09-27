// Стенд тач-петли аимбота (запуск: sh tools/aim/run.sh).
//
// НАСТОЯЩИЙ UpdateAimTouch из jni/src/aim.cpp (вырезает run.sh в
// touch_ctrl.inc вместе с выбором цели, потоком камеры и коэффициентом из
// чувствительности) гоняется по модели устройства: оверлей тикает своим
// темпом, игра — 60 кадров в секунду, касание доезжает до камеры через
// «трубу» ввода Android (2-8 кадров игры, замер контроллера фарма), боксы
// ESP отражают камеру с той же задержкой.
//
// Зачем. Жалоба 27.09: «тач-аим всё равно дёргает, маятник первых 2-3
// секунд не пропал». На устройстве глазами видно только «трясёт». Здесь
// воспроизводится причина числом: когда на экране меньше трёх боксов (дуэль
// 1 на 1), поток камеры не считается, и оценка приказов «в полёте» бралась
// МИНИМУМОМ из поглощения и окна в 0.13 с — при трубе 0.2-0.3 с окно пустеет
// раньше прилёта приказа, контроллер заказывает остаток повторно, камера
// прилетает на сумму: мах через цель, реверс, снова мах = маятник.
// Проверяется правка: на захвате летящий заказ — поглощение, списываемое
// ТОЛЬКО по факту движения камеры (ему безразлична длина трубы), а малый
// реверс на захвате — ещё и доказательство заниженного коэффициента, по
// которому рабочий гейн поднимается выше потолка 1.4x (см. сам код).
//
// Каждый сценарий — отдельный процесс (fork): состояние контроллера живёт в
// статиках внутри UpdateAimTouch, и прогоны не должны наследовать чужие хвосты.
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <deque>
#include <algorithm>
#include <cstdarg>
#include <sys/wait.h>
#include <unistd.h>

#include "xp.inc"

#include "logfile.h"   // без AIM_LOG LogLine — no-op

namespace lang { inline const char* text(const char* ru) { return ru; } }

#include "app_state.h"
#include "game.h"
#include "aim.h"
#include "aim_touch_geometry.h"
#include "aim_learn.h"
#include "widgets.h"

// ---- заглушки оверлея ------------------------------------------------------
struct { int width = 2460, height = 1080, orientation = 0; } displayInfo;
static int native_window_screen_x = 2460, native_window_screen_y = 1080;
static bool g_esp_attached = true;
static int  g_calibMode = 0;
static bool g_buildPrompt = false;
AppState   g_state;
InputState g_input;
bool       g_aimActive = false;
Sheet      g_sheet;
Popover    g_pop;

bool s_fingerDown = false;
static void AimReleaseFinger(bool& down) { down = false; }

static int g_toasts = 0;
void ShowToast(const char*, float) { ++g_toasts; }
void ShowToast(const char*) { ++g_toasts; }

// ---- часы стенда: время симулированное, MonoNow отдаёт его -----------------
static double g_now = 0.0;
double MonoNow() { return g_now; }

// ====================== «игра» ==============================================
// Камера крутится приказами пальца с задержкой трубы. Приказ — это СДВИГ
// пальца по экрану (px); камера отвечает на него поворотом dx * gTrue град
// через pipe секунд (ровно в ближайшем своём кадре, 60 fps).
struct Plant {
    float  gTrue = 0.10f;      // истинный град/px игры
    double pipe = 0.20;        // задержка касание->камера, с
    bool   sensOk = true;
    float  sens = 2.0f;        // настройка чувствительности в игре
    float  camYaw = 0.f, camPitch = 0.f;
    struct Order { double at; float dxPx, dyPx; };
    std::deque<Order> orders;
    float  fx = -1e9f, fy = -1e9f;   // позиция пальца (стенд)
    bool   haveFinger = false;

    void fingerDown(float x, float y) { fx = x; fy = y; haveFinger = true; }
    void fingerMove(float x, float y) {
        if (!haveFinger) { fx = x; fy = y; haveFinger = true; return; }
        const float dx = x - fx, dy = y - fy;
        fx = x; fy = y;
        if (dx == 0.f && dy == 0.f) return;
        orders.push_back({g_now + pipe, dx, dy});
    }
    void fingerUp() { haveFinger = false; }

    // Кадр игры в момент t: применить все долетевшие приказы.
    void frame(double t) {
        while (!orders.empty() && orders.front().at <= t) {
            camYaw   += orders.front().dxPx * gTrue;
            camPitch -= orders.front().dyPx * gTrue;   // палец вниз = взгляд вниз
            orders.pop_front();
        }
    }
};
static Plant g_plant;

// ---- тач-API (записывает приказы в «трубу» устройства) ---------------------
void Touch_Down(float x, float y) { g_plant.fingerDown(x, y); }
void Touch_Move(float x, float y) { g_plant.fingerMove(x, y); }
void Touch_Up() { g_plant.fingerUp(); }
float Touch_DeviceUnitsPerPixel() { return 1.f; }

// ---- ESP: цель + болванки на фиксированных мировых углах -------------------
// Поток камеры (AimUpdateFlow) считается по медиане сдвига центров боксов:
// болванки стоят в мире, поэтому их экранный сдвиг — чистый поворот камеры.
// Боксов меньше трёх — потока нет, как в дуэли 1 на 1.
static const float kRad2Deg = 57.29577951f;
static const float kPxPerDeg = 18.f;   // 1080 px / 60°

struct Entity { unsigned long long id; float yaw, pitch; };
static std::vector<Entity> g_world;      // мир: цель + болванки
static std::vector<EspBox> g_boxes;
static float g_jitAmp = 0.f;             // дрожь кости цели, град (знак чередуется)
static int   g_jitPhase = 0;

static void anglesTo(float yaw, float pitch, float& x, float& y, float& z) {
    const float cy = cosf(yaw / kRad2Deg), sy = sinf(yaw / kRad2Deg);
    const float cp = cosf(pitch / kRad2Deg), sp = sinf(pitch / kRad2Deg);
    x = cp * sy; y = sp; z = cp * cy;
}

// Ошибка прицела: от базиса (yaw/pitch камеры) к мировому углу точки.
static void errFrom(float baseYaw, float basePitch, float tgtYaw, float tgtPitch,
                    float& eYaw, float& ePitch) {
    float tx = 0.f, ty = 0.f, tz = 1.f;
    anglesTo(tgtYaw, tgtPitch, tx, ty, tz);
    float fx = 0.f, fy = 0.f, fz = 1.f;
    anglesTo(baseYaw, basePitch, fx, fy, fz);
    float rx = fz, ry = 0.f, rz = -fx;
    const float rl = sqrtf(rx * rx + rz * rz) > 1e-6f ? sqrtf(rx * rx + rz * rz) : 1.f;
    rx /= rl; rz /= rl;
    const float ux = fy * rz - fz * ry, uy = fz * rx - fx * rz, uz = fx * ry - fy * rx;
    const float df = tx * fx + ty * fy + tz * fz;
    const float dr = tx * rx + ty * ry + tz * rz;
    const float du = tx * ux + ty * uy + tz * uz;
    eYaw = atan2f(dr, df) * kRad2Deg;
    ePitch = atan2f(du, sqrtf(df * df + dr * dr)) * kRad2Deg;
}

// Снимок ESP этого такта: боксы отражают ТЕКУЩУЮ камеру (задержка трубы уже
// внутри неё). Три слота костей (голова/шея/грудь) цели — в одну точку;
// дрожь кости существует только в ВИДЕ контроллера (кости в боксе), метрики
// ниже считаются по физической оси «камера -> тело цели», без дрожи.
static void snapshot() {
    g_boxes.clear();
    g_jitPhase ^= 1;
    for (size_t i = 0; i < g_world.size(); ++i) {
        const Entity& e = g_world[i];
        const float jit = (i == 0) ? g_jitAmp * (g_jitPhase ? 1.f : -1.f) : 0.f;
        float eYaw = 0.f, ePitch = 0.f;
        errFrom(g_plant.camYaw, g_plant.camPitch, e.yaw, e.pitch + jit, eYaw, ePitch);
        EspBox b{};
        b.id = e.id;
        b.ally = false;
        b.distance = 25.f;
        const float cx = 1230.f + eYaw * kPxPerDeg;
        const float cy = 540.f - ePitch * kPxPerDeg;
        b.x1 = cx - 30.f; b.x2 = cx + 30.f;
        b.y1 = cy - 90.f; b.y2 = cy + 10.f;
        for (int k = 0; k < 3; ++k) {
            b.aim_valid[k] = true;
            b.aim_yaw[k] = eYaw; b.aim_pitch[k] = ePitch;
            b.aim_pts[k][0] = cx; b.aim_pts[k][1] = cy;
        }
        g_boxes.push_back(b);
    }
}

const std::vector<EspBox>& FrameBoxes(float, float) { return g_boxes; }
static float g_testFovRadius = 1e6f;
float AimFovRadiusPx(float, float) { return g_testFovRadius; }

float esp_camera_fov_deg() { return 60.f; }
bool esp_local_player_is_aiming() { return true; }
int  esp_local_aim_source() { return 1; }
// Настоящей оси камеры на этом «устройстве» нет (cam_st = 0 в логах) — тач
// живёт в режиме учёта «в полёте».
bool esp_aim_camera_angles(float&, float&) { return false; }
// Базис проекции (матрица вида) существует и без оси — на нём нарисован бокс.
bool esp_aim_projection_basis(float& yaw, float& pitch) {
    yaw = g_plant.camYaw; pitch = g_plant.camPitch;
    return true;
}
bool esp_read_look_sensitivity(float& out) {
    if (!g_plant.sensOk) return false;
    out = g_plant.sens;
    return true;
}

void esp_get_ally_diag(int* a, int* b, int* c, int* d, int* e, int* f, int* g,
                       int* h, int* i, int* j, int* k, int* l, int* m,
                       unsigned long long* n, unsigned long long* o,
                       char* team, size_t cap) {
    if (a) *a = 0;
    if (b) *b = 0;
    if (c) *c = 0;
    if (d) *d = 0;
    if (e) *e = 0;
    if (f) *f = 0;
    if (g) *g = 0;
    if (h) *h = 0;
    if (i) *i = 0;
    if (j) *j = 0;
    if (k) *k = -1;
    if (l) *l = -1;
    if (m) *m = 0;
    if (n) *n = 0;
    if (o) *o = 0;
    if (team && cap) team[0] = '\0';
}
void esp_get_ally_diag_details(char* out, size_t cap) { if (out && cap) out[0] = '\0'; }

#include "ui_util.h"   // WrapDeg180

// Настоящая тач-петля (см. run.sh): дробь экрана пальца, коэффициент из
// чувствительности, AimTarget, общий блок (выбор цели + поток) и весь
// UpdateAimTouch со статиками.
#include "touch_ctrl.inc"

// ====================== прогон ==============================================
struct Scenario {
    const char* name;
    float startYaw = 30.f;      // ошибка захвата, град
    float ratio = 1.f;          // gTrue / гейн из настройки (1.45 — настройка занижена)
    double pipe = 0.20;         // задержка трубы, с
    float tickDt = 1.f / 60.f;  // темп оверлея
    int   decoys = 0;           // болванок кроме цели (>= 3 боксов — есть поток)
    float trackVel = 0.f;       // скорость цели с trackFrom, град/с
    float trackFrom = 1e9f;
    float jitStart = 1e9f;      // с этой секунды кость дрожит +-0.3°
    float retargetAt = 1e9f;    // в этот момент цель сменяется (второй захват)
    float secs = 6.f;
    bool  longFrames = false;   // каждый седьмой такт dt = 0.11 с
    // ожидания (NaN/-1 — не проверять)
    float wantOvershoot = -1.f; // перелёт после первого перехода через ноль, град
    int   wantFlips = -1;       // смен знака ошибки после первой
    float wantSettle = -1.f;    // с, с которой ошибка <= 0.5° навсегда
    float wantTrail = -1.f;     // средняя |ошибка| за последние 2 с
    float wantTurn = -1.f;      // макс ход камеры за такт в последние 2 с
    float wantOvershoot2 = -1.f;
    int   wantFlips2 = -1;
    float wantSettle2 = -1.f;
};

struct Metrics {
    float firstCross = -1.f, overshoot = 0.f, settle = -1.f, trail = 0.f, maxTurn = 0.f;
    int   flips = 0;
    float firstCross2 = -1.f, overshoot2 = 0.f, settle2 = -1.f;
    int   flips2 = 0;
    int   steps = 0, ctrlFlips = 0, skipNoFrame = 0, skipLong = 0;
    float errAvg = 0.f, errMax = 0.f, stepAvg = 0.f, trim = 0.f;
};

// Разбор одной фазы [t0, t1) (захват или повторный): скользящий знак с
// мёртвой полосой 0.3° — качели мельче глазом не видны.
static void phaseMetrics(const std::vector<float>& t, const std::vector<float>& err,
                         float t0, float t1, float startSign,
                         float& firstCross, float& overshoot, int& flips, float& settle) {
    firstCross = -1.f; overshoot = 0.f; flips = 0; settle = -1.f;
    int lastSign = (startSign > 0.f) ? 1 : -1;
    bool crossed = false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] < t0 || t[i] >= t1) continue;
        const float e = err[i];
        if (fabsf(e) > 0.3f) {
            const int s = (e > 0.f) ? 1 : -1;
            if (s != lastSign) {
                if (!crossed) { crossed = true; firstCross = t[i]; }
                else ++flips;
                lastSign = s;
            }
        }
        if (crossed && e * startSign < 0.f && fabsf(e) > overshoot) overshoot = fabsf(e);
        if (fabsf(e) > 0.5f) settle = -1.f;
        else if (settle < 0.f) settle = t[i] - t0;
    }
    if (!crossed) overshoot = 0.f;
}

static Metrics runScenario(const Scenario& s) {
    Metrics m;
    g_state = AppState{};
    g_state.aim_touch = true;
    g_state.aim_mode = AIM_MODE_TOUCH;
    g_state.gun_str = 5.f;          // полный лок («Скорость» 5 из 10)
    g_plant = Plant{};
    g_plant.pipe = s.pipe;
    g_plant.gTrue = 0.10f * s.ratio;

    g_world.clear();
    g_world.push_back({1, s.startYaw, 0.f});
    for (int i = 0; i < s.decoys; ++i) {
        // Болванки обязаны стоять ДАЛЬШЕ от прицела, чем цель: выбор цели
        // берёт ближайшую к перекрестию, и бокс на 2-8° от центра честно
        // побеждал бы цель на 30° (стенд так и ловил «застревание на 28°» —
        // это был не зацикленный контроллер, а аим на болванке).
        const float off = (s.startYaw + 7.f) * ((i % 2) ? 1.f : -1.f) * (1.f + 0.13f * i);
        g_world.push_back({10 + (unsigned long long)i, off, (i % 2 ? 5.f : -4.f)});
    }

    std::vector<float> tHist, errHist, camHist;
    const float frameDt = 1.f / 60.f;
    double nextFrame = frameDt;
    double t = 0.0;
    bool retargeted = false;
    float retargetYaw = -0.6f * s.startYaw;   // повторный захват в другую сторону

    while (t < s.secs) {
        float dt = s.tickDt;
        if (s.longFrames && ((int)(t / s.tickDt) % 7) == 3) dt = 0.11f;
        while (nextFrame <= t + 1e-9) { g_plant.frame(nextFrame); nextFrame += frameDt; }
        if (t >= s.trackFrom) g_world[0].yaw += s.trackVel * dt;
        if (!retargeted && t >= s.retargetAt) {
            // Новая цель (другой id) в другой стороне: обязан перезахватить
            // так же чисто, как первую.
            retargeted = true;
            g_world[0].id = 77; g_world[0].yaw = retargetYaw; g_world[0].pitch = 2.f;
        }
        g_jitAmp = (t >= s.jitStart) ? 0.3f : 0.f;
        g_now = t;
        snapshot();
        // Физическая ошибка (без дрожи кости) — то, что видит игрок.
        float errY = 0.f, errP = 0.f;
        errFrom(g_plant.camYaw, g_plant.camPitch, g_world[0].yaw, g_world[0].pitch, errY, errP);
        tHist.push_back((float)t); errHist.push_back(errY); camHist.push_back(g_plant.camYaw);
        if (getenv("AIM_TRACE") && (s.ratio > 1.4f || s.trackVel != 0.f))
            printf("      t=%.2f err=%7.2f cam=%8.2f tgt=%8.2f буст=%.2f vel=%6.2f палец=%s%.0f,%.0f\n",
                   t, (double)errY, (double)g_plant.camYaw, (double)g_world[0].yaw,
                   (double)AimTouchBoost(), (double)AimTouchVelYaw(),
                   s_fingerDown ? "" : "(нет) ", (double)g_plant.fx, (double)g_plant.fy);
        UpdateAimTouch(dt);
        t += dt;
    }

    const float split = retargeted ? s.retargetAt : (float)s.secs;
    phaseMetrics(tHist, errHist, 0.f, split, s.startYaw,
                 m.firstCross, m.overshoot, m.flips, m.settle);
    if (retargeted)
        phaseMetrics(tHist, errHist, split, (float)s.secs, retargetYaw,
                     m.firstCross2, m.overshoot2, m.flips2, m.settle2);
    {
        double sum = 0.0; int n = 0;
        float mt = 0.f;
        for (size_t i = 0; i < tHist.size(); ++i) {
            if (tHist[i] >= (float)s.secs - 2.f) {
                sum += fabsf(errHist[i]); ++n;
                if (i > 0) {
                    const float turn = fabsf(camHist[i] - camHist[i - 1]);
                    if (turn > mt) mt = turn;
                }
            }
        }
        m.trail = n ? (float)(sum / n) : 0.f;
        m.maxTurn = mt;
    }
    AimTouchDiagFetch(nullptr, &m.steps, &m.ctrlFlips, &m.skipNoFrame, &m.skipLong,
                      &m.errAvg, &m.errMax, &m.stepAvg, nullptr, nullptr, &m.trim, nullptr);
    return m;
}

static int g_fail = 0;
static void check(bool ok, const char* what) {
    printf("    %-58s %s\n", what, ok ? "ок" : "ПРОВАЛ");
    if (!ok) g_fail = 1;
}

static void printAndCheck(const Scenario& s, const Metrics& m) {
    printf("  %s\n", s.name);
    printf("    захват: перелёт %.2f°, реверсов %d, settle %s",
           m.overshoot, m.flips, m.settle >= 0.f ? "" : "НЕТ ");
    if (m.settle >= 0.f) printf("%.2f с", m.settle);
    printf(" | шлейф %.2f°, ход камеры %.2f°, шагов %d, трим %.2f\n",
           m.trail, m.maxTurn, m.steps, m.trim);
    if (s.wantOvershoot >= 0.f)
        check(m.overshoot <= s.wantOvershoot, "перелёт захвата в норме");
    if (s.wantFlips >= 0)
        check(m.flips <= s.wantFlips, "маятника нет (реверсы в норме)");
    if (s.wantSettle >= 0.f)
        check(m.settle >= 0.f && m.settle <= s.wantSettle, "захват быстрый");
    if (s.wantTrail >= 0.f)
        check(m.trail <= s.wantTrail, "шлейф ведения в норме");
    if (s.wantTurn >= 0.f)
        check(m.maxTurn <= s.wantTurn, "камера у цели стоит (не охотится)");
    if (s.retargetAt < 1e8f) {
        printf("    повтор: перелёт %.2f°, реверсов %d, settle %s",
               m.overshoot2, m.flips2, m.settle2 >= 0.f ? "" : "НЕТ ");
        if (m.settle2 >= 0.f) printf("%.2f с", m.settle2);
        printf("\n");
        if (s.wantOvershoot2 >= 0.f)
            check(m.overshoot2 <= s.wantOvershoot2, "повторный захват чистый");
        if (s.wantFlips2 >= 0)
            check(m.flips2 <= s.wantFlips2, "повторный захват без реверсов");
        if (s.wantSettle2 >= 0.f)
            check(m.settle2 >= 0.f && m.settle2 <= s.wantSettle2, "повторный захват быстрый");
    }
}

int main() {
    const Scenario list[] = {
        // 1. База жалобы: дуэль 1 на 1 (потока нет), труба 0.20 с, оверлей 60 fps.
        {"дуэль (без потока), труба 0.20 с", 30.f, 1.f, 0.20, 1.f / 60.f, 0,
         0.f, 1e9f, 1e9f, 1e9f, 6.f, false,
         1.0f, 1, 1.6f, 0.35f, -1.f},
        // 2. Худшая труба (8 кадров при 30 fps).
        {"дуэль, труба 0.27 с", 30.f, 1.f, 0.27, 1.f / 60.f, 0,
         0.f, 1e9f, 1e9f, 1e9f, 6.f, false,
         1.5f, 1, 1.9f, 0.35f, -1.f},
        // 3. Настройка занижена (коэффициенту нужно выше потолка 1.4): на
        //    первом захвате пара качков неизбежна (растение неизвестно), зато
        //    буст по доказательству реверса гасит её за 1-2 качка, а второй
        //    захват обязан быть чистым.
        {"дуэль, гейн занижен 1.45x, два захвата", 30.f, 1.45f, 0.20, 1.f / 60.f, 0,
         0.f, 1e9f, 1e9f, 3.0f, 7.f, false,
         15.0f, 4, 2.8f, 0.4f, -1.f,
         2.5f, 2, 2.8f},
        // 4. Завышенная настройка: медленнее, но без маха через цель.
        {"дуэль, гейн завышен 0.7x", 30.f, 0.7f, 0.20, 1.f / 60.f, 0,
         0.f, 1e9f, 1e9f, 1e9f, 6.f, false,
         0.8f, 1, 3.0f, 0.35f, -1.f},
        // 5. Регресс поточного пути: 5 боксов, поток виден, труба длинная.
        {"5 боксов (поток), труба 0.27 с", 30.f, 1.f, 0.27, 1.f / 60.f, 4,
         0.f, 1e9f, 1e9f, 1e9f, 6.f, false,
         1.5f, 1, 1.9f, 0.35f, -1.f},
        // 6. Ведение бегущей цели 20°/с без потока:
        {"дуэль, ведение 20°/с с 0.8 с", 30.f, 1.f, 0.20, 1.f / 60.f, 0,
         20.f, 0.8f, 1e9f, 1e9f, 6.f, false,
         -1.f, 4, -1.f, 4.0f, -1.f},
        // 7. Дрожь кости у взятой цели: камера стоит, никакой «охоты».
        //    (Перелёт захвата чуть шире: вид контроллера подмешивает дрожь.)
        {"дуэль, дрожь +-0.3° с 1.5 с", 20.f, 1.f, 0.20, 1.f / 60.f, 0,
         0.f, 1e9f, 1.5f, 1e9f, 6.f, false,
         1.6f, 1, 1.6f, 0.35f, 0.15f},
        // 8. Темп устройства из логов: оверлей 12 fps (81 мс), дуэль.
        {"дуэль, оверлей 12 fps, труба 0.20 с", 30.f, 1.f, 0.20, 0.081f, 0,
         0.f, 1e9f, 1e9f, 1e9f, 6.f, false,
         1.2f, 2, 1.9f, 0.35f, -1.f},
        // 9. Рваные кадры под нагрузкой: каждый седьмой такт 0.11 с. Темп
        //    заказов падает до ~1/(0.11+труба) — захват сериализуется, так что
        //    времени ему нужно больше; маятника быть не должно совсем.
        {"дуэль, рваные кадры", 30.f, 1.f, 0.20, 1.f / 60.f, 0,
         0.f, 1e9f, 1e9f, 1e9f, 6.f, true,
         1.5f, 1, 2.6f, 0.35f, -1.f},
    };

    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            g_fail = 0;
            Metrics m = runScenario(list[i]);
            printAndCheck(list[i], m);
            fflush(stdout);          // _exit буфер не сливает
            _exit(g_fail ? 1 : 0);
        }
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) g_fail = 1;
    }
    if (g_fail) printf("  ЕСТЬ ПРОВАЛЫ\n");
    return g_fail;
}
