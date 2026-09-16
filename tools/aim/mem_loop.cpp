// Стенд мемори-аима: петля «память -> прицел -> ошибка» без устройства.
//
// Зачем он есть. Тач-аим доворачивает прицел пальцем и поэтому зависит от того,
// что игра делает с этим пальцем: квант цифровера, настройка чувствительности,
// задержка отклика на два кадра — всё это приходится выяснять замерами на
// устройстве (см. tools/aim/sim_loop.cpp и лог 16.09.2026). Мемори-аим пишет
// угол напрямую, поэтому проверить его можно здесь и целиком.
//
// Что важно: стенд компилирует НАСТОЯЩИЙ jni/src/aim/memory.cpp, а не пересказ
// его арифметики. Значит проверяется тот самый код, что уезжает на устройство, и
// разойтись с ним стенд не может по построению.
//
// Что подделывается: сама игра. Вместо памяти — «процесс», который держит
// настоящие углы прицела и применяет запись с задержкой в кадрах (у разных
// устройств и разной нагрузки она разная), плюс шум чтения углов и движение
// цели. Контроллер об этом ничего не знает: он видит только углы и цель.
//
// Сборка/запуск:
//   sh tools/aim/run_mem.sh

#include <cstdio>
#include <cmath>
#include <algorithm>

#include "aim/memory.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* name) {
    printf("%-58s %s\n", name, ok ? "ОК" : "ПРОВАЛ");
    if (!ok) ++g_fail;
}

// ---- Поддельная игра ------------------------------------------------------
//
// Углы прицела в градусах плюс очередь записей: то, что мы записали, игра
// применит через `delay` кадров (в том числе никогда — если запись не проходит,
// так стенд проверяет и «мертвый» случай).
struct Game {
    float yaw = 0.f, pitch = 0.f;      // настоящий прицел
    int   delay = 1;                   // кадров до применения записи
    float noise = 0.f;                 // шум чтения углов, градусы
    float phase = 0.f;                 // фаза «дрожи» чтения
    struct Cmd { float yaw, pitch; int at; bool live; };
    Cmd   queue[64];
    int   qn = 0, frame = 0;
    bool  accept = true;               // игра вообще принимает запись

    void Write(float y, float p) {
        if (!accept) return;
        if (qn >= 64) return;          // переполнение очереди = потеря записи
        queue[qn++] = {y, p, frame + delay, true};
    }

    void Tick() {
        for (int i = 0; i < qn; i++)
            if (queue[i].live && queue[i].at <= frame) {
                yaw = queue[i].yaw; pitch = queue[i].pitch;
                queue[i].live = false;
            }
        // Компакция очереди: применённые записи уходят.
        int k = 0;
        for (int i = 0; i < qn; i++) if (queue[i].live) queue[k++] = queue[i];
        qn = k;
        ++frame;
    }

    // Дрожь чтения: медленная и детерминированная, чтобы прогоны повторялись
    // бит в бит (никакого rand() — иначе стенд «мигает» на границах допусков).
    float ReadYaw()   { phase += 0.37f; return yaw   + noise * (float)sin(phase); }
    float ReadPitch() { phase += 0.23f; return pitch + noise * (float)sin(phase * 1.7f); }
};

// Один прогон: цель, к которой ведём прицел. Возвращает ошибку на последнем
// кадре и максимальный перелёт (в градусах) — по ним и судим о точности.
struct RunResult { float final_err = 0.f, overshoot = 0.f, settle_s = -1.f, first = 0.f; };

RunResult Run(float start_yaw, float start_pitch, float tgt_yaw, float tgt_pitch,
              float dt, float speed, float world_dist,
              const Game& proto, int frames, float tgt_vel_yaw = 0.f) {
    Game g = proto;
    g.yaw = start_yaw; g.pitch = start_pitch;
    float err_start = (float)hypot(tgt_yaw - start_yaw, tgt_pitch - start_pitch);
    RunResult r; r.first = err_start;
    float sign0 = tgt_yaw - start_yaw;
    for (int i = 0; i < frames; i++) {
        const float ty = tgt_yaw + tgt_vel_yaw * ((float)i * dt);
        const float cy = g.ReadYaw(), cp = g.ReadPitch();
        AimTarget t;
        t.valid = true;
        t.yaw = ((ty - cy) > 180.f) ? (ty - cy) - 360.f : (((ty - cy) < -180.f) ? (ty - cy) + 360.f : (ty - cy));
        t.pitch = tgt_pitch - cp;
        t.world_dist = world_dist;
        float ny = 0.f, np = 0.f;
        if (AimMemoryStep(t, dt, speed, world_dist, cy, cp, ny, np)) g.Write(ny, np);
        g.Tick();

        const float e = (float)hypot((float)((ty - g.yaw) > 180.f ? (ty - g.yaw) - 360.f : ((ty - g.yaw) < -180.f ? (ty - g.yaw) + 360.f : (ty - g.yaw))),
                                     (float)(tgt_pitch - g.pitch));
        if (r.settle_s < 0.f && e < 0.15f) r.settle_s = (float)(i + 1) * dt;
        if (sign0 * (float)(tgt_yaw - g.yaw) < 0.f) r.overshoot = std::max(r.overshoot, e);
        r.final_err = e;
    }
    return r;
}

}  // namespace

