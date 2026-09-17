// aim_mem.cpp — Мемори-аим: поворот прицела записью в память игры.
//
// Модуль разрезан из прежнего монолита? Нет — он новый. Появился как второй
// режим аима (вкладка «Аим»: «Тач» / «Мемори»), потому что тач-аим водит палец
// через /dev/uinput:
//   * на части устройств инъекция не поднимается вовсе (нет доступа к uinput),
//     и аим просто не работает;
//   * где поднимается — поворот идёт через палец: квант цифровера, мёртвая зона
//     зоны наведения, зависимость от чувствительности игры в настройках игрока.
// Мемори-аим ничего этого не касается: он двигает прицел там, где игра сама
// хранит поворот, — то есть тем же кодом, которым игра поворачивает камеру от
// касания, только без касания.
//
// ГЛАВНОЕ: куда писать — не угадывается, а ИЗМЕРЯЕТСЯ. Раскладка MouseLook
// между сборками игры разъезжается (её и не публикуют), поэтому модуль по
// очереди пробует дорожки, пишет ими пробный доворот на пару градусов и
// смотрит по настоящим углам прицела, повернулся ли прицел ровно на столько.
// Дорожка принимается только после такого замера; если ни одна не подтвердилась
// — режим честно сообщает «не поддерживается» и меню показывает причину, а аим
// остаётся на тач-режиме (см. aim/update.cpp).
//
// Дорожки, в порядке проверки:
//
//   1. STATE_QUAT / STATE_DEG / STATE_RAD — в самом MouseLook лежит текущий
//      поворот: кватернион либо пара float (yaw, pitch) в градусах или
//      радианах. Пишем туда АБСОЛЮТНЫЕ углы прицела: игра сама применит их к
//      камере и к оси выстрела (MouseLook.Update пишет LookDirection из
//      m_LookRoot.forward каждый кадр). Поля ищутся разовым чтением области
//      объекта: кандидат — кватернион, который разворачивает (0,0,1) в текущую
//      ось прицела, либо пара float, равная текущим углам. Поиск только читает.
//
//   2. INPUT — поле накопленного за такт сдвига взгляда (MOUSE_LOOK_ACCUM_OFFSET,
//      +0x88; рядом +0x8C — вторая ось). Игра читает это поле, умножает на
//      m_Sensitivity (+0x34) и применяет к повороту (разбор в camera.cpp, RVA
//      0x64e312c). Дорожка добавляет в поле свою дельту в «единицах ввода», а
//      град/единицу выучивает на самотесте: пишем известную дельту и меряем
//      получившийся доворот. Это тот же путь, которым идёт касание, поэтому
//      поведение камеры остаётся игровым (сглаживание, ограничения), а палец,
//      экран и uinput не участвуют.
//
//   3. TRANSFORM — поворот узла прицела (MouseLook.m_LookRoot) в иерархии
//      Transform. Узел ищется чтением полей MouseLook: берём тот, чья мировая
//      поза совпадает с осью прицела и точкой глаза. Пишем локальный
//      кватернион (родительский поворот учитывается, см.
//      read_transform_parent_world_rotation) — та же запись, которую читает поза.
//
// Что модуль отдаёт наружу — esp_mem_aim_state/path/reason (меню показывает
// словами), esp_mem_aim_read_angles и esp_mem_aim_apply (ими работает
// контроллер aim/memory.cpp). Никаких своих решений о том, куда целиться,
// модуль не принимает: это дело контроллера.
#include "esp/common.h"
#include "app/diag_log.h"   // подтверждённая дорожка и приговоры — в журнал
#include "esp/aim_mem.h"
#include "esp/camera.h"
#include "esp/frame.h"
#include "esp/il2cpp.h"
#include "esp/math.h"
#include "esp/mem.h"
#include "esp/transform.h"

// ---- Углы прицела: та же система, что у esp_aim_camera_angles и фарма -------
// yaw — вокруг мировой вертикали (atan2(x, z)), pitch — вверх. Поэтому вектор
// оси прицела из углов и углы из вектора считаются здесь ровно так же, как в
// esp/frame.cpp: иначе мемори-аим целился бы в другое место экрана.

static constexpr float kRadToDeg = 57.29577951f;
static constexpr float kDegToRad = 0.01745329252f;

static Vec3 forward_from_angles(float yaw_deg, float pitch_deg) {
    const float yaw = yaw_deg * kDegToRad;
    const float pitch = pitch_deg * kDegToRad;
    const float cos_pitch = cosf(pitch);
    return {sinf(yaw) * cos_pitch, sinf(pitch), cosf(yaw) * cos_pitch};
}

static bool angles_from_forward(const Vec3& f, float& yaw_deg, float& pitch_deg) {
    if (!vec3_is_finite(f)) return false;
    const float horizontal = sqrtf(f.x * f.x + f.z * f.z);
    if (!(horizontal > 1e-4f) && fabsf(f.y) < 1e-4f) return false;
    yaw_deg = atan2f(f.x, f.z) * kRadToDeg;
    pitch_deg = atan2f(f.y, horizontal) * kRadToDeg;
    return std::isfinite(yaw_deg) && std::isfinite(pitch_deg);
}

static float wrap180(float angle) {
    while (angle > 180.f) angle -= 360.f;
    while (angle < -180.f) angle += 360.f;
    return angle;
}

static float signed_angle_diff(float a, float b) { return wrap180(a - b); }

// Кватернион, переводящий вектор `from` в `to` (оба — направления, не позиции).
static Vec4 quaternion_between(const Vec3& from, const Vec3& to) {
    const float from_len = sqrtf(from.x * from.x + from.y * from.y + from.z * from.z);
    const float to_len = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z);
    if (!(from_len > 1e-5f) || !(to_len > 1e-5f)) return {0.f, 0.f, 0.f, 1.f};
    Vec3 a = {from.x / from_len, from.y / from_len, from.z / from_len};
    Vec3 b = {to.x / to_len, to.y / to_len, to.z / to_len};
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot > 0.999999f) return {0.f, 0.f, 0.f, 1.f};
    if (dot < -0.999999f) {
        // Разворот на 180 градусов: ось любая перпендикулярная.
        Vec3 axis = (fabsf(a.x) < 0.9f) ? cross_product(a, {1.f, 0.f, 0.f})
                                        : cross_product(a, {0.f, 1.f, 0.f});
        Vec4 rotation = {axis.x, axis.y, axis.z, 0.f};
        normalize_quaternion(rotation);
        return rotation;
    }
    Vec4 rotation = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 1.f + dot};
    normalize_quaternion(rotation);
    return rotation;
}

static Vec4 quaternion_inverse(const Vec4& q) { return {-q.x, -q.y, -q.z, q.w}; }

// ---- Состояние модуля ------------------------------------------------------

static int      s_state = MEM_AIM_IDLE;
static int      s_path = MEM_PATH_NONE;
static int      s_reason = MEM_REASON_NONE;

