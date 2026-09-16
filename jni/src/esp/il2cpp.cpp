// il2cpp.cpp — Il2Cpp: базовые адреса, классы, статические поля.
//
// Модуль разрезан из прежнего монолита jni/src/game.cpp;
// что здесь лежит и кто это зовёт — в шапке il2cpp.h и в docs/CODE_MAP.md.

#include "esp/common.h"
#include "esp/aim_points.h"
#include "esp/boxes.h"
#include "esp/camera.h"
#include "esp/farm_scan.h"
#include "esp/frame.h"
#include "esp/managed.h"
#include "esp/markers.h"
#include "esp/melee.h"
#include "esp/mem.h"
#include "esp/names.h"
#include "esp/transform.h"
#include "esp/weapons.h"
#include "il2cpp.h"

// Карта памяти процесса читается ЦЕЛИКОМ одним дескриптором (раньше был fgets по
// строкам): это и быстрее, и надёжнее — строку разбираем сами, а имя сверяем с
// последним сегментом пути, поэтому подстрока в чужом имени (или « (deleted)» в
// конце) больше не путает.
// Все базы-кандидаты той же библиотеки (см. maps::lookup_library_bases): после
// перезапуска игры образов в карте бывает несколько.
int get_base_candidates(const char* lib, uint64_t* out, int max) {
    if (g_pid <= 0 || !out || max <= 0) return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", g_pid);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    std::string text;
    char chunk[8192];
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            text.append(chunk, (size_t)n);
            if (text.size() > (8u << 20)) break;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    return maps::lookup_library_bases(text, lib, out, max);
}

// Похож ли адрес на Il2CppClass — по одной структуре, без чтения имён.
bool class_looks_alive(uint64_t klass) {
    if (!valid_obj(klass)) return false;
    const uint64_t image = rd_ptr(klass);
    const uint64_t name  = rd_ptr(klass + IL2CPP_CLASS_NAME);
    const uint64_t space = rd_ptr(klass + IL2CPP_CLASS_NAMESPACE);
    if (!image || !name || !space) return false;
    // Имя и пространство имён — указатели в память метаданных: не ноль и не мусор.
    return name >= 0x10000 && space >= 0x10000;
}

// Сверка класса с ожидаемым именем и пространством имён.
//   1 — имя прочитано и совпало;
//   2 — имя прочитать не удалось, класс принят по структуре;
//   0 — не он.
//
// Почему не только имя. С устройства пришёл лог, где ВСЕ чтения имён классов
// падали с errno 5: адреса 0x2002a500 и 0x20026580 — это строки имён классов в
// памяти метаданных, и на том устройстве эта память не отображена вовсе (ядро
// отдаёт EIO, как по отданной странице), хотя всё остальное — классы, статические
// поля, куча — читается прекрасно. Из-за одних только имён чит не мог опознать
// ни одного класса, и «функционал не работает» целиком: отказов 35 тысяч, боксов
// ноль.
// Поэтому там, где имя доступно, сверяем его, как раньше (защита от чужой сборки
// игры); где нет — верим смещению из таблицы оффсетов и проверяем структуру.
int class_identity(uint64_t klass, const char* expected_name, const char* expected_ns) {
    if (!class_looks_alive(klass)) return 0;
    bool name_readable = false, ns_readable = false;
    const std::string name  = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME), &name_readable);
    const std::string space = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAMESPACE), &ns_readable);
    if (!name_readable && !ns_readable) return 2;
    if (name_readable && name != expected_name) return 0;
    if (ns_readable && space != expected_ns) return 0;
    return 1;
}

// Живой ли это il2cpp выбранной сборки по адресу base: читаем Il2CppClass*
// PlayerManager из его typeinfo и сверяем имя. Проверка дешёвая (несколько сотен
// байт) и однозначная: в чужом/мёртвом образе (старый «(deleted)» после
// перезапуска игры) там мусор, и класс игры не резолвится — привязка при этом
// выглядит успешной, а весь чит молча не работает.
bool base_resolves_game(uint64_t base) {
    if (!base) return false;
    if (PLAYER_MANAGER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(base + PLAYER_MANAGER_TYPEINFO_RVA);
        if (class_identity(candidate, "PlayerManager", "Oxide") != 0) return true;
    }
    if (GAME_CONTROLLER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(base + GAME_CONTROLLER_TYPEINFO_RVA);
        if (class_identity(candidate, "GameControllerBase", "Oxide") != 0) return true;
    }
    return false;
}

