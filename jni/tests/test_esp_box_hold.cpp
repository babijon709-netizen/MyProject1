// Host tests for the shared player-box and world-marker frame hold policy.
//
// Проверяется именно то, на что была жалоба 07.10:
//   * убитый/вышедший игрок исчезает в первый же ЗДОРОВЫЙ кадр;
//   * сбойный кадр не гасит живого игрока;
//   * сбойный кадр никогда не оставляет бокс навсегда.
// И то, на что была жалоба по «плавающим» визуалам: продление идёт только через
// перепроецирование МИРОВОГО якоря текущим кадром. Если кадра (матрицы вида) нет
// или перепроецирование не удалось, запись не рисуется вовсе — приклеенный к
// экрану бокс разъезжается с миром, как только камера поехала.
#include "esp_box_hold.h"

#include <cstdio>
#include <vector>

namespace {
int failures = 0;
#define CHECK(cond, name) do { \
    if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

constexpr double kHold = 0.30;

EspBox make_box(unsigned long long id, float distance) {
    EspBox box{};
    box.id = id;
    box.distance = distance;
    box.x1 = 1.0f; box.y1 = 2.0f; box.x2 = 3.0f; box.y2 = 4.0f;
    return box;
}

std::vector<EspBox> roster(std::initializer_list<unsigned long long> ids) {
    std::vector<EspBox> out;
    for (unsigned long long id : ids) out.push_back(make_box(id, (float)id));
    return out;
}

bool has(const std::vector<EspBox>& boxes, unsigned long long id) {
    for (const EspBox& box : boxes) if (box.id == id) return true;
    return false;
}

float distance_of(const std::vector<EspBox>& boxes, unsigned long long id, float fallback) {
    for (const EspBox& box : boxes) if (box.id == id) return box.distance;
    return fallback;
}

using Entry = espboxhold::Entry<EspBox>;

// Здоровый кадр игры (матрица вида есть): каждая удержанная запись
// перепроецируется своим мировым якорем — в тестах холда это «остаётся видна».
template <typename Value>
inline bool FrameOk(Value&) { return true; }

// Без кадра (матрицы вида нет) перепроецирование невозможно: запись уходит.
template <typename Value>
inline bool FrameMissing(Value&) { return false; }

// Тот же вызов, что и в оверлее, но с «кадр в порядке»: так проверяется политика
// удержания (состав, окно, отсутствие дублей) без привязки к матрицам.
template <typename Value>
void update(std::vector<espboxhold::Entry<Value>>& hold,
            const std::vector<Value>& fresh,
            bool transient, double now, double hold_seconds,
            std::vector<Value>& out) {
    espboxhold::update(hold, fresh, transient, now, hold_seconds, FrameOk<Value>, out);
}

EspMarker make_marker(unsigned long long id, float x) {
    EspMarker marker{};
    marker.id = id;
    marker.x = x;
    return marker;
}

const EspMarker* find_marker(const std::vector<EspMarker>& markers, unsigned long long id) {
    for (const EspMarker& marker : markers)
        if (marker.id == id) return &marker;
    return nullptr;
}
} // namespace