static uint64_t s_mouse_look = 0;     // разрешённый объект MouseLook
static uint64_t s_look_root = 0;      // managed Transform узла прицела (для TRANSFORM)
static uint64_t s_state_field = 0;    // адрес поля поворота в MouseLook (для STATE_*)
static bool     s_pinned_pair_path = false;  // дорожка — углы из дампа (0x4C)
// Знаки пары углов: в игре значение = sign * угол. Знак выясняется замером на
// пробе (см. PROBE_VERIFY) — по дампу видно, что пара хранит рыскание и тангаж,
// но не видно, в какой стороне положительное направление.
static float    s_pair_yaw_sign = 1.f;
static float    s_pair_pitch_sign = 1.f;
static int      s_pair_sign_retry = 0;

// ---- Углы прицела MouseLook: источник и приёмник (проверено по дампу) ------
//
// Разбор дампа 205619 (dump.cs + libil2cpp, RVA ниже) и dump_beta по Oxide.MouseLook:
//   0x28  m_LookRoot   (UnityEngine.Transform) — узел прицела, его игра и
//                      поворачивает: Oxide.MouseLook$$ZJo (RVA 0x64e312c) в конце
//                      зовёт Quaternion.Euler(углы) и Transform.set_localRotation
//                      на [this+0x28];
//   0x4C  Vector2 углов прицела (x — рыскание, y — тангаж): функция складывает
//                      туда шаг (углы += delta*sensitivity), нормализует рыскание
//                      в [-180,180) и клэмпит тангаж, и только ПОТОМ строит из них
//                      поворот узла. То есть поле — единственная «настоящая»
//                      память углов, а поворот узла каждый такт пересчитывается
//                      игрой.
// Почему это важно для мемори-режима: писать в узел прицела (доржка «узел
// камеры») — это гонка за фазой кадра: игра перезапишет поворот на следующем
// такте, и запись подтверждается только если мы успели между её записью и
// чтением камерой (в логе устройства это «дорожка не подтвердилась (причина 4)»).
// Запись в сами углы игры такой гонки не имеет: мы подставляем игре готовое
// значение, и она сама доворачивает узел — как будто игрок повёл мышью.
//
// Смещения (в camera.h рядом с прочими полями MouseLook) одинаковы в релизе и в
// бете (проверено по обоим дампам), поэтому берём их как проверенную дорожку, а
// не как догадку.

static int      s_state_range = 0;    // 0: yaw в [-180,180], 1: [0,360)
static float    s_deg_per_unit = 0.f; // INPUT: град доворота на единицу поля
static float    s_deg_per_unit_pitch = 0.f;

// Самотест: пробный доворот и цена кадра.
static constexpr float kProbeYawDeg = 2.0f;
static constexpr float kProbePitchDeg = 1.0f;
static constexpr float kProbeInputUnits = 12.f;    // единиц поля на пробе INPUT
static constexpr int   kProbeApplyFrames = 3;      // даём игре применить запись
static constexpr int   kProbeSettleFrames = 2;     // и убедиться, что доворот встал
static constexpr float kProbeToleranceDeg = 0.6f;  // допуск на измеренный доворот
static constexpr float kProbeRunawayDeg = 0.6f;    // «поехало дальше» = не наша дорожка

// Сторож: сколько кадров подряд углы не совпадают с заказанными, прежде чем
// объявить, что игра перестала принимать запись (смена мира, респавн).
static constexpr int   kLostFramesLimit = 45;
static constexpr float kLostToleranceDeg = 3.0f;

static float s_commanded_yaw = 0.f;
static float s_commanded_pitch = 0.f;
static bool  s_have_command = false;
static int   s_lost_frames = 0;

// Отложенная перепроверка после отказа и после потери записи.
static float s_retry_timer = 0.f;
static bool  s_ever_ready = false;    // дорожка хоть раз подтверждалась
static bool  s_had_success = false;   // хоть один кандидат прошёл проверку доворотом
static constexpr float kRetrySeconds = 15.f;      // долгий повтор, когда сорвалась READY
static constexpr float kRetryFastSeconds = 2.f;   // быстрый повтор первого самотеста

// Бюджет самотеста. Пока он идёт, аим сознательно не трогает камеру (пробный
// доворот меряется по настоящему повороту прицела), поэтому у самотеста обязан
// быть предел: на устройстве, где MouseLook не находится или углы не читаются,
// «подбираю…» не должно висеть вечно — иначе переключение в «Мемори» убило бы
// аим целиком. Не уложились — объявляем отказ с последней причиной, и аим
// продолжает работать тач-веткой.
static float s_probe_elapsed = 0.f;
static constexpr float kProbeBudgetSeconds = 6.f;

// ---- Самотест --------------------------------------------------------------

enum ProbeStep {
    PROBE_FIND = 0,     // найти MouseLook и прочитать текущие углы
    PROBE_SCAN,         // разовое чтение области MouseLook: где лежит поворот
    PROBE_APPLY,        // записать пробный доворот выбранной дорожкой
    PROBE_WAIT,         // ждём, пока игра его применит
    PROBE_VERIFY,       // измерить доворот и решить: наша дорожка или нет
    PROBE_SETTLE,       // убедиться, что доворот встал и не поехал дальше
    PROBE_NEXT,         // следующая дорожка
    PROBE_FINISH,       // всё проверено
};

struct TestCandidate {
    int      path = MEM_PATH_NONE;   // STATE_QUAT / STATE_DEG / STATE_RAD
    uint64_t field = 0;              // адрес поля в MouseLook
};

static constexpr int kMaxCandidates = 3;
static TestCandidate s_candidates[kMaxCandidates];
static int      s_candidate_count = 0;
static int      s_candidate_index = 0;
static int      s_probe_step = PROBE_FIND;
static int      s_probe_frames = 0;
static float    s_probe_yaw_before = 0.f;
static float    s_probe_pitch_before = 0.f;
static float    s_probe_expected_yaw = 0.f;
static float    s_probe_expected_pitch = 0.f;

// Что записали на пробе — чтобы вернуть как было, если дорожка не подтвердится.
static uint8_t  s_probe_saved[16] = {};
static int      s_probe_saved_size = 0;
static float    s_probe_input_units = 0.f;   // сколько единиц влили в INPUT

// Углы прицела откуда только можно. Приоритет — своя дорожка: у STATE_* и
// TRANSFORM это ровно то, что игра держит для поворота. Дальше — ось выстрела
// или поза камеры (esp_aim_camera_angles), и в самом конце — углы камеры с
// базисом из матрицы вида: он отстаёт на кадр, но мемори-аиму это не мешает
// (шаг всё равно считается от полного остатка ошибки), а на части устройств
// только он и читается.
// Углы игры читает блок ниже (он же их и пишет) — здесь только объявление:
// read_current_angles стоит выше и первым делом спрашивает именно их.
static bool read_mouse_look_angles(uint64_t mouse_look, float& yaw_deg, float& pitch_deg);