static bool validate_player_list(uint64_t list, uint64_t player_class) {
    if (!list || !player_class) return false;
    uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
    int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
    if (!items || count < 0 || count > 512) return false;
    if (count == 0) {
        // Пустой список — обычное состояние, пока игроки не заспавнились, и
        // сверить класс по элементам нечем. Раньше здесь сверялось имя класса
        // ("List`1"): там, где память имён недоступна (см. class_identity),
        // список отвергался — и чит не работал вообще. Теперь: массив элементов
        // на месте (List хранит его даже пустым), а класс — живой il2cpp-класс.
        if (!valid_obj(rd_ptr(list + IL2CPP_LIST_ITEMS))) return false;
        return class_looks_alive(rd_ptr(list));
    }
    int32_t checked = 0;
    for (int32_t index = 0; index < count && checked < 4; ++index) {
        uint64_t player = rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t));
        if (!player) continue;
        if (rd_ptr(player) != player_class) return false;
        ++checked;
    }
    return checked > 0;
}

bool player_list_contains(uint64_t list, uint64_t player) {
    if (!list || !player) return false;
    uint64_t items = rd_ptr(list + IL2CPP_LIST_ITEMS);
    int32_t count = rd<int32_t>(list + IL2CPP_LIST_SIZE);
    if (!items || count <= 0 || count > 512) return false;
    for (int32_t index = 0; index < count; ++index) {
        if (rd_ptr(items + IL2CPP_ARRAY_FIRST_ELEMENT + (uint64_t)index * sizeof(uint64_t)) == player)
            return true;
    }
    return false;
}

static constexpr uint64_t IL2CPP_CLASS_STATIC_FIELDS = 0xB8;

uint64_t get_class_static_fields(uint64_t klass) {
    if (!klass) return 0;
    return rd_ptr(klass + IL2CPP_CLASS_STATIC_FIELDS);
}

uint64_t resolve_runtime_player_list() {
    if (!g_player_manager_class && PLAYER_MANAGER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(g_il2cpp_base + PLAYER_MANAGER_TYPEINFO_RVA);
        if (class_identity(candidate, "PlayerManager", "Oxide") != 0)
            g_player_manager_class = candidate;
    }
    if (!g_player_manager_class) {
        return 0;
    }
    if (!g_player_manager_static_fields)
        g_player_manager_static_fields = get_class_static_fields(g_player_manager_class);
    if (!g_player_manager_static_fields) {
        return 0;
    }
    uint64_t list = rd_ptr(g_player_manager_static_fields + PLAYER_MANAGER_STATIC_FIELDS_LIST);
    if (!validate_player_list(list, g_player_manager_class)) {
        g_player_manager_static_fields = 0;
        return 0;
    }
    return list;
}

uint64_t resolve_local_player() {
    if (g_local_player && rd_ptr(g_local_player) == g_player_manager_class)
        return g_local_player;
    g_local_player = 0;

    if (!g_game_controller_class && GAME_CONTROLLER_TYPEINFO_RVA != 0) {
        const uint64_t candidate = rd_ptr(g_il2cpp_base + GAME_CONTROLLER_TYPEINFO_RVA);
        if (class_identity(candidate, "GameControllerBase", "Oxide") != 0)
            g_game_controller_class = candidate;
    }

    if (!g_game_controller_class || !g_player_manager_class) return 0;

    uint64_t gcb_static_fields = get_class_static_fields(g_game_controller_class);
    if (!gcb_static_fields) return 0;
    uint64_t local_player = rd_ptr(gcb_static_fields + GAME_CONTROLLER_LOCAL_PLAYER_FIELD);
    if (local_player && rd_ptr(local_player) == g_player_manager_class) {
        g_local_player = local_player;
        return local_player;
    }
    return 0;
}

uint64_t resolve_native_transform(uint64_t transform) {
    if (!transform) return 0;
    return rd_ptr(transform + MANAGED_CACHED_PTR);
}

// Il2CppClass name check (klass @0x0, name @0x10) — identifies PlayerWeapon
// without relying on the obfuscated wrapper layout.
// Совпадает ли имя класса объекта с ожидаемым.
// accept_when_unreadable — что делать, если имя прочитать нельзя (память имён на
// части устройств недоступна, см. class_identity). Где рядом есть второй,
// независимый признак (обратная ссылка на игрока) — объект принимаем; где признак
// только имя — нет, иначе под проверку попадёт что угодно.
bool object_class_name_is(uint64_t obj, const char* expected,
                                 bool accept_when_unreadable) {
    if (!valid_obj(obj)) return false;
    uint64_t klass = rd_ptr(obj);
    if (!valid_obj(klass)) return false;
    bool readable = false;
    const std::string name = read_remote_string(rd_ptr(klass + IL2CPP_CLASS_NAME), &readable);
    if (!readable) return accept_when_unreadable;
    return name == expected;
}