int main() {
    // 1) Здоровый кадр отдаёт ровно конвейерный состав.
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        update(hold, roster({0x1000, 0x2000, 0x3000}), false, 1.0, kHold, out);
        CHECK(out.size() == 3, "healthy frame keeps every player");
    }

    // 2) Смерть/выход с сервера: игрока нет в здоровом срезе — бокс уходит
    //    В ТОМ ЖЕ кадре, без удержания.
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        update(hold, roster({0x1000, 0x2000, 0x3000}), false, 1.0, kHold, out);
        update(hold, roster({0x1000, 0x3000}), false, 1.016, kHold, out);
        CHECK(out.size() == 2, "dead/left player gone on healthy frame");
        CHECK(!has(out, 0x2000), "dead id is not drawn");
    }

    // 3) Здоровый пустой реестр (один на сервере) — пусто сразу, а не «до
    //    следующего здорового кадра».
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        update(hold, roster({0x1000}), false, 1.0, kHold, out);
        update(hold, {}, false, 1.016, kHold, out);
        CHECK(out.empty(), "healthy empty registry clears instantly");
    }

    // 4) Рваный кадр с теми же игроками — состав не меняется.
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        update(hold, roster({0x1000, 0x2000}), false, 1.0, kHold, out);
        update(hold, roster({0x1000, 0x2000}), true, 1.016, kHold, out);
        CHECK(out.size() == 2, "transient frame keeps present players");
    }

    // 5) Рваный кадр, у одного игрока сорвалось чтение — его бокс держится.
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        update(hold, roster({0x1000, 0x2000}), false, 1.0, kHold, out);
        update(hold, roster({0x1000}), true, 1.016, kHold, out);
        CHECK(out.size() == 2, "single read failure does not blank a player");
        CHECK(has(out, 0x2000), "held player is still drawn");
    }

    // 6) Удержание ограничено по времени: чтение сломалось навсегда — боксы
    //    уходят, «залипнуть» не могут.
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        update(hold, roster({0x1000, 0x2000}), false, 1.0, kHold, out);
        update(hold, {}, true, 1.10, kHold, out);
        CHECK(out.size() == 2, "hold inside the window");
        update(hold, {}, true, 1.29, kHold, out);
        CHECK(out.size() == 2, "hold still inside the window");
        update(hold, {}, true, 1.31, kHold, out);
        CHECK(out.empty(), "hold expires after kBoxHoldSeconds");
        update(hold, {}, true, 5.0, kHold, out);
        CHECK(out.empty(), "no permanent ghosts");
    }

    // 7) Возврат игрока в рваном кадре обновляет бокс, а не дублирует запись.
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        update(hold, roster({0x1000}), false, 1.0, kHold, out);
        update(hold, {}, true, 1.05, kHold, out);
        CHECK(out.size() == 1, "held while missing");
        update(hold, {make_box(0x1000, 42.0f)}, true, 1.10, kHold, out);
        CHECK(out.size() == 1, "reappearance does not duplicate");
        CHECK(distance_of(out, 0x1000, -1.0f) == 42.0f, "held box is refreshed");
        // last_seen продлился возвратом: окно отсчитывается заново.
        update(hold, {}, true, 1.39, kHold, out);
        CHECK(out.size() == 1, "fresh last_seen extends the window");
        update(hold, {}, true, 1.41, kHold, out);
        CHECK(out.empty(), "and expires one window later");
    }

    // 8) Новый id в рваном кадре добавляется своим боксом и не наследует чужой
    //    адрес (объекты PlayerManager переиспользуются).
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        update(hold, roster({0x1000}), false, 1.0, kHold, out);
        update(hold, {make_box(0x4000, 77.0f)}, true, 1.05, kHold, out);
        CHECK(out.size() == 2, "new id joins the held set");
        CHECK(distance_of(out, 0x4000, -1.0f) == 77.0f, "new id keeps its own box");
        // roster() кладёт distance = (float)id, отсюда 0x1000 == 4096.
        CHECK(distance_of(out, 0x1000, -1.0f) == 4096.0f, "old id keeps its own box");
    }

    // 9) Смена мира: здоровый кадр с полностью новым составом заменяет всё.
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        update(hold, roster({0x1000, 0x2000}), false, 1.0, kHold, out);
        update(hold, roster({0x9000}), false, 1.016, kHold, out);
        CHECK(out.size() == 1, "new world replaces the roster");
        CHECK(has(out, 0x9000), "new world player drawn");
    }

    // 10) World markers use the same per-id update/expiry rules as player ESP.
    {
        std::vector<espboxhold::Entry<EspMarker>> hold;
        std::vector<EspMarker> out;
        update(hold, {make_marker(1, 10.0F), make_marker(2, 20.0F)}, false, 1.0, kHold, out);
        CHECK(out.size() == 2, "healthy marker frame keeps every id");

        update(hold, {make_marker(1, 15.0F)}, true, 1.1, kHold, out);
        CHECK(out.size() == 2, "transient marker frame holds the missing id");
        CHECK(find_marker(out, 1) && find_marker(out, 1)->x == 15.0F,
              "transient marker frame refreshes a readable id");
        CHECK(find_marker(out, 2) && find_marker(out, 2)->x == 20.0F,
              "held marker with a live camera frame is kept");

        update(hold, {}, true, 1.29, kHold, out);
        CHECK(out.size() == 2, "marker hold remains inside the time window");
        update(hold, {}, true, 1.31, kHold, out);
        CHECK(out.size() == 1 && find_marker(out, 1) && !find_marker(out, 2),
              "only markers whose own hold expired are removed");
        update(hold, {}, true, 1.41, kHold, out);
        CHECK(out.empty(), "marker hold expires without permanent ghosts");

        update(hold, {make_marker(3, 30.0F)}, false, 2.0, kHold, out);
        update(hold, {}, false, 2.016, kHold, out);
        CHECK(out.empty(), "healthy empty marker frame clears immediately");
    }

    // 11) Кадра нет (не прочиталась матрица вида): удержанная запись не рисуется.
    //     Иначе она осталась бы висеть в прошлых ЭКРАННЫХ координатах, пока
    //     камера едет, — это и есть «визуалы плавают по экрану».
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        espboxhold::update(hold, roster({0x1000, 0x2000}), false, 1.0, kHold,
                           FrameOk<EspBox>, out);
        CHECK(out.size() == 2, "healthy frame with camera");
        espboxhold::update(hold, {}, true, 1.05, kHold, FrameMissing<EspBox>, out);
        CHECK(out.empty(), "no camera frame: held boxes are not drawn");
        // Кадр вернулся — состав берётся из конвейера заново, без пустых
        // экранных координат из прошлого.
        espboxhold::update(hold, roster({0x1000, 0x2000}), false, 1.10, kHold,
                           FrameOk<EspBox>, out);
        CHECK(out.size() == 2, "healthy frame restores the roster after a dropped tick");
    }

    // 12) Перепроецирование зовётся для удержанных записей и только для них:
    //     этим кадр и пересчитывает экран из мирового якоря.
    {
        std::vector<Entry> hold; std::vector<EspBox> out;
        espboxhold::update(hold, roster({0x1000}), false, 1.0, kHold, FrameOk<EspBox>, out);
        int calls = 0;
        unsigned long long last_id = 0;
        // Игрок пропал из сбойного среза: запись удержана и перепроецируется.
        espboxhold::update(hold, {}, true, 1.05, kHold,
                           [&](EspBox& box) { ++calls; last_id = box.id; return true; }, out);
        CHECK(calls == 1, "reproject called for the held entry");
        CHECK(last_id == 0x1000, "reproject sees the held entry");
        calls = 0;
        espboxhold::update(hold, roster({0x1000}), true, 1.10, kHold,
                           [&](EspBox&) { ++calls; return true; }, out);
        CHECK(calls == 0, "freshly updated entries are not reprojected again");
    }

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("ALL ESP-FRAME-HOLD TESTS PASS\n");
    return 0;
}