static bool read_current_angles(float& yaw_deg, float& pitch_deg) {
    if (s_state_field && (s_path == MEM_PATH_STATE_QUAT || s_path == MEM_PATH_STATE_DEG ||
                          s_path == MEM_PATH_STATE_RAD)) {
        if (s_path == MEM_PATH_STATE_QUAT) {
            Vec4 rotation{};
            if (rd_exact(s_state_field, rotation)) {
                if (normalize_quaternion(rotation) &&
                    angles_from_forward(rotate_vector(rotation, {0.f, 0.f, 1.f}), yaw_deg, pitch_deg))
                    return true;
            }
        } else {
            float stored[2] = {0.f, 0.f};
            if (rd_exact(s_state_field, stored[0]) && rd_exact(s_state_field + sizeof(float), stored[1]) &&
                std::isfinite(stored[0]) && std::isfinite(stored[1])) {
                if (s_path == MEM_PATH_STATE_RAD) { stored[0] *= kRadToDeg; stored[1] *= kRadToDeg; }
                yaw_deg = stored[0];
                pitch_deg = stored[1];
                if (std::isfinite(yaw_deg) && std::isfinite(pitch_deg)) return true;
            }
        }
    }
    // Углы самой игры: единственный источник, которому камера не нужна вовсе.
    // Именно из-за его отсутствия мемори-режим и молчал «не находит поворот
    // камеры» в те секунды, когда кадр ESP не собирался (см. xvcen_health.log).
    if (read_mouse_look_angles(s_mouse_look, yaw_deg, pitch_deg)) return true;
    if (s_path == MEM_PATH_TRANSFORM && s_look_root) {
        Vec4 local{}, parent{};
        if (read_transform_local_rotation(s_look_root, local) &&
            read_transform_parent_world_rotation(s_look_root, parent)) {
            const Vec4 world = multiply_quaternion(parent, local);
            if (angles_from_forward(rotate_vector(world, {0.f, 0.f, 1.f}), yaw_deg, pitch_deg)) return true;
        }
    }
    if (esp_aim_camera_angles(yaw_deg, pitch_deg)) return true;
    return esp_camera_angles(yaw_deg, pitch_deg);
}

// ---- Запись выбранной дорожкой --------------------------------------------

static bool read_mouse_look_floats(uint64_t field, float& yaw_value, float& pitch_value) {
    if (!field) return false;
    if (!rd_exact(field, yaw_value)) return false;
    if (!rd_exact(field + sizeof(float), pitch_value)) return false;
    return std::isfinite(yaw_value) && std::isfinite(pitch_value);
}

// Углы для записи приводим к той же «ветке», в которой поле держит игру: одни
// сборки считают yaw в [-180,180], другие в [0,360). Ветку запомнили на пробе.
static float to_stored_yaw(float yaw_deg) {
    if (s_state_range == 1) {
        while (yaw_deg < 0.f) yaw_deg += 360.f;
        while (yaw_deg >= 360.f) yaw_deg -= 360.f;
        return yaw_deg;
    }
    return wrap180(yaw_deg);
}

// ---- Углы прицела в MouseLook: чтение, запись, свидетель -------------------

// Текущие углы прямо из состояния игры (поле 0x4C). Камеру для этого читать не
// нужно — именно поэтому дорожка работает и тогда, когда кадр ESP не собрался
// («мемори-аим: не находит поворот камеры» в логе устройства случалось ровно в
// те секунды, когда камера не читалась).
static bool read_mouse_look_angles(uint64_t mouse_look, float& yaw_deg, float& pitch_deg) {
    if (!mouse_look) return false;
    float pair[2] = {0.f, 0.f};
    const uint64_t field = mouse_look + MOUSE_LOOK_ANGLES_OFFSET;
    if (!read_mouse_look_floats(field, pair[0], pair[1])) return false;
    if (fabsf(pair[0]) > 100000.f || fabsf(pair[1]) > 89.9f) return false;  // тангаж игра держит в своих пределах
    yaw_deg = wrap180(s_pair_yaw_sign * pair[0]);
    pitch_deg = s_pair_pitch_sign * pair[1];
    return std::isfinite(yaw_deg) && std::isfinite(pitch_deg);
}

// Записать абсолютные углы в ту же пару и с теми же знаками — это то же, что
// делает Oxide.MouseLook$$ZJo, складывая туда шаг от ввода: пара и есть
// «настоящие» углы прицела, из которых игра строит поворот узла (0x64e3408).
static bool write_mouse_look_angles(uint64_t mouse_look, float yaw_deg, float pitch_deg) {
    if (!mouse_look) return false;
    const uint64_t field = mouse_look + MOUSE_LOOK_ANGLES_OFFSET;
    const float stored_yaw = to_stored_yaw(s_pair_yaw_sign * yaw_deg);
    const float stored_pitch = s_pair_pitch_sign * pitch_deg;
    if (!std::isfinite(stored_yaw) || !std::isfinite(stored_pitch)) return false;
    if (!wr_buf(field, &stored_yaw, sizeof(float))) return false;
    return wr_buf(field + sizeof(float), &stored_pitch, sizeof(float));
}

// Свидетель: направление, по которому видно, что игра ПРИМЕНИЛА запись. Камера
// (матрицы ESP или поза выстрела) — лучший свидетель, но её может не быть;
// тогда смотрим на узел прицела: игра строит его поворот из этих же углов
// каждый такт, поэтому если запись игра проигнорировала — узел не повернётся,
// и обмануть проверку чтением того же поля не выйдет.
static bool read_look_root_angles(uint64_t mouse_look, float& yaw_deg, float& pitch_deg) {
    if (!mouse_look) return false;
    uint64_t root = resolve_native_transform(rd_ptr(mouse_look + MOUSE_LOOK_LOOK_ROOT_OFFSET));
    if (!root) return false;
    Vec4 local{}, parent{};
    if (!read_transform_local_rotation(root, local)) return false;
    if (!read_transform_parent_world_rotation(root, parent)) return false;
    const Vec4 world = multiply_quaternion(parent, local);
    return angles_from_forward(rotate_vector(world, {0.f, 0.f, 1.f}), yaw_deg, pitch_deg);
}

// Свидетель для самотеста: сначала камера, потом узел прицела.
static bool read_witness_angles(float& yaw_deg, float& pitch_deg) {
    if (esp_aim_camera_angles(yaw_deg, pitch_deg)) return true;
    if (esp_camera_angles(yaw_deg, pitch_deg)) return true;
    return read_look_root_angles(s_mouse_look, yaw_deg, pitch_deg);
}

// Идёт ли проверка дорожки углов из дампа (0x4C): от этого зависит, кого
// спрашивать свидетелем — см. read_measured_angles.
static void note_probing_pinned_pair() {
    s_pinned_pair_path = s_candidate_index < s_candidate_count &&
                         s_candidates[s_candidate_index].path == MEM_PATH_STATE_DEG &&
                         s_candidates[s_candidate_index].field == s_mouse_look + MOUSE_LOOK_ANGLES_OFFSET;
}

