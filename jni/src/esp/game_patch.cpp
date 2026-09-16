// game_patch.cpp — Запись в память игры: X-ray и «всегда день».
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке game_patch.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/aim_points.h"
#include "esp/boxes.h"
#include "esp/camera.h"
#include "esp/farm_scan.h"
#include "esp/farm_target.h"
#include "esp/frame.h"
#include "esp/il2cpp.h"
#include "esp/managed.h"
#include "esp/marker_labels.h"
#include "esp/markers.h"
#include "esp/math.h"
#include "esp/melee.h"
#include "esp/mem.h"
#include "esp/player_pose.h"
#include "esp/skeleton_build.h"
#include "esp/transform.h"
#include "game_patch.h"

// Маска ресурсов автофарма (bit0 дерево..bit3 сера).
unsigned g_farm_mask = 0;

bool vec3_is_finite(const Vec3& value);

// ==== X-ray: камера отсекает всё ближе N метров (near clip plane) ==========
// Пишется прямо в native Camera каждый кадр, пока включено; при выключении
// восстанавливается исходное значение. Смена камеры игрой обрабатывается —
// старой камере возвращается её клип, новой сохраняется свой.
static float    g_xray_meters = 0.0F;      // 0 = выключено

uint64_t g_xray_cam = 0;

static float    g_xray_saved_near = 0.1F;

bool     g_xray_saved_valid = false;

void esp_set_xray(float meters) {
    if (!std::isfinite(meters) || meters < 0.0F) meters = 0.0F;
    if (meters > 50.0F) meters = 50.0F;
    g_xray_meters = meters;
}

void xray_apply(uint64_t native_cam) {
    if (!native_cam) return;
    if (g_xray_meters > 0.05F) {
        if (g_xray_cam != native_cam || !g_xray_saved_valid) {
            // Другая камера: вернуть клип прежней, запомнить клип новой.
            if (g_xray_saved_valid && g_xray_cam)
                wr_buf(g_xray_cam + CAMERA_NEAR_CLIP, &g_xray_saved_near, sizeof(float));
            float current = rd<float>(native_cam + CAMERA_NEAR_CLIP);
            g_xray_saved_near = (std::isfinite(current) && current > 0.0001F && current < 5.0F)
                              ? current : 0.1F;
            g_xray_saved_valid = true;
            g_xray_cam = native_cam;
        }
        wr_buf(native_cam + CAMERA_NEAR_CLIP, &g_xray_meters, sizeof(float));
    } else if (g_xray_saved_valid) {
        if (g_xray_cam)
            wr_buf(g_xray_cam + CAMERA_NEAR_CLIP, &g_xray_saved_near, sizeof(float));
        g_xray_saved_valid = false;
        g_xray_cam = 0;
    }
}

// ==== Всегда день: TOD_Sky (ассет Time Of Day) ==============================
// Инстанс ищется сканом области TypeInfo-слотов и валидируется структурой
// (см. TOD_* в game_offsets.h): статик-список инстансов -> элемент того же
// класса -> Cycle -> Hour/Day/Month/Year в разумных пределах. Пока включено,
// фоновый поток пишет полдень прямо в Cycle.Hour — игровой Update сам
// разворачивает солнце.
std::string read_remote_string(uint64_t address, bool* readable); // определена ниже

static bool     g_day_enabled = false;

uint64_t g_day_tod = 0;          // подтверждённый инстанс TimeOfDay

std::atomic<uint64_t> g_day_cycle_addr{0}; // Cycle.Hour для писателя

static std::atomic<bool>     g_day_writer_running{false};

int      g_day_retry = 0;

void esp_set_always_day(bool enabled) { g_day_enabled = enabled; }

