#pragma once
// vis_check_inproc.h — ВНУТРИПРОЦЕССНЫЙ вариант проверки видимости: не свой BVH
// по мешу, а Physics.Linecast САМОЙ игры, вызванный изнутри её процесса.
//
// В текущую сборку (xvcen) НЕ встроен и намеренно ниоткуда не включается:
// висибл-чек здесь работает внешним чтением по мешу карты (vis_check_map.h).
// Этот путь требует другого исполнения — кода, живущего внутри игры:
//   * собрать как .so (LOCAL_MODULE как BUILD_SHARED_LIBRARY) и загрузить в
//     процесс игры инъекцией (или через свой лоадер на старте игры);
//   * RVAs ниже — из своего дампа (dump.cs + libil2cpp.so). Они меняются на
//     КАЖДОМ обновлении игры: переметить Il2CppDumper'ом и перенести пять
//     констант, ничего больше;
//   * звать только из главного потока Unity (хук в Update/LateUpdate):
//     Physics.* с другого потока роняет процесс;
//   * в отличие от меша карты — учитывает динамику (постройки игроков, двери,
//     технику) и не требует экспорта геометрии вовсе.
// Плата за это: инъекция в защищённый процесс (см. шапку stealth.h о способах
// чтения — там же сказано, чем это отличается по риску от внешнего чтения).
//
// Источник: oxide_unity_vis.hpp из архива vischeck, оставлен как есть —
// подключать осмысленно только вместе с инъектором, иначе это мёртвый код.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <link.h>   // dl_iterate_phdr

namespace oxide {

// ---- offsets (RVA into libil2cpp.so) ----
namespace off {
    constexpr uintptr_t Physics_Linecast        = 0xCCDEC0C; // static bool Linecast(Vector3,Vector3,int,QueryTriggerInteraction)
    constexpr uintptr_t Camera_get_main         = 0xCBA4214; // static Camera get_main()
    constexpr uintptr_t Component_get_transform = 0xCC1FA20; // Transform get_transform()
    constexpr uintptr_t Transform_get_position  = 0xCC39128; // Vector3 get_position()
    constexpr uintptr_t Camera_WorldToScreen    = 0xCBA3768; // Vector3 WorldToScreenPoint(Vector3)
}

// FILL: layers that block sight (terrain, buildings, rocks, trees) - NOT players/triggers.
// Find names via dump.cs / TagManager, or log LayerMask.NameToLayer for candidates. -5 = Unity default raycast mask.
constexpr int kBlockMask = -5;
constexpr int kIgnoreTriggers = 1; // QueryTriggerInteraction.Ignore

struct Vector3 { float x, y, z; };

namespace detail {
    inline uintptr_t g_base = 0;
    inline int Cb(dl_phdr_info* info, size_t, void*) {
        if (info->dlpi_name && std::strstr(info->dlpi_name, "libil2cpp.so")) { g_base = info->dlpi_addr; return 1; }
        return 0;
    }
    template <class F> inline F Fn(uintptr_t rva) { return reinterpret_cast<F>(g_base + rva); }
}

inline bool Init() {
    if (!detail::g_base) dl_iterate_phdr(detail::Cb, nullptr);
    return detail::g_base != 0;
}

// il2cpp icall signatures: trailing void* = MethodInfo* (pass nullptr). Vector3 (3 floats) is an HFA on AAPCS64,
// so by-value passing matches the generated code.
using Linecast_t      = bool    (*)(Vector3, Vector3, int, int, void*);
using GetMain_t       = void*   (*)(void*);
using GetTransform_t  = void*   (*)(void*, void*);
using GetPosition_t   = Vector3 (*)(void*, void*);

inline Vector3 PositionOf(void* component) {
    auto tr  = detail::Fn<GetTransform_t>(off::Component_get_transform)(component, nullptr);
    return tr ? detail::Fn<GetPosition_t>(off::Transform_get_position)(tr, nullptr) : Vector3{0, 0, 0};
}

inline bool CameraEye(Vector3& eye) {
    void* cam = detail::Fn<GetMain_t>(off::Camera_get_main)(nullptr);
    if (!cam) return false;               // null mid-scene-load
    eye = PositionOf(cam);
    return true;
}

// true = nothing in kBlockMask between eye and target. Pad the end so the target's own ground/collider doesn't block.
inline bool LineClear(const Vector3& eye, Vector3 target) {
    const float dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 0.01f) return true;
    const float k = (len - 0.15f) / len;  // stop 15cm short
    const Vector3 end{eye.x + dx * k, eye.y + dy * k, eye.z + dz * k};
    return !detail::Fn<Linecast_t>(off::Physics_Linecast)(eye, end, kBlockMask, kIgnoreTriggers, nullptr);
}

// pts = head / chest / pelvis / feet world positions (Transform.get_position of bones, or root + offsets)
inline bool IsVisible(const Vector3& eye, const Vector3* pts, int n) {
    for (int i = 0; i < n; ++i) if (LineClear(eye, pts[i])) return true;
    return false;
}

} // namespace oxide