// Углы для ЗАМЕРА пробного доворота. Первым — свидетель (камера, иначе узел
// прицела): узел игра перестраивает из углов каждый такт, поэтому он доказывает,
// что запись применила именно игра, а не что мы записали поле и сами же его
// прочли. Если свидетеля нет вовсе (на устройстве не читается ни камера, ни
// иерархия Transform — в логе это первые секунды после привязки), замеряем по
// самому полю: оно состояние игры, и если игра его не затирает своим значением,
// значит доворот по нему игра применит (иначе значение вернулось бы прежним за
// такт). Так дорожка остаётся рабочей и без кадра ESP — это и было «не находит
// поворот камеры».
static bool read_measured_angles(float& yaw_deg, float& pitch_deg) {
    // Для дорожки углов первым свидетелем идёт узел прицела: игру он устраивает
    // ровно так, как она сама его строит из этих углов, а камера при
    // прицеливании подмешивается к оружию и повернулась бы меньше записанного —
    // пробный доворот забраковался бы на ровном месте.
    if (s_pinned_pair_path && read_look_root_angles(s_mouse_look, yaw_deg, pitch_deg)) return true;
    if (read_witness_angles(yaw_deg, pitch_deg)) return true;
    return read_current_angles(yaw_deg, pitch_deg);
}

static bool path_apply(int path, float yaw_deg, float pitch_deg) {
    if (!s_mouse_look) return false;
    if (path == MEM_PATH_STATE_DEG &&
        s_state_field == s_mouse_look + MOUSE_LOOK_ANGLES_OFFSET)
        return write_mouse_look_angles(s_mouse_look, yaw_deg, pitch_deg);
    if (path == MEM_PATH_STATE_QUAT) {
        Vec4 rotation{};
        if (!s_state_field || !rd_exact(s_state_field, rotation)) return false;
        if (!normalize_quaternion(rotation)) return false;
        const Vec3 current = rotate_vector(rotation, {0.f, 0.f, 1.f});
        const Vec4 delta = quaternion_between(current, forward_from_angles(yaw_deg, pitch_deg));
        const Vec4 updated = multiply_quaternion(delta, rotation);
        Vec4 write_value = updated;
        if (!normalize_quaternion(write_value)) return false;
        return wr_buf(s_state_field, &write_value, sizeof(Vec4));
    }
    if (path == MEM_PATH_STATE_DEG || path == MEM_PATH_STATE_RAD) {
        float yaw_value = to_stored_yaw(yaw_deg);
        float pitch_value = pitch_deg > 89.f ? 89.f : (pitch_deg < -89.f ? -89.f : pitch_deg);
        if (path == MEM_PATH_STATE_RAD) { yaw_value *= kDegToRad; pitch_value *= kDegToRad; }
        if (!s_state_field) return false;
        if (!wr_buf(s_state_field, &yaw_value, sizeof(float))) return false;
        return wr_buf(s_state_field + sizeof(float), &pitch_value, sizeof(float));
    }
    if (path == MEM_PATH_INPUT) {
        // Дорожка ввода: поле — это накопленный сдвиг, а не угол. Значит пишем
        // ДЕЛЬТУ, переведённую в единицы поля по выученному град/единицу.
        if (!(fabsf(s_deg_per_unit) > 1e-4f)) return false;
        float current_yaw = 0.f, current_pitch = 0.f;
        if (!read_current_angles(current_yaw, current_pitch)) return false;
        const float delta_yaw_deg = signed_angle_diff(yaw_deg, current_yaw);
        const float delta_pitch_deg = pitch_deg - current_pitch;
        float yaw_units = delta_yaw_deg / s_deg_per_unit;
        float pitch_units = delta_pitch_deg / (fabsf(s_deg_per_unit_pitch) > 1e-4f ? s_deg_per_unit_pitch
                                                                                   : s_deg_per_unit);
        // Ограничение на одну запись: даже если единицы выучены с ошибкой,
        // камера не уедет на пол-экрана (контроллер всё равно шагает по
        // несколько градусов за такт).
        const float max_units = 400.f;
        if (yaw_units > max_units) yaw_units = max_units;
        if (yaw_units < -max_units) yaw_units = -max_units;
        if (pitch_units > max_units) pitch_units = max_units;
        if (pitch_units < -max_units) pitch_units = -max_units;

        float accumulated_yaw = 0.f, accumulated_pitch = 0.f;
        if (!read_mouse_look_floats(s_mouse_look + MOUSE_LOOK_ACCUM_OFFSET, accumulated_yaw, accumulated_pitch))
            return false;
        if (fabsf(accumulated_yaw) > 100000.f || fabsf(accumulated_pitch) > 100000.f) return false;
        accumulated_yaw += yaw_units;
        accumulated_pitch += pitch_units;
        if (!wr_buf(s_mouse_look + MOUSE_LOOK_ACCUM_OFFSET, &accumulated_yaw, sizeof(float))) return false;
        return wr_buf(s_mouse_look + MOUSE_LOOK_ACCUM_OFFSET + sizeof(float), &accumulated_pitch, sizeof(float));
    }
    if (path == MEM_PATH_TRANSFORM) {
        if (!s_look_root) return false;
        Vec4 local{}, parent{};
        if (!read_transform_local_rotation(s_look_root, local)) return false;
        if (!read_transform_parent_world_rotation(s_look_root, parent)) return false;
        const Vec4 world = multiply_quaternion(parent, local);
        const Vec3 current = rotate_vector(world, {0.f, 0.f, 1.f});
        const Vec4 delta = quaternion_between(current, forward_from_angles(yaw_deg, pitch_deg));
        // Желаемый мировой поворот -> локальный: L' = P^-1 * (delta * P * L).
        const Vec4 desired_world = multiply_quaternion(delta, world);
        Vec4 updated = multiply_quaternion(quaternion_inverse(parent), desired_world);
        if (!normalize_quaternion(updated)) return false;
        return write_transform_local_rotation(s_look_root, updated);
    }
    return false;
}

// ---- Поиск полей поворота в MouseLook (только чтение) ----------------------