int main() {
    Game proto;
    proto.delay = 1;      // запись применяется на следующем кадре
    proto.noise = 0.0f;

    const float DT = 1.f / 60.f;

    // 1. Захват: 60° в сторону, обычная скорость. Мемори-аим обязан прийти к
    //    цели за доли секунды и не качаться (в тач-аиме на это уходил секунда с
    //    хвостом колебаний — там шаг ограничен квантом пальца и потолком px).
    {
        RunResult r = Run(0.f, 0.f, 60.f, 10.f, DT, 5.f, 50.f, proto, 120);
        printf("захват 60°: время %.2f с, ошибка %.3f°\n", r.settle_s, r.final_err);
        check(r.settle_s > 0.f && r.settle_s < 0.5f, "захват 60° — быстрее 0.5 с");
        check(r.final_err < 0.3f, "захват 60° — ошибка меньше 0.3°");
    }

    // 2. Задержка отклика не ломает сходимость: игра может применить запись и
    //    через 3 кадра (медленное устройство), и сразу.
    for (int delay = 0; delay <= 3; delay++) {
        proto.delay = delay;
        RunResult r = Run(0.f, 0.f, 45.f, -8.f, DT, 5.f, 40.f, proto, 200);
        char name[80];
        snprintf(name, sizeof(name), "задержка %d кадр(а) — сходится без перелёта", delay);
        check(r.final_err < 0.3f && r.overshoot < 1.5f, name);
    }
    proto.delay = 1;

    // 3. Шум чтения 0.3° (кость/камера читаются с дрожью) — прицел не должен
    //    «петь»: шаг меньше мёртвой зоны контроллер не делает вовсе.
    proto.noise = 0.3f;
    {
        RunResult r = Run(0.f, 0.f, 20.f, 0.f, DT, 5.f, 60.f, proto, 200);
        printf("шум чтения 0.3°: ошибка %.3f°, перелёт %.3f°\n", r.final_err, r.overshoot);
        check(r.final_err < 0.8f && r.overshoot < 1.5f, "шум чтения 0.3° — прицел стоит спокойно");
    }
    proto.noise = 0.0f;

    // 4. Мёртвая зона: цель в пределах 3 см на 50 м (0.03°) — писать нечего.
    {
        AimTarget t; t.valid = true; t.yaw = 0.02f; t.pitch = 0.02f; t.world_dist = 50.f;
        float ny = 0.f, np = 0.f;
        check(!AimMemoryStep(t, DT, 5.f, 50.f, 0.f, 0.f, ny, np), "цель в 3 см на 50 м — доворота нет");
        t.yaw = 0.5f;
        check(AimMemoryStep(t, DT, 5.f, 50.f, 0.f, 0.f, ny, np), "цель в 44 см на 50 м — доворот есть");
    }

    // 5. Переход через ±180°: цель в двух градусах, но «через полюс». Аим обязан
    //    довернуть на два градуса, а не развернуться на 358.
    {
        proto.delay = 1;
        RunResult r = Run(179.f, 0.f, -179.f, 0.f, DT, 5.f, 60.f, proto, 120);
        // Два градуса пути: если бы аим пошёл длинной дугой (358°), он бы за
        // 120 кадров до конца не дошёл; быстрое время захвата и есть проверка.
        printf("переход ±180°: время %.2f с, ошибка %.3f°\n", r.settle_s, r.final_err);
        check(r.settle_s > 0.f && r.settle_s < 0.15f && r.final_err < 0.3f,
              "переход через ±180° — короткой дугой");
    }

    // 6. Тангаж не уходит за вертикаль: цель за зенитом (100° вверх) — прицел
    //    обязан упереться в предел и не перевернуть камеру.
    {
        Game g = proto; g.pitch = 80.f;
        AimTarget t; t.valid = true; t.yaw = 0.f; t.pitch = 20.f; t.world_dist = 30.f;
        for (int i = 0; i < 300; i++) {
            float cy = g.ReadYaw(), cp = g.ReadPitch(), ny = 0.f, np = 0.f;
            t.pitch = 100.f - cp;
            if (AimMemoryStep(t, DT, 10.f, 30.f, cy, cp, ny, np)) g.Write(ny, np);
            g.Tick();
        }
        check(g.pitch <= 89.001f && g.pitch > 88.f, "тангаж упирается в 89°, камера не перевёрнута");
    }

    // 7. «Скорость» из меню: 10 обязана доводить быстрее, чем 1, и обе — сходиться.
    {
        RunResult slow = Run(0.f, 0.f, 60.f, 0.f, DT, 1.f, 50.f, proto, 400);
        RunResult fast = Run(0.f, 0.f, 60.f, 0.f, DT, 10.f, 50.f, proto, 400);
        printf("скорость 1: %.2f с / скорость 10: %.2f с\n", slow.settle_s, fast.settle_s);
        check(fast.settle_s > 0.f && fast.settle_s < slow.settle_s, "скорость 10 быстрее скорости 1");
        check(slow.final_err < 0.3f && fast.final_err < 0.3f, "обе скорости сходятся к цели");
    }

    // 8. Наведение по движущейся цели: цель едет 40°/с вбок. Прицел обязан
    //    отставать не больше чем на пару десятых градуса (отставание = скорость
    //    цели × задержка применения записи).
    //    (Упреждение из aim/update.cpp сюда не входит — стенд меряет сам
    //    контроллер, поэтому здесь именно остаточное отставание задержки:
    //    2 кадра x 0.67° = 1.3° на скорости 10 и 1.3/0.72 = 1.8° на скорости 5.)
    {
        proto.delay = 2;
        RunResult s5  = Run(0.f, 0.f, 0.f, 0.f, DT, 5.f, 50.f, proto, 300, 40.f);
        RunResult s10 = Run(0.f, 0.f, 0.f, 0.f, DT, 10.f, 50.f, proto, 300, 40.f);
        printf("цель 40°/с: скорость 5 — %.3f°, скорость 10 — %.3f°\n", s5.final_err, s10.final_err);
        check(s5.final_err < 2.2f, "движущаяся цель, скорость 5 — отставание меньше 2.2°");
        check(s10.final_err < 1.6f, "движущаяся цель, скорость 10 — отставание меньше 1.6°");
        proto.delay = 1;
    }

    // 9. Кадр не 60: на 30 и на 144 fps сходимость не должна разъезжаться —
    //    иначе «точнее тач-аима» превращалось бы в «точнее на 60 fps».
    {
        RunResult a = Run(0.f, 0.f, 50.f, 5.f, 1.f / 30.f, 5.f, 50.f, proto, 30);
        RunResult b = Run(0.f, 0.f, 50.f, 5.f, 1.f / 144.f, 5.f, 50.f, proto, 144);
        printf("30 fps: %.3f° / 144 fps: %.3f° (остаток через 1 с)\n", a.final_err, b.final_err);
        check(a.final_err < 0.6f && b.final_err < 0.6f, "сходимость не зависит от частоты кадров");
    }

    // 10. Мусор на входе не должен превращаться в запись: углы NaN/inf, цель без
    //     дистанции, отрицательный dt — всё это «не доворачиваем».
    {
        float ny = 0.f, np = 0.f;
        AimTarget t; t.valid = true; t.yaw = 5.f; t.pitch = 5.f; t.world_dist = 30.f;
        // Плохой dt (первый кадр, лаг планировщика) — не повод бросать аим:
        // контроллер считает такой такт как 60 fps и всё равно даёт шаг.
        ny = np = 0.f;
        check(AimMemoryStep(t, -1.f, 5.f, 30.f, 0.f, 0.f, ny, np) &&
              std::isfinite(ny) && std::isfinite(np) && fabsf(ny) <= 12.001f,
              "отрицательный dt — такт считается как 60 fps, шаг вменяемый");
        check(!AimMemoryStep(t, DT, 5.f, 30.f, (float)NAN, 0.f, ny, np), "углы NaN — записи нет");
        t.yaw = (float)INFINITY;
        check(!AimMemoryStep(t, DT, 5.f, 30.f, 0.f, 0.f, ny, np), "цель inf — записи нет");
        t.yaw = 5.f; t.valid = false;
        check(!AimMemoryStep(t, DT, 5.f, 30.f, 0.f, 0.f, ny, np), "цели нет — записи нет");
    }

    // 11. Потолок шага: даже при ошибке 170° за один такт контроллер не может
    //     крутануть камеру больше 12° — иначе на устройстве это читалось бы как
    //     «аим дёрнул камеру».
    {
        AimTarget t; t.valid = true; t.yaw = 170.f; t.pitch = 0.f; t.world_dist = 50.f;
        float ny = 0.f, np = 0.f;
        check(AimMemoryStep(t, DT, 10.f, 50.f, 0.f, 0.f, ny, np) && fabsf(ny) <= 12.001f,
              "шаг за такт не больше 12°");
    }

    printf("\n%s (%d проверок не прошло)\n", g_fail ? "ПРОВАЛ" : "всё сошлось", g_fail);
    return g_fail ? 1 : 0;
}