void always_day_tick() {
    if (!g_day_enabled) {
        // Выключили: писатель замолкает (адрес в 0), часы игры идут сами.
        g_day_cycle_addr.store(0);
        g_day_tod = 0;
        return;
    }
    if (!g_il2cpp_base || g_pid <= 0) return;
    if (g_day_tod) {
        // Живучесть: инстанс мог умереть при перезагрузке мира.
        uint64_t cyc = rd_ptr(g_day_tod + TOD_SKY_CYCLE);
        float hour = (cyc >= 0x10000) ? rd<float>(cyc + TOD_CYCLE_HOUR) : -1.0F;
        if (!(std::isfinite(hour) && hour >= 0.0F && hour <= 24.0F)) g_day_tod = 0;
    }
    // g_day_tod здесь — объект TOD_Sky (небесный менеджер ассета Time of
    // Day). Имя его класса обфусцировано и РОТИРУЕТ каждый билд: в дампе
    // 89e0b63 это был "IY", в 62a8534 — "UV" (в ещё более старом — "Gq").
    // Прежний Oxide.TimeOfDay в боевых сценах не существует: его ленивые
    // метадата-слоты так и не инициализированы (лог day_log: нечётные токены)
    // — Awake ни разу не вызывался. TOD_Sky ищем по СИГНАТУРЕ, без имён: у его
    // класса первое статик-поле — список инстансов List<Self>; элемент списка —
    // объект того же класса; у объекта по TOD_SKY_CYCLE (0x40) лежит
    // TOD_CycleParameters с полями Hour(float 0..24)/Day(1..31)/Month(1..12)/
    // Year(1900..2100). Имя TOD_CycleParameters не обфусцировано — по нему
    // класс и находится в новом дампе (grep 'TOD_CycleParameters_o\* Cycle').
    if (!g_day_tod) {
        static uint64_t s_scan_rva = TOD_SCAN_RVA_BEGIN;
        // const, а не constexpr: значения приходят из активного набора
        // оффсетов (релиз/бета), а он переключается в рантайме.
        const uint64_t kScanEnd = TOD_SCAN_RVA_END;
        const uint64_t kCycleOff = TOD_SKY_CYCLE;
        uint64_t slots[128];
        if (rd_buf(g_il2cpp_base + s_scan_rva, slots, sizeof(slots))) {
            for (int i = 0; i < 128 && !g_day_tod; ++i) {
                uint64_t klass = slots[i];
                if (klass < 0x10000 || (klass & 0x7) != 0) continue;
                uint64_t statics = rd_ptr(klass + 0xB8);
                if (statics < 0x10000) continue;
                uint64_t list = rd_ptr(statics);      // static List<Gq> instances
                if (list < 0x10000) continue;
                uint64_t items = rd_ptr(list + 0x10); // List._items
                int32_t size = rd<int32_t>(list + 0x18);
                if (items < 0x10000 || size <= 0 || size > 4) continue;
                uint64_t sky = rd_ptr(items + 0x20);  // [0]
                if (sky < 0x10000) continue;
                if (rd_ptr(sky) != klass) continue;   // элемент — того же класса
                uint64_t cyc = rd_ptr(sky + kCycleOff);
                if (cyc < 0x10000) continue;
                float hour = rd<float>(cyc + TOD_CYCLE_HOUR);
                int day = rd<int32_t>(cyc + TOD_CYCLE_DAY);
                int mon = rd<int32_t>(cyc + TOD_CYCLE_MONTH);
                int year = rd<int32_t>(cyc + TOD_CYCLE_YEAR);
                if (std::isfinite(hour) && hour >= 0.0F && hour <= 24.0F &&
                    day >= 1 && day <= 31 && mon >= 1 && mon <= 12 &&
                    year >= 1900 && year <= 2100)
                    g_day_tod = sky;
            }
        }
        s_scan_rva += 128 * 8;
        if (s_scan_rva >= kScanEnd) s_scan_rva = TOD_SCAN_RVA_BEGIN;
        if (!g_day_tod) return;
    }
    // Полдень: писатель-доминатор. Игровой писатель обновляет Cycle.Hour
    // каждый кадр, и запись раз в кадр оверлея с ним гонялась — отсюда
    // миллисекундные проблески старого времени. Теперь час пишет фоновый
    // поток с периодом ~2 мс: окно, в котором игра успевает и записать своё
    // время, и отрендерить его, практически исчезает.
    {
        uint64_t cyc = rd_ptr(g_day_tod + TOD_SKY_CYCLE);
        if (cyc >= 0x10000) {
            g_day_cycle_addr.store(cyc + TOD_CYCLE_HOUR);
            if (!g_day_writer_running.exchange(true)) {
                std::thread([]() {
                    while (g_day_writer_running.load()) {
                        uint64_t addr = g_day_cycle_addr.load();
                        if (addr && g_day_enabled && g_pid > 0) {
                            float noon = 12.0F;
                            wr_buf(addr, &noon, sizeof(float));
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                }).detach();
            }
        } else {
            g_day_tod = 0; // объект умер (смена сцены) — переискать
            g_day_cycle_addr.store(0);
        }
    }
}