static void scan_state_fields(uint64_t mouse_look, const Vec3& aim_forward,
                              float yaw_now, float pitch_now) {
    s_candidate_count = 0;
    float best_quat_dot = 0.97f;
    float best_deg_error = 3.0f;
    float best_rad_error = 3.0f;
    TestCandidate best_quat{}, best_deg{}, best_rad{};

    // Область полей объекта. Заголовок (klass, monitor) не читаем, дальше идём
    // по 4 байта: поля выравнены, а промах на 4 байта даёт мусор, который
    // отсеивают проверки ниже.
    for (uint64_t offset = 0x10; offset <= 0x200; offset += 4) {
        const uint64_t field = mouse_look + offset;

        // Кандидат-кватернион: единичный и разворачивает (0,0,1) в текущую ось.
        Vec4 rotation{};
        if (rd_exact(field, rotation) && std::isfinite(rotation.x) && std::isfinite(rotation.y) &&
            std::isfinite(rotation.z) && std::isfinite(rotation.w)) {
            const float norm = sqrtf(rotation.x * rotation.x + rotation.y * rotation.y +
                                     rotation.z * rotation.z + rotation.w * rotation.w);
            if (norm > 0.99f && norm < 1.01f) {
                const Vec3 forward = rotate_vector(rotation, {0.f, 0.f, 1.f});
                const float dot = forward.x * aim_forward.x + forward.y * aim_forward.y +
                                  forward.z * aim_forward.z;
                if (dot > best_quat_dot) {
                    best_quat_dot = dot;
                    best_quat = {MEM_PATH_STATE_QUAT, field};
                }
            }
        }

        // Кандидат-пара float: углы в градусах или радианах, совпадающие с
        // текущими. Точка-цель — 0.5 градуса, иначе это не углы прицела.
        float first = 0.f, second = 0.f;
        if (!read_mouse_look_floats(field, first, second)) continue;
        if (fabsf(first) > 100000.f || fabsf(second) > 100000.f) continue;
        const float pitch_deg = wrap180(second);          // тангаж в обоих вариантах
        if (fabsf(pitch_deg) <= 89.5f && fabsf(signed_angle_diff(first, yaw_now)) < best_deg_error &&
            fabsf(pitch_deg - pitch_now) < best_deg_error) {
            const float error = fabsf(signed_angle_diff(first, yaw_now)) + fabsf(pitch_deg - pitch_now);
            best_deg_error = error;
            best_deg = {MEM_PATH_STATE_DEG, field};
        }
        const float first_deg = first * kRadToDeg, second_deg = second * kRadToDeg;
        if (fabsf(first) <= 3.2f && fabsf(second) <= 1.6f &&
            fabsf(signed_angle_diff(first_deg, yaw_now)) < best_rad_error &&
            fabsf(second_deg - pitch_now) < best_rad_error) {
            const float error = fabsf(signed_angle_diff(first_deg, yaw_now)) + fabsf(second_deg - pitch_now);
            best_rad_error = error;
            best_rad = {MEM_PATH_STATE_RAD, field};
        }
    }

    // Порядок проверки: сначала кватернион (самая правдоподобная и самая
    // дешёвая дорожка), затем углы в градусах и радианах, затем ввод и узел
    // прицела — они проверяются всегда, даже без кандидатов из скана.
    if (best_quat.path != MEM_PATH_NONE && s_candidate_count < kMaxCandidates)
        s_candidates[s_candidate_count++] = best_quat;
    if (best_deg.path != MEM_PATH_NONE && s_candidate_count < kMaxCandidates)
        s_candidates[s_candidate_count++] = best_deg;
    if (best_rad.path != MEM_PATH_NONE && s_candidate_count < kMaxCandidates)
        s_candidates[s_candidate_count++] = best_rad;
}

// Узел прицела: managed Transform, чья мировая поза совпадает с осью прицела и
// точкой глаза. Ищем среди полей MouseLook — так же чтением, как и поля углов.
static uint64_t find_look_root(uint64_t mouse_look, const Vec3& aim_forward) {
    // Сначала — узел из дампа: Oxide.MouseLook.m_LookRoot (0x28). Смещение
    // проверено и в релизе, и в бете, поэтому скан ниже остаётся страховкой на
    // будущие сборки: раньше узел искался только перебором полей, и на
    // перезагрузке мира, когда в объекте мигали мусорные указатели, его можно
    // было не найти вовсе — «не находит поворот камеры».
    const uint64_t hint = rd_ptr(mouse_look + MOUSE_LOOK_LOOK_ROOT_OFFSET);
    if (hint >= 0x10000 && hint < 0x0001000000000000ULL && (hint & 0x7) == 0) {
        const uint64_t native = resolve_native_transform(hint);
        Vec4 local{}, parent{};
        if (native && read_transform_local_rotation(native, local) &&
            read_transform_parent_world_rotation(native, parent)) {
            const Vec4 world = multiply_quaternion(parent, local);
            Vec3 forward = rotate_vector(world, {0.f, 0.f, 1.f});
            const float length = sqrtf(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
            if (length > 0.5f && length < 1.5f) {
                forward = {forward.x / length, forward.y / length, forward.z / length};
                const float dot = forward.x * aim_forward.x + forward.y * aim_forward.y +
                                  forward.z * aim_forward.z;
                if (dot >= 0.995f) return native;
            }
        }
    }
    for (uint64_t offset = 0x10; offset <= 0x200; offset += 8) {
        const uint64_t value = rd_ptr(mouse_look + offset);
        if (value < 0x10000 || value >= 0x0001000000000000ULL || (value & 0x7) != 0) continue;
        const uint64_t native = resolve_native_transform(value);
        if (!native) continue;
        Vec4 local{}, parent{};
        if (!read_transform_local_rotation(native, local)) continue;
        if (!read_transform_parent_world_rotation(native, parent)) continue;
        const Vec4 world = multiply_quaternion(parent, local);
        Vec3 forward = rotate_vector(world, {0.f, 0.f, 1.f});
        const float length = sqrtf(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
        if (!(length > 0.5f && length < 1.5f)) continue;
        forward = {forward.x / length, forward.y / length, forward.z / length};
        const float dot = forward.x * aim_forward.x + forward.y * aim_forward.y + forward.z * aim_forward.z;
        // Ось самого узла и ось прицела (ось выстрела) расходятся максимум на
        // качку/отдачу — это доли градуса, поэтому порог строгий.
        if (dot < 0.995f) continue;
        return native;
    }
    return 0;
}

// ---- Самотест: шаг за шагом ------------------------------------------------

static void save_probe_value(uint64_t field, int size) {
    s_probe_saved_size = size;
    if (!field || size <= 0 || size > (int)sizeof(s_probe_saved)) { s_probe_saved_size = 0; return; }
    if (!rd_buf(field, s_probe_saved, (size_t)size)) s_probe_saved_size = 0;
}

static void restore_probe_value(uint64_t field) {
    if (!field || s_probe_saved_size <= 0) return;
    wr_buf(field, s_probe_saved, (size_t)s_probe_saved_size);
    s_probe_saved_size = 0;
}

// Какая дорожка проверяется на этом шаге: сначала найденные поля поворота,
// затем ввод (записью в поле накопленного сдвига) и узел прицела.
static int probe_path_at(int index) {
    if (index < s_candidate_count) return s_candidates[index].path;
    if (index == s_candidate_count) return MEM_PATH_INPUT;
    return MEM_PATH_TRANSFORM;
}

static void probe_finish_unsupported(int reason) {
    const bool ever_ready = s_ever_ready;
    s_state = MEM_AIM_UNSUPPORTED;
    s_path = MEM_PATH_NONE;
    s_reason = reason;
    // Первый самотест мог сорваться просто потому, что мир ещё грузился (позиции
    // не читались, углы не менялись). Тогда ждать 15 секунд нельзя: это и есть
    // жалоба «мемори начинает наводиться не сразу, а через время». Пока дорожка
    // ни разу не подтверждалась — пробуем снова через 2 секунды, после срыва
    // рабочей дорожки — как раньше, через 15 (тому, что работало, за 2 секунды
    // не восстановиться, а лишние записи в игру ни к чему).
    s_retry_timer = (ever_ready || s_had_success) ? kRetrySeconds : kRetryFastSeconds;
    s_have_command = false;
    s_lost_frames = 0;
}

static void probe_accept(int path) {
    s_state = MEM_AIM_READY;
    s_ever_ready = true;
    s_path = path;
    s_reason = MEM_REASON_NONE;
    s_probe_elapsed = 0.f;
    s_probe_step = PROBE_FIND;
    s_have_command = false;
    s_lost_frames = 0;
}

// Одна дорожка: записать пробный доворот и запомнить, что вернуть.
static void probe_begin_candidate() {
    s_probe_step = PROBE_APPLY;
    s_probe_frames = 0;
    s_probe_input_units = 0.f;
    s_state_range = 0;
    if (s_candidate_index >= s_candidate_count) return;   // ввод или узел прицела
    const TestCandidate& candidate = s_candidates[s_candidate_index];
    s_state_field = candidate.field;

    // Ветка для yaw: смотрим, в каком диапазоне поле держит угол.
    if (candidate.path == MEM_PATH_STATE_DEG) {
        float stored_yaw = 0.f, stored_pitch = 0.f;
        if (read_mouse_look_floats(candidate.field, stored_yaw, stored_pitch) && stored_yaw < 0.f)
            s_state_range = 1;   // поле уже ушло в плюс — значит ветка [0,360)
    }

    if (candidate.path == MEM_PATH_STATE_QUAT) save_probe_value(candidate.field, (int)sizeof(Vec4));
    else if (candidate.path == MEM_PATH_STATE_DEG ||
             candidate.path == MEM_PATH_STATE_RAD) save_probe_value(candidate.field, (int)(sizeof(float) * 2));
}

// Применить пробный доворот той дорожкой, что проверяем сейчас.
static bool probe_apply_current() {
    const int path = probe_path_at(s_candidate_index);
    const float target_yaw = wrap180(s_probe_yaw_before + kProbeYawDeg);
    const float target_pitch = s_probe_pitch_before + kProbePitchDeg;
    s_probe_expected_yaw = kProbeYawDeg;
    s_probe_expected_pitch = kProbePitchDeg;
    if (path == MEM_PATH_INPUT) {
        // Ввод меряется в единицах поля: ожидаемый доворот заранее неизвестен
        // (град/единицу выучиваем по этому же замеру), поэтому пишем известное
        // число единиц и смотрим, что получилось.
        if (!s_mouse_look) return false;
        float accumulated_yaw = 0.f, accumulated_pitch = 0.f;
        if (!read_mouse_look_floats(s_mouse_look + MOUSE_LOOK_ACCUM_OFFSET, accumulated_yaw, accumulated_pitch))
            return false;
        if (fabsf(accumulated_yaw) > 100000.f || fabsf(accumulated_pitch) > 100000.f) return false;
        const float yaw_units = kProbeInputUnits;
        const float pitch_units = kProbeInputUnits * 0.5f;
        if (!wr_buf(s_mouse_look + MOUSE_LOOK_ACCUM_OFFSET, &(accumulated_yaw += yaw_units), sizeof(float)))
            return false;
        if (!wr_buf(s_mouse_look + MOUSE_LOOK_ACCUM_OFFSET + sizeof(float), &(accumulated_pitch += pitch_units), sizeof(float)))
            return false;
        s_probe_input_units = yaw_units;
        s_probe_expected_yaw = 0.f;     // ожидание: «хоть что-то», см. PROBE_VERIFY
        s_probe_expected_pitch = 0.f;
        return true;
    }
    if (path == MEM_PATH_TRANSFORM) {
        // Узел возвращаем не «как было», а доворотом назад: локальный поворот
        // пишем заново, поэтому отдельного сохранения не нужно (см. probe_undo).
        if (!s_look_root) return false;
        Vec4 local{};
        if (!read_transform_local_rotation(s_look_root, local)) return false;
    }
    return path_apply(path, target_yaw, target_pitch);
}

static void probe_next_candidate() {
    ++s_candidate_index;
    if (s_candidate_index > s_candidate_count) {
        // Учтены все: STATE_* (сколько нашлось), INPUT, TRANSFORM.
        probe_finish_unsupported(s_reason == MEM_REASON_NONE ? MEM_REASON_NO_FIELD : s_reason);
        return;
    }
    probe_begin_candidate();
}

// Вернуть прицел как было, если дорожка не подтвердилась.
static void probe_undo() {
    const int path = probe_path_at(s_candidate_index);
    if (path == MEM_PATH_INPUT) {
        if (s_mouse_look && fabsf(s_probe_input_units) > 1e-4f) {
            float accumulated_yaw = 0.f, accumulated_pitch = 0.f;
            if (read_mouse_look_floats(s_mouse_look + MOUSE_LOOK_ACCUM_OFFSET, accumulated_yaw, accumulated_pitch)) {
                accumulated_yaw -= s_probe_input_units;
                accumulated_pitch -= s_probe_input_units * 0.5f;
                wr_buf(s_mouse_look + MOUSE_LOOK_ACCUM_OFFSET, &accumulated_yaw, sizeof(float));
                wr_buf(s_mouse_look + MOUSE_LOOK_ACCUM_OFFSET + sizeof(float), &accumulated_pitch, sizeof(float));
            }
            s_probe_input_units = 0.f;
        }
        return;
    }
    if (path == MEM_PATH_TRANSFORM) {
        if (s_look_root) path_apply(MEM_PATH_TRANSFORM, s_probe_yaw_before, s_probe_pitch_before);
        return;
    }
    if (s_candidate_index < s_candidate_count) restore_probe_value(s_candidates[s_candidate_index].field);
}

// Включили «Мемори»: подбирать дорожку сразу, а не «когда-нибудь потом».
// Отдельно от esp_mem_aim_reset: тот зовётся и на смене мира, и при перепривязке,
// где ждать 15 секунд после отказа как раз разумно. Здесь другое — человек
// только что включил режим и ждёт наведения сейчас.
//
// s_had_success не сбрасывается: он про то, принимала ли игра наши записи в этом
// процессе вообще, а это не зависит от того, сколько раз мы начинали подбор.
void esp_mem_aim_power_on() {
    esp_mem_aim_reset();
    s_retry_timer = 0.f;
}

void esp_mem_aim_reset() {
    s_state = MEM_AIM_IDLE;
    s_path = MEM_PATH_NONE;
    s_reason = MEM_REASON_NONE;
    s_mouse_look = 0;
    s_look_root = 0;
    s_state_field = 0;
    s_pinned_pair_path = false;
    s_pair_yaw_sign = 1.f;
    s_pair_pitch_sign = 1.f;
    s_pair_sign_retry = 0;
    s_state_range = 0;
    s_deg_per_unit = 0.f;
    s_deg_per_unit_pitch = 0.f;
    s_candidate_count = 0;
    s_candidate_index = 0;
    s_probe_step = PROBE_FIND;
    s_probe_frames = 0;
    s_probe_elapsed = 0.f;
    s_probe_saved_size = 0;
    s_have_command = false;
    s_lost_frames = 0;
    s_retry_timer = 0.f;
    // s_state_field обнулён выше: объект MouseLook живёт в мире, после смены мира
    // он другой, и адрес поля углов надо подтверждать заново.
}

void esp_mem_aim_tick(float dt) {
    if (!(dt > 0.f) || !std::isfinite(dt)) dt = 1.f / 60.f;

    // Готов: следим, что игра продолжает принимать нашу запись. Смена мира или
    // респавн ломают адреса — тогда заново самотест, а не тихая пустота.
    if (s_state == MEM_AIM_READY) {
        if (s_have_command) {
            float yaw_now = 0.f, pitch_now = 0.f;
            // Сначала узел прицела: он поворачивается только если игра приняла
            // запись. Поле углов — запасной вариант (по нему видно, что игра
            // затёрла наше значение своим).
            const bool have_now = read_look_root_angles(s_mouse_look, yaw_now, pitch_now) ||
                                  read_current_angles(yaw_now, pitch_now);
            if (have_now) {
                const float yaw_miss = fabsf(signed_angle_diff(s_commanded_yaw, yaw_now));
                const float pitch_miss = fabsf(s_commanded_pitch - pitch_now);
                if (yaw_miss > kLostToleranceDeg || pitch_miss > kLostToleranceDeg) {
                    if (++s_lost_frames > kLostFramesLimit) {
                        diag_log("aim", "мемори-режим: запись перестала доворачивать прицел (расхождение %.1f° по рысканию, %.1f° по тангажу, путь %d)",
                                 (double)yaw_miss, (double)pitch_miss, s_path);
                        s_state = MEM_AIM_PROBING;
                        s_path = MEM_PATH_NONE;
                        s_reason = MEM_REASON_WRITE_LOST;
                        s_probe_step = PROBE_FIND;
                        s_candidate_count = 0;
                        s_candidate_index = 0;
                        s_have_command = false;
                        s_lost_frames = 0;
                    }
                } else {
                    s_lost_frames = 0;
                }
            }
        }
        return;
    }

    if (s_state == MEM_AIM_UNSUPPORTED) {
        s_retry_timer -= dt;
        if (s_retry_timer > 0.f) return;
        s_state = MEM_AIM_PROBING;      // обстановка могла измениться (мир, сборка)
        s_probe_step = PROBE_FIND;
        s_candidate_count = 0;
        s_candidate_index = 0;
        s_reason = MEM_REASON_NONE;
    }

    if (s_state == MEM_AIM_IDLE) s_state = MEM_AIM_PROBING;

    // Предел по времени на весь самотест (см. kProbeBudgetSeconds).
    s_probe_elapsed += dt;
    if (s_probe_elapsed > kProbeBudgetSeconds) {
        probe_finish_unsupported(s_reason == MEM_REASON_NONE ? MEM_REASON_NO_FIELD : s_reason);
        return;
    }

    float yaw_now = 0.f, pitch_now = 0.f;
    const bool have_angles = read_current_angles(yaw_now, pitch_now);

    switch (s_probe_step) {
        case PROBE_FIND: {
            s_mouse_look = esp_resolve_mouse_look();
            if (!s_mouse_look) { s_reason = MEM_REASON_NO_MOUSE_LOOK; return; }
            if (!have_angles) { s_reason = MEM_REASON_NO_ANGLES; return; }
            s_probe_step = PROBE_SCAN;
            return;
        }
        case PROBE_SCAN: {
            if (!have_angles) { s_reason = MEM_REASON_NO_ANGLES; s_probe_step = PROBE_FIND; return; }
            const Vec3 aim_forward = forward_from_angles(yaw_now, pitch_now);
            scan_state_fields(s_mouse_look, aim_forward, yaw_now, pitch_now);
            // Углы самой игры (MouseLook 0x4C) — первой дорожкой: игра строит
            // поворот узла прицела именно из них, поэтому запись сюда не гонка, а
            // готовое значение (см. разбор в шапке блока). Смещение взято из дампа
            // и одинаково в релизе и бете, но подтверждаем его значением: в поле
            // обязаны лежать текущие углы прицела.
            float pair_yaw = 0.f, pair_pitch = 0.f;
            if (read_mouse_look_floats(s_mouse_look + MOUSE_LOOK_ANGLES_OFFSET, pair_yaw, pair_pitch) &&
                std::isfinite(pair_yaw) && std::isfinite(pair_pitch) &&
                fabsf(pair_yaw) <= 100000.f && fabsf(pair_pitch) <= 89.9f) {
                // Значение пары сверяем с углами прицела только для журнала:
                // совпасть оно может и не совпасть (у пары своё начало отсчёта и
                // знаки), но поле проверено дампом, а проверять дорожку будет
                // замер — как и любую другую. Раньше требовалось совпадение в
                // 3°, и дорожка отбрасывалась ещё до пробы.
                const bool looks_like_aim =
                    fabsf(signed_angle_diff(s_pair_yaw_sign * pair_yaw, yaw_now)) < 3.0f &&
                    fabsf(s_pair_pitch_sign * pair_pitch - pitch_now) < 3.0f;
                for (int i = 0; i < s_candidate_count; ++i) {
                    if (s_candidates[i].field != s_mouse_look + MOUSE_LOOK_ANGLES_OFFSET) continue;
                    for (int k = i; k + 1 < s_candidate_count; ++k) s_candidates[k] = s_candidates[k + 1];
                    --s_candidate_count;
                    break;
                }
                if (s_candidate_count >= kMaxCandidates) --s_candidate_count;
                for (int i = s_candidate_count; i > 0; --i) s_candidates[i] = s_candidates[i - 1];
                s_candidates[0] = {MEM_PATH_STATE_DEG, s_mouse_look + MOUSE_LOOK_ANGLES_OFFSET};
                ++s_candidate_count;
                if (!looks_like_aim)
                    diag_log("aim", "мемори-режим: углы игры (%0.1f, %0.1f) не совпали с углами прицела (%0.1f, %0.1f) — проверяю дорожку замером",
                             (double)(s_pair_yaw_sign * pair_yaw), (double)(s_pair_pitch_sign * pair_pitch),
                             (double)yaw_now, (double)pitch_now);
            }
            s_look_root = find_look_root(s_mouse_look, aim_forward);
            s_probe_yaw_before = yaw_now;
            s_probe_pitch_before = pitch_now;
            s_candidate_index = 0;
            // Даже без кандидатов есть что проверить: ввод и узел прицела.
            probe_begin_candidate();
            return;
        }
        case PROBE_APPLY: {
            if (!have_angles) { s_reason = MEM_REASON_NO_ANGLES; s_probe_step = PROBE_FIND; return; }
            s_probe_yaw_before = yaw_now;
            s_probe_pitch_before = pitch_now;
            const bool applied = probe_apply_current();
            s_probe_frames = kProbeApplyFrames;
            s_probe_step = applied ? PROBE_WAIT : PROBE_NEXT;
            return;
        }
        case PROBE_WAIT: {
            if (--s_probe_frames > 0) return;
            s_probe_step = PROBE_VERIFY;
            return;
        }
        case PROBE_VERIFY: {
            const int path = probe_path_at(s_candidate_index);
            note_probing_pinned_pair();
            float verify_yaw = 0.f, verify_pitch = 0.f;
            if (!read_measured_angles(verify_yaw, verify_pitch)) {
                s_reason = MEM_REASON_NO_ANGLES; s_probe_step = PROBE_FIND; return;
            }
            const float measured_yaw = signed_angle_diff(verify_yaw, s_probe_yaw_before);
            const float measured_pitch = verify_pitch - s_probe_pitch_before;
            if (path == MEM_PATH_INPUT) {
                // Дорожка ввода: ожидание не в градусах, а в том, что доворот
                // вообще случился и в ту же сторону. Отсюда же выучиваем
                // град/единицу — это и есть «настройка чувствительности» игры,
                // измеренная напрямую, без логов и догадок.
                if (fabsf(measured_yaw) < 0.15f && fabsf(measured_pitch) < 0.1f) {
                    probe_undo();
                    probe_next_candidate();
                    return;
                }
                s_deg_per_unit = measured_yaw / s_probe_input_units;
                s_deg_per_unit_pitch = (fabsf(s_probe_input_units) > 1e-4f)
                                           ? measured_pitch / (s_probe_input_units * 0.5f)
                                           : s_deg_per_unit;
                if (!std::isfinite(s_deg_per_unit) || fabsf(s_deg_per_unit) < 0.01f ||
                    fabsf(s_deg_per_unit) > 5.f) {
                    probe_undo();
                    probe_next_candidate();
                    return;
                }
            } else {
                // Знак пары мог оказаться обратным: тогда камера повернулась ровно
                // в другую сторону. Это не «дорожка не годится», а неизвестный
                // знак — переворачиваем и меряем снова (не больше двух раз).
                if (s_pinned_pair_path && s_pair_sign_retry < 2) {
                    const bool yaw_flip = measured_yaw < 0.f &&
                        fabsf(fabsf(measured_yaw) - fabsf(s_probe_expected_yaw)) < kProbeToleranceDeg;
                    const bool pitch_flip = measured_pitch < 0.f &&
                        fabsf(fabsf(measured_pitch) - fabsf(s_probe_expected_pitch)) < kProbeToleranceDeg;
                    if (yaw_flip || pitch_flip) {
                        if (yaw_flip) s_pair_yaw_sign = -s_pair_yaw_sign;
                        if (pitch_flip) s_pair_pitch_sign = -s_pair_pitch_sign;
                        ++s_pair_sign_retry;
                        diag_log("aim", "мемори-режим: углы игры повернули прицел в обратную сторону (рыскание: %d, тангаж: %d) — переворачиваю знак и проверяю снова",
                                 yaw_flip ? 1 : 0, pitch_flip ? 1 : 0);
                        probe_undo();
                        s_probe_step = PROBE_SCAN;   // текущие углы и пара — по новой
                        return;
                    }
                }
                if (fabsf(measured_yaw - s_probe_expected_yaw) > kProbeToleranceDeg ||
                    fabsf(measured_pitch - s_probe_expected_pitch) > kProbeToleranceDeg) {
                    probe_undo();
                    probe_next_candidate();
                    return;
                }
            }
            s_probe_step = PROBE_SETTLE;
            s_probe_frames = kProbeSettleFrames;
            return;
        }
        case PROBE_SETTLE: {
            if (--s_probe_frames > 0) return;
            const int path = probe_path_at(s_candidate_index);
            note_probing_pinned_pair();
            float settle_yaw = 0.f, settle_pitch = 0.f;
            if (!read_measured_angles(settle_yaw, settle_pitch)) {
                s_reason = MEM_REASON_NO_ANGLES; s_probe_step = PROBE_FIND; return;
            }
            yaw_now = settle_yaw;
            pitch_now = settle_pitch;
            // Доворот обязан встать и не поехать дальше: поле, которое игра не
            // обнуляет, уводило бы камеру всё дальше — такая дорожка не годится.
            const float extra_yaw = fabsf(signed_angle_diff(yaw_now, s_probe_yaw_before));
            const float extra_pitch = fabsf(pitch_now - s_probe_pitch_before);
            const float limit_yaw = (path == MEM_PATH_INPUT ? 6.f : fabsf(s_probe_expected_yaw) + kProbeRunawayDeg);
            const float limit_pitch = (path == MEM_PATH_INPUT ? 4.f : fabsf(s_probe_expected_pitch) + kProbeRunawayDeg);
            if (extra_yaw > limit_yaw || extra_pitch > limit_pitch) {
                probe_undo();
                probe_next_candidate();
                return;
            }
            // Дорожка подтверждена замером — только теперь ей можно работать.
            const bool state_path = (path == MEM_PATH_STATE_QUAT || path == MEM_PATH_STATE_DEG ||
                                     path == MEM_PATH_STATE_RAD);
            s_state_field = state_path ? s_candidates[s_candidate_index].field : 0;
            s_had_success = true;   // игра приняла нашу запись: дорожка реальна
            if (path == MEM_PATH_STATE_DEG && s_state_field == s_mouse_look + MOUSE_LOOK_ANGLES_OFFSET)
                diag_log("aim", "мемори-режим: углы прицела игры подтверждены (MouseLook +%#x, доворот %.1f° по рысканию за %.1f с)",
                         (unsigned)MOUSE_LOOK_ANGLES_OFFSET, (double)extra_yaw, (double)s_probe_elapsed);
            // У STATE_* углы читаются из того же поля — ветка yaw уже запомнена
            // при заходе на дорожку (см. probe_begin_candidate).
            probe_accept(path);
            return;
        }
        case PROBE_NEXT: {
            probe_next_candidate();
            return;
        }
        default:
            s_probe_step = PROBE_FIND;
            return;
    }
}

int esp_mem_aim_state() { return s_state; }
int esp_mem_aim_path() { return s_path; }
int esp_mem_aim_reason() { return s_reason; }

bool esp_mem_aim_read_angles(float& yaw_deg, float& pitch_deg) {
    return read_current_angles(yaw_deg, pitch_deg);
}

bool esp_mem_aim_apply(float yaw_deg, float pitch_deg) {
    if (s_state != MEM_AIM_READY || s_path == MEM_PATH_NONE) return false;
    if (!std::isfinite(yaw_deg) || !std::isfinite(pitch_deg)) return false;
    if (pitch_deg > 89.f) pitch_deg = 89.f;
    if (pitch_deg < -89.f) pitch_deg = -89.f;
    if (!path_apply(s_path, yaw_deg, pitch_deg)) {
        // Запись перестала проходить: адреса умерли. Заново самотест, а не
        // молчаливое «аим включён, но ничего не делает».
        s_state = MEM_AIM_PROBING;
        s_path = MEM_PATH_NONE;
        s_reason = MEM_REASON_WRITE_LOST;
        s_probe_step = PROBE_FIND;
        s_candidate_count = 0;
        s_candidate_index = 0;
        s_have_command = false;
        return false;
    }
    s_commanded_yaw = yaw_deg;
    s_commanded_pitch = pitch_deg;
    s_have_command = true;
    return true;
}
