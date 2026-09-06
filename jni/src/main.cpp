#include "main.h"
#include "game.h"
#include "game_offsets.h"   // PLAYER_BOX_WIDTH_RATIO (box proportions)
#include <cmath>
#include <atomic>
#include <chrono>
#include <string>
#include <GLES3/gl3.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <algorithm>
#include <random>
#include <sys/inotify.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <time.h>

// media/anime.h (танцующий спрайт) больше не подключается — аватар-гифка
// убрана из интерфейса, и её данные не должны раздувать бинарник.

#if __has_include("media/icons.h")
#  include "media/icons.h"
#  define ICONS_AVAILABLE
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image/stb_image.h"
#include "Blur/Blur.h"

#if __has_include("media/audio.h")
#  include "media/audio.h"
#  define AUDIO_AVAILABLE
#endif

#ifdef AUDIO_AVAILABLE
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

enum SoundId { SND_CLICK = 0, SND_SUCCESS, SND_COUNT };

struct AudioState {
    SLObjectItf engineObj  = nullptr;
    SLEngineItf engineItf  = nullptr;
    SLObjectItf outputMix  = nullptr;
    struct Player {
        SLObjectItf                   obj  = nullptr;
        SLPlayItf                     play = nullptr;
        SLAndroidSimpleBufferQueueItf bq   = nullptr;
    } players[SND_COUNT];
};
static AudioState g_audio;

static void AudioInit() {
    SLEngineOption opt[] = {{ SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE }};
    if (slCreateEngine(&g_audio.engineObj, 1, opt, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engineObj)->Realize(g_audio.engineObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engineObj)->GetInterface(g_audio.engineObj, SL_IID_ENGINE, &g_audio.engineItf) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engineItf)->CreateOutputMix(g_audio.engineItf, &g_audio.outputMix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.outputMix)->Realize(g_audio.outputMix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;

    const struct { const uint8_t* data; size_t len; } srcs[SND_COUNT] = {
        { click_wav,   click_wav_len   },
        { success_wav, success_wav_len },
    };

    for (int i = 0; i < SND_COUNT; i++) {
        const uint8_t* wav = srcs[i].data;
        uint16_t channels = 0; uint32_t sampleRate = 0; uint16_t bps = 0;
        memcpy(&channels,   wav + 22, 2);
        memcpy(&sampleRate, wav + 24, 4);
        memcpy(&bps,        wav + 34, 2);

        SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1 };
        SLDataFormat_PCM fmt = {
            SL_DATAFORMAT_PCM, (SLuint32)channels, (SLuint32)sampleRate * 1000,
            (SLuint32)bps, (SLuint32)bps,
            channels == 2 ? SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT : SL_SPEAKER_FRONT_CENTER,
            SL_BYTEORDER_LITTLEENDIAN
        };
        SLDataSource src  = { &loc, &fmt };
        SLDataLocator_OutputMix outLoc = { SL_DATALOCATOR_OUTPUTMIX, g_audio.outputMix };
        SLDataSink   sink = { &outLoc, nullptr };

        const SLInterfaceID ids[] = { SL_IID_BUFFERQUEUE };
        const SLboolean     req[] = { SL_BOOLEAN_TRUE };
        if ((*g_audio.engineItf)->CreateAudioPlayer(g_audio.engineItf, &g_audio.players[i].obj, &src, &sink, 1, ids, req) != SL_RESULT_SUCCESS) continue;
        if ((*g_audio.players[i].obj)->Realize(g_audio.players[i].obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) continue;
        (*g_audio.players[i].obj)->GetInterface(g_audio.players[i].obj, SL_IID_PLAY,        &g_audio.players[i].play);
        (*g_audio.players[i].obj)->GetInterface(g_audio.players[i].obj, SL_IID_BUFFERQUEUE, &g_audio.players[i].bq);
        (*g_audio.players[i].play)->SetPlayState(g_audio.players[i].play, SL_PLAYSTATE_PLAYING);
    }
}

static void PlaySound(SoundId id) {
    static const struct { const uint8_t* data; size_t len; } srcs[SND_COUNT] = {
        { click_wav,   click_wav_len   },
        { success_wav, success_wav_len },
    };
    auto& p = g_audio.players[id];
    if (!p.bq || !p.play) return;
    (*p.play)->SetPlayState(p.play, SL_PLAYSTATE_STOPPED);
    (*p.bq)->Clear(p.bq);
    (*p.bq)->Enqueue(p.bq, (void*)(srcs[id].data + 44), (SLuint32)(srcs[id].len - 44));
    (*p.play)->SetPlayState(p.play, SL_PLAYSTATE_PLAYING);
}

static void AudioFree() {
    for (int i = 0; i < SND_COUNT; i++) {
        if (g_audio.players[i].obj) { (*g_audio.players[i].obj)->Destroy(g_audio.players[i].obj); g_audio.players[i].obj = nullptr; }
    }
    if (g_audio.outputMix) { (*g_audio.outputMix)->Destroy(g_audio.outputMix); g_audio.outputMix = nullptr; }
    if (g_audio.engineObj) { (*g_audio.engineObj)->Destroy(g_audio.engineObj); g_audio.engineObj = nullptr; }
}
#else
enum SoundId { SND_CLICK = 0, SND_SUCCESS, SND_COUNT };
static void AudioInit() {}
static void PlaySound(SoundId) {}
static void AudioFree() {}
#endif

namespace xp {

static constexpr uint64_t _ks0 = 0xC3A1F72B9D4E058BULL;
static constexpr uint64_t _ks1 = 0x2B7E3D48F1C96A30ULL;

static constexpr uint8_t _xk(size_t i) noexcept {
    uint64_t s = _ks0 ^ (_ks1 * (i + 1));
    s ^= s >> 33; s *= 0xff51afd7ed558ccdULL;
    s ^= s >> 33; s *= 0xc4ceb9fe1a85ec53ULL;
    s ^= s >> 33;
    return static_cast<uint8_t>(s & 0xFF);
}

template<size_t N>
struct _XS {
    char b[N]{};
    mutable char o[N]{};
    constexpr _XS(const char (&s)[N]) noexcept {
        for (size_t i = 0; i < N; ++i) b[i] = static_cast<char>(static_cast<uint8_t>(s[i]) ^ _xk(i));
    }
    __attribute__((noinline)) const char* d() const noexcept {
        for (size_t i = 0; i < N; ++i) o[i] = static_cast<char>(static_cast<uint8_t>(b[i]) ^ _xk(i));
        return o;
    }
};
template<size_t N> constexpr auto _mk(const char (&s)[N]) noexcept { return _XS<N>(s); }

}

#define XS(s) ([]() noexcept -> const char* { static constexpr auto _x = xp::_mk(s); return _x.d(); }())

namespace prot {
static void Init() {}
}

static float g_sw = 1920.f;
static float g_sh = 1080.f;

static void VisibleScreen(float& w, float& h);

// Автофарм: статус для строки во вкладке «Разное» (определены рядом с
// UpdateFarm ниже).
extern bool g_farmActive;
extern int  g_farmPhase;
extern int  g_farmNodes;      // сколько узлов нашёл последний скан реестра
extern int  g_farmReason;     // причина простоя (см. esp_farm_debug)
extern float g_farmTgtDist;   // дистанция до текущей цели, м
extern int  g_farmTgtKind;    // 0 дерево, 1 камень, 2 металл, 3 сера

namespace ui { namespace bar {
    inline float g_game_alpha = 1.f;
    inline void  set_game_alpha(float a){ g_game_alpha=a; }
    inline float game_alpha(){ return g_game_alpha; }
}}

namespace cfg { namespace esp {
    inline ImVec4 box_col         = {0.20f, 0.85f, 0.35f, 1.f};
    inline ImVec4 box_col_invis   = {1.00f, 0.20f, 0.20f, 1.f};
    inline ImVec4 name_col        = {1.00f, 1.00f, 1.00f, 1.f};
    inline ImVec4 distance_col    = {0.70f, 0.70f, 0.70f, 1.f};
    inline ImVec4 weapon_col      = {1.00f, 0.95f, 0.10f, 1.f};
    inline ImVec4 tracer_col      = {1.00f, 0.20f, 0.20f, 1.f};
    inline ImVec4 skeleton_col    = {0.20f, 0.85f, 0.35f, 1.f};
    inline ImVec4 animal_col      = {1.00f, 0.60f, 0.25f, 1.f};
    inline ImVec4 loot_col        = {0.55f, 0.80f, 1.00f, 1.f};
    inline ImVec4 ally_col        = {0.25f, 0.55f, 1.00f, 1.f};
    inline ImVec4 pickup_col      = {0.60f, 1.00f, 0.60f, 1.f};
    // Tracers drawn to a team mate are always green, no matter what colour the
    // enemy tracers use — that is the whole point of telling them apart.
    inline ImVec4 ally_tracer_col = {0.20f, 0.90f, 0.35f, 1.f};

    inline bool box          = false;
    inline bool name_esp     = false;
    inline bool distance     = false;
    inline bool weapon       = false;
    inline bool tracer       = false;
    inline bool skeleton     = false;
    inline bool ore          = false;
    inline bool animal       = false;
    inline bool loot         = false;
    inline bool team         = false;
    inline bool pickup       = false;
    inline bool  vis_check        = false;
    inline bool  fill             = false;
    inline float stroke           = 2.f;
    inline float rounding         = 0.f;
    inline float fill_pct         = 50.f;
    inline int   box_type         = 0;
    inline float box_rounding     = 0.f;
    inline bool  hp_outline       = true;
    inline bool  hp_gradient      = false;
    inline float tracer_thickness = 1.5f;
    inline ImVec4 hp_min_col      = {1.00f, 0.20f, 0.10f, 1.f};
    inline ImVec4 hp_max_col      = {0.20f, 0.85f, 0.35f, 1.f};
}}

namespace cfg { namespace aim {
    inline bool  enabled           = false;
    inline bool  vis_check         = false;
    inline bool  draw_fov          = false;
    inline bool  scope_only        = false;   // aim only while ADS (прицел)
    inline float fov               = 80.f;
    inline float smoothness        = 5.f;
    inline int   bone              = 0;
    inline bool  trigger_bot       = false;
    inline bool  knife_bot         = false;
    inline float trigger_delay     = 0.0f;
    inline float fov_color[4]      = {1.0f, 1.0f, 1.0f, 1.0f};
}}

static std::mt19937& GetRNG() {
    static std::mt19937 rng(std::random_device{}());
    return rng;
}

std::atomic<bool> main_thread_flag{true};
std::atomic<bool> g_frame_done{true};

#define TARGET_PACKAGE "com.catsbit.oxidesurvivalisland"

static pid_t g_target_pid = -1;
static bool  g_esp_attached = false;
static std::thread g_attach_thread;
static std::atomic<bool> g_attach_running{false};

static pid_t find_pid(const char* pkg) {
    DIR* directory = opendir("/proc");
    if (!directory) return -1;
    struct dirent* entry;
    char path[256], command[256];
    pid_t child_fallback = -1;
    while ((entry = readdir(directory))) {
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0) continue;
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        FILE* file = fopen(path, "r");
        if (!file) continue;
        memset(command, 0, sizeof(command));
        size_t count = fread(command, 1, sizeof(command) - 1, file);
        fclose(file);
        if (count == 0) continue;
        if (strcmp(command, pkg) == 0) { closedir(directory); return pid; }
        size_t pkg_len = strlen(pkg);
        if (child_fallback <= 0 && strncmp(command, pkg, pkg_len) == 0 && command[pkg_len] == ':')
            child_fallback = pid;
    }
    closedir(directory);
    return child_fallback;
}

static void start_attach_thread() {
    g_attach_running.store(true);
    g_attach_thread = std::thread([]() {
        while (g_attach_running.load()) {
            if (!g_esp_attached) {
                pid_t pid = find_pid(TARGET_PACKAGE);
                if (pid > 0 && esp_init(pid)) {
                    g_target_pid = pid;
                    g_esp_attached = true;
                }
            } else {
                pid_t current = find_pid(TARGET_PACKAGE);
                if (current != g_target_pid) {
                    esp_reset();
                    g_esp_attached = false;
                    g_target_pid = -1;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    });
}

static void stop_attach_thread() {
    g_attach_running.store(false);
    if (g_attach_thread.joinable()) g_attach_thread.join();
}

struct ToastState {
    char  msg[128]     = {};
    char  nextMsg[128] = {};
    bool  hasNext      = false;
    float slidePos = 0.f, slideVel = 0.f;
    float life = 0.f;
    float textA = 0.f, textAVel = 0.f;
    float barW = 0.f, barVel = 0.f;
    float widthAnim = 200.f, widthVel = 0.f;
    bool  visible = false, closing = false;
};
static ToastState g_toast;

static void ShowToast(const char* msg) {
    if (!g_toast.visible || g_toast.closing) {
        snprintf(g_toast.msg, sizeof(g_toast.msg), "%s", msg);
        g_toast.life = 0.f; g_toast.closing = false; g_toast.visible = true;
        g_toast.slidePos = 0.f; g_toast.slideVel = 0.f;
        g_toast.textA = 0.f; g_toast.textAVel = 0.f;
        g_toast.barW = 1.f; g_toast.barVel = 0.f;
        g_toast.widthVel = 0.f;
        g_toast.hasNext = false;
    } else {
        snprintf(g_toast.nextMsg, sizeof(g_toast.nextMsg), "%s", msg);
        g_toast.hasNext = true; g_toast.life = 0.f;
    }
}

static constexpr int kTabCount = 6;
static GLuint g_tabIcons[kTabCount] = {};

static GLuint LoadTexFromMemory(const unsigned char* data, int len) {
    int w, h, ch;
    unsigned char* px = stbi_load_from_memory(data, len, &w, &h, &ch, 4);
    if (!px) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return tex;
}

void LoadTabIcons() {
#ifdef ICONS_AVAILABLE
    g_tabIcons[0] = LoadTexFromMemory(main_tab_png, (int)main_tab_png_len);
    g_tabIcons[1] = LoadTexFromMemory(aimbot_png,   (int)aimbot_png_len);
    g_tabIcons[2] = LoadTexFromMemory(visuals_png,  (int)visuals_png_len);
    // Tab order: 3=Разное, 4=Конфиги, 5=Опции. У «Разное» своя векторная
    // иконка (рисуется кодом), поэтому текстура ему не нужна — иначе она
    // дублировала бы иконку «Конфиги» (misc_png).
    g_tabIcons[3] = 0;
    g_tabIcons[4] = LoadTexFromMemory(misc_png,     (int)misc_png_len);
    g_tabIcons[5] = LoadTexFromMemory(settings_png, (int)settings_png_len);
#endif
}

// Танцующий спрайт-аватар убран из интерфейса; текстура больше не грузится.
void LoadAnimeImage() {}

static inline float Lerpf(float a, float b, float t)   { return a + (b - a) * t; }
static inline float Clamp01(float t)                    { return t < 0.f ? 0.f : t > 1.f ? 1.f : t; }
static inline float EaseOut3(float t)                   { float i = 1 - t; return 1 - i * i * i; }
static inline float EaseInOut(float t)                  { return t * t * (3 - 2 * t); }



static inline bool WasTappedHere() {
    const auto& io = ImGui::GetIO();
    if (!io.MouseReleased[0]) return false;
    auto min = ImGui::GetItemRectMin();
    auto max = ImGui::GetItemRectMax();
    // Строки контента существуют и за пределами видимой области (скролл),
    // поэтому нажатие засчитывается только по видимой (не обрезанной клипом)
    // части строки. Без этого тап по нижней панели вкладок «проваливается»
    // в невидимую строку под ней и включает её функцию.
    {
        ImVec2 clMin = ImGui::GetWindowDrawList()->GetClipRectMin();
        ImVec2 clMax = ImGui::GetWindowDrawList()->GetClipRectMax();
        min.x = ImMax(min.x, clMin.x); min.y = ImMax(min.y, clMin.y);
        max.x = ImMin(max.x, clMax.x); max.y = ImMin(max.y, clMax.y);
        if (min.x >= max.x || min.y >= max.y) return false;
    }
    auto cp  = io.MouseClickedPos[0];
    auto mp  = io.MousePos;
    bool started = cp.x >= min.x && cp.x <= max.x && cp.y >= min.y && cp.y <= max.y;
    bool ended   = mp.x >= min.x - 12.f && mp.x <= max.x + 12.f && mp.y >= min.y - 12.f && mp.y <= max.y + 12.f;
    float dx = mp.x - cp.x, dy = mp.y - cp.y;
    return started && ended && (dx * dx + dy * dy) <= 30.f * 30.f;
}

// Точка внутри текущего клип-прямоугольника окна? Ручные обработчики кликов
// (карточки конфигов, цветные точки и т.п.) обязаны проверять это, иначе тап
// по нижней панели вкладок «проваливается» в прокрученную за неё строку.
static inline bool PtInClip(ImVec2 p) {
    auto* dl = ImGui::GetWindowDrawList();
    ImVec2 mn = dl->GetClipRectMin(), mx = dl->GetClipRectMax();
    return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y;
}

static void SpringTick(float& pos, float& vel, float target, float dt) {
    static constexpr float kStiffness = 180.f;
    static constexpr float kDamping   = 26.f;
    static constexpr float kPosTol    = 0.3f;
    static constexpr float kVelTol    = 1.f;
    float force = kStiffness * (target - pos) - kDamping * vel;
    vel += force * dt;
    pos += vel * dt;
    if (fabsf(target - pos) < kPosTol && fabsf(vel) < kVelTol) { pos = target; vel = 0.f; }
}

static void Tick(float& a, bool v, float dt, float spd = 10.f) {
    if (dt > 0.032f) dt = 0.032f;
    a += (float(v) - a) * spd * dt;
    if (!v && a < .002f) a = 0.f;
    if (v && a > .998f) a = 1.f;
}

namespace R {
    static constexpr float Card  = 24.f;
    static constexpr float Pill  = 16.f;
    static constexpr float Sheet = 26.f;
    static constexpr float Btn   = 18.f;
}

// Тёмная тема по умолчанию — лучше сочетается с фиолетовым акцентом
// и не слепит поверх игры; светлая включается в Настройках как раньше.
static bool  g_darkTheme = true;
static float g_themeT    = 1.f;
static float g_menuFadeIn = 0.f;

namespace C {
    // "Graphite & Violet": спокойный графитовый фон + фиолетовый акцент.
    // Light — мягкий светло-серый с лёгким лавандовым оттенком,
    // Dark  — глубокий сине-графитовый (почти чёрный), карточки чуть светлее.
    namespace Light {
        static constexpr ImVec4 Bg     = {0.929f, 0.933f, 0.953f, 1};
        static constexpr ImVec4 LeftBg = {0.957f, 0.957f, 0.973f, 1};
        static constexpr ImVec4 Card   = {1.000f, 1.000f, 1.000f, 1};
        static constexpr ImVec4 Acc    = {0.424f, 0.361f, 0.906f, 1};
        static constexpr ImVec4 AccDk  = {0.333f, 0.275f, 0.784f, 1};
        static constexpr ImVec4 Red    = {1.000f, 0.271f, 0.227f, 1};
        static constexpr ImVec4 Txt    = {0.078f, 0.082f, 0.102f, 1};
        static constexpr ImVec4 Dim    = {0.541f, 0.553f, 0.596f, 1};
        static constexpr ImVec4 TrkOff = {0.788f, 0.796f, 0.839f, 1};
        static constexpr ImVec4 Sep    = {0.851f, 0.859f, 0.898f, 1};
    }
    namespace Dark {
        static constexpr ImVec4 Bg     = {0.063f, 0.071f, 0.094f, 1};
        static constexpr ImVec4 LeftBg = {0.047f, 0.055f, 0.075f, 1};
        static constexpr ImVec4 Card   = {0.106f, 0.118f, 0.153f, 1};
        static constexpr ImVec4 Acc    = {0.545f, 0.486f, 1.000f, 1};
        static constexpr ImVec4 AccDk  = {0.424f, 0.361f, 0.906f, 1};
        static constexpr ImVec4 Red    = {1.000f, 0.361f, 0.322f, 1};
        static constexpr ImVec4 Txt    = {0.949f, 0.953f, 0.969f, 1};
        static constexpr ImVec4 Dim    = {0.604f, 0.616f, 0.663f, 1};
        static constexpr ImVec4 TrkOff = {0.180f, 0.196f, 0.251f, 1};
        static constexpr ImVec4 Sep    = {0.196f, 0.212f, 0.271f, 1};
    }

    static inline ImVec4 Lerp4(ImVec4 a, ImVec4 b) {
        float t = EaseInOut(g_themeT);
        return {Lerpf(a.x,b.x,t), Lerpf(a.y,b.y,t), Lerpf(a.z,b.z,t), 1.f};
    }

    static inline ImVec4 Bg()     { return Lerp4(Light::Bg,     Dark::Bg);     }
    static inline ImVec4 LeftBg() { return Lerp4(Light::LeftBg, Dark::LeftBg); }
    static inline ImVec4 Card()   { return Lerp4(Light::Card,   Dark::Card);   }
    static inline ImVec4 Acc()    { return Lerp4(Light::Acc,    Dark::Acc);    }
    static inline ImVec4 AccDk()  { return Lerp4(Light::AccDk,  Dark::AccDk);  }
    static inline ImVec4 Red()    { return Lerp4(Light::Red,    Dark::Red);    }
    static inline ImVec4 Txt()    { return Lerp4(Light::Txt,    Dark::Txt);    }
    static inline ImVec4 Dim()    { return Lerp4(Light::Dim,    Dark::Dim);    }
    static inline ImVec4 TrkOff() { return Lerp4(Light::TrkOff, Dark::TrkOff); }
    static inline ImVec4 Sep()    { return Lerp4(Light::Sep,    Dark::Sep);    }

    static ImU32 Mix(ImVec4 a, ImVec4 b, float t) {
        return IM_COL32(
            int(Lerpf(a.x, b.x, t) * 255), int(Lerpf(a.y, b.y, t) * 255),
            int(Lerpf(a.z, b.z, t) * 255), int(Lerpf(a.w, b.w, t) * 255));
    }
    static ImU32 U(ImVec4 v)               { return ImGui::ColorConvertFloat4ToU32(v); }
    static ImU32 UA(ImVec4 v, float alpha) { v.w = alpha; return ImGui::ColorConvertFloat4ToU32(v); }
}

void ApplyTheme() {
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = false;
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 24; s.ChildRounding  = 20; s.FrameRounding = 16;
    s.GrabRounding   = 16; s.PopupRounding  = 16; s.TabRounding   = 16;
    s.WindowBorderSize = 0; s.FrameBorderSize = 0;
    s.ItemSpacing  = {10, 0}; s.FramePadding  = {14, 12}; s.WindowPadding = {0, 0};
    s.ScrollbarSize = 0; s.GrabMinSize = 20;
    auto* c = s.Colors;
    c[ImGuiCol_WindowBg]              = C::Bg();
    c[ImGuiCol_ChildBg]               = {0, 0, 0, 0};
    c[ImGuiCol_Border]                = C::Sep();
    c[ImGuiCol_BorderShadow]          = {0, 0, 0, 0};
    c[ImGuiCol_Text]                  = C::Txt();
    c[ImGuiCol_TextDisabled]          = C::Dim();
    c[ImGuiCol_ScrollbarBg]           = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab]         = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrabHovered]  = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrabActive]   = {0, 0, 0, 0};
    c[ImGuiCol_SliderGrab]            = {0, 0, 0, 0};
    c[ImGuiCol_SliderGrabActive]      = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg]               = {0, 0, 0, 0};
    c[ImGuiCol_FrameBgHovered]        = {0, 0, 0, 0};
    c[ImGuiCol_FrameBgActive]         = {0, 0, 0, 0};
    c[ImGuiCol_Button]                = {0, 0, 0, 0};
    c[ImGuiCol_ButtonHovered]         = {0, 0, 0, 0};
    c[ImGuiCol_ButtonActive]          = {0, 0, 0, 0};
    c[ImGuiCol_Header]                = {0, 0, 0, 0};
    c[ImGuiCol_HeaderHovered]         = {0, 0, 0, 0};
    c[ImGuiCol_HeaderActive]          = {0, 0, 0, 0};
    c[ImGuiCol_NavHighlight]          = {0, 0, 0, 0};
    c[ImGuiCol_NavWindowingHighlight] = {0, 0, 0, 0};
    c[ImGuiCol_Separator]             = C::Sep();
}

namespace Layout {
    static constexpr float RowH      = 78.f;
    static constexpr float SliderH   = 108.f;
    static constexpr float HeaderH   = 66.f;
    static constexpr float Inset     = 16.f;
    static constexpr float PadX      = 20.f;
    static constexpr float BtnH      = 62.f;
    // Нижняя панель вкладок: строка по центру, иконка + подпись.
    // Кнопки крупные: панель выше и ячейки шире (~2x от первоначальных).
    static constexpr float BottomH   = 150.f;
    static constexpr float TabW      = 136.f;
    // Левая панель вкладок: вертикальный столбец по центру.
    static constexpr float RailW     = 128.f;
    static constexpr float TabHV     = 108.f;
}

struct InputState {
    bool touchConsumed = false;
};
static InputState g_input;

struct AppState {
    struct SliderAnim { float pos = -1.f; float vel = 0.f; };
    struct RadioAnim  { float scale = 1.f, scaleVel = 0.f, ring = 0.f, ringVel = 0.f; };

    int   cur_tab = 1;   // при запуске открыта вкладка «Аим»
    bool  aim_touch = false, aim_pos = false, aim_special = false, aim_scope_only = false;
    int   aim_bone = 0;
    // 0 = balanced (crosshair + range), 1 = nearest to the crosshair,
    // 2 = nearest in the world.
    int   aim_priority = 0;
    bool  esp_box = false, esp_name = false, esp_wall = false, esp_chams = false;
    bool  esp_weapon = false, esp_tracer = false, esp_skeleton = false;
    bool  esp_ore = false, esp_animal = false, esp_loot = false, esp_team = false;
    bool  esp_pickup = false;
    float marker_dist = 150.f;
    float esp_thick = 1.5f;
    float gun_str = 5.f, gun_fov = 80.f, gun_trigger_delay = 0.0f;
    // Автофарм: главный выключатель + какие ресурсы добывать.
    bool  farm_on = false;
    bool  farm_wood = true, farm_stone = false, farm_metal = false, farm_sulfur = false;
    // Калибровка зон бота (доли экрана 0..1; -1 = не задано, берём дефолт).
    // Джойстик движения и кнопка огня/атаки — у всех раскладки разные.
    float farm_joy_x = -1.f, farm_joy_y = -1.f;
    float farm_fire_x = -1.f, farm_fire_y = -1.f;
    // Дальность поиска ресурсов, метры.
    float farm_range = 100.f;
    // ui_fps выключен навсегда (счётчик убран), рамки карточек — всегда вкл.
    bool  ui_fps = false, ui_dark_mode = true, ui_show_sep = true;
    // Положение панели вкладок: true = слева (по умолчанию), false = снизу.
    bool  ui_panel_left = true;

    float tab_alpha = 1.f, tab_slide = 0.f, tab_slide_vel = 0.f;
    float a_aim_touch = 0, a_aim_pos = 0, a_aim_spec = 0, a_aim_scope = 0;
    float a_aim_head  = 1, a_aim_chest = 0, a_aim_pelvis = 0;
    RadioAnim ra_aim_head, ra_aim_chest, ra_aim_pelvis;
    float a_aim_pr0 = 1, a_aim_pr1 = 0, a_aim_pr2 = 0;
    RadioAnim ra_aim_pr0, ra_aim_pr1, ra_aim_pr2;
    float a_esp_box = 0, a_esp_name = 0, a_esp_wall = 0, a_esp_chams = 0;
    float a_esp_weapon = 0, a_esp_tracer = 0, a_esp_skeleton = 0;
    float a_esp_ore = 0, a_esp_animal = 0, a_esp_loot = 0, a_esp_team = 0, a_esp_pickup = 0;
    float a_ui_dark = 1;
    float a_farm_on = 0, a_farm_wood = 1, a_farm_stone = 0, a_farm_metal = 0, a_farm_sulfur = 0;

    SliderAnim sl_gun_str, sl_gun_fov, sl_esp_thick, sl_gun_trig, sl_marker_dist, sl_farm_range;
};
static AppState g_state;


static ImU32 ColU32(const ImVec4& c) {
    return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), (int)(c.w * 255));
}

// Radius (px) of the aim FOV circle. Original logic: cfg::aim::fov is an
// angle against a fixed 60° reference (not the live camera FOV), so the circle
// stays put on screen when the player zooms in. 180° = whole screen.
static float AimFovRadiusPx(float sw, float sh) {
    float fov = cfg::aim::fov;
    if (fov <= 0.f) return 0.f;
    if (fov >= 180.f) return sw + sh;
    float t = tanf(fov * 0.5f * (float)M_PI / 180.f) / tanf(30.f * (float)M_PI / 180.f);
    float r = t * (sh * 0.5f);
    if (!std::isfinite(r) || r > sw + sh) r = sw + sh;
    return r;
}

// One remote snapshot per frame, shared by the ESP overlay and the aimbot.
static const std::vector<EspBox>& FrameBoxes(float sw, float sh) {
    static std::vector<EspBox> s_boxes;
    static int s_frame = -1;
    int frame = ImGui::GetFrameCount();
    if (frame != s_frame) {
        s_frame = frame;
        esp_set_skeleton_enabled(g_state.esp_skeleton);
        esp_set_aim_bones_enabled(g_state.aim_touch);
        esp_set_markers_enabled(g_state.esp_ore, g_state.esp_animal,
                                g_state.esp_loot, g_state.esp_pickup);
        esp_set_marker_max_distance(g_state.marker_dist);
        s_boxes = esp_get_boxes((int)sw, (int)sh);
    }
    return s_boxes;
}

static void DrawEspOverlay() {
    float sw = (float)native_window_screen_x;
    float sh = (float)native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float)displayInfo.width;
        sh = (float)displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float)displayInfo.height;
        sh = (float)displayInfo.width;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    if (!g_esp_attached) return;

    if (g_state.aim_touch && g_state.aim_special) {
        float fovR = AimFovRadiusPx(sw, sh);
        if (fovR > 1.f) {
            ImVec4 fc = ImVec4(cfg::aim::fov_color[0], cfg::aim::fov_color[1],
                               cfg::aim::fov_color[2], cfg::aim::fov_color[3]);
            dl->AddCircle(ImVec2(sw * 0.5f, sh * 0.5f), fovR,
                          ImGui::ColorConvertFloat4ToU32(fc), 72, 1.5f);
        }
    }

    if (!g_state.esp_box && !g_state.esp_chams && !g_state.esp_wall && !g_state.esp_tracer && !g_state.esp_skeleton && !g_state.esp_name && !g_state.esp_weapon && !g_state.esp_ore && !g_state.esp_animal) return;

    const std::vector<EspBox>& boxes = FrameBoxes(sw, sh);
    constexpr int BOX_EDGES[][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    float thick = g_state.esp_thick;
    if (thick < 0.5f) thick = 0.5f;

    // Thin dark-gray outline used across all visuals (no bold black strokes).
    const ImU32 kVisOutline = IM_COL32(45, 45, 52, 220);

    // ESP labels in the GUI style: compact rounded pill (light translucent fill
    // + thin gray outline) + colored text. The text is drawn with the same font
    // and size it is measured at (espFont/espFs) so it never spills out of the
    // pill — the old call drew at the full-size font while sizing at 0.8×, which
    // made the distance/weapon text overflow the pill.
    ImFont* espFont = ImGui::GetFont();
    float espFs = ImGui::GetFontSize() * 0.8f;
    auto PillH = [&](const char* text, float scale = 1.f) {
        if (!text || !text[0]) return 0.f;
        ImVec2 tsz = espFont->CalcTextSizeA(espFs * scale, FLT_MAX, 0, text);
        return tsz.y + 4.f * scale;
    };
    auto EspPill = [&](float cx, float y, const char* text, ImU32 textCol, float scale = 1.f) {
        if (!text || !text[0]) return;
        float fsz = espFs * scale;
        ImVec2 tsz = espFont->CalcTextSizeA(fsz, FLT_MAX, 0, text);
        const float padX = 5.f * scale, padY = 2.f * scale;
        float x0 = cx - tsz.x * 0.5f - padX;
        float x1 = cx + tsz.x * 0.5f + padX;
        float y1 = y + tsz.y + padY * 2.f;
        // Light translucent fill (not a dense black block), same for the name,
        // weapon and distance pills so they read consistently over any scene.
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y1), IM_COL32(30, 30, 36, 40), 5.f * scale);
        dl->AddRect(ImVec2(x0, y), ImVec2(x1, y1), kVisOutline, 5.f * scale, 0, 1.0f);
        dl->AddText(espFont, fsz, ImVec2(cx - tsz.x * 0.5f, y + padY), textCol, text);
    };
    // A far away player projects to a couple of pixels, which used to make the
    // box degenerate (corners gone, top/bottom strokes fused into one line).
    // Every visual therefore works on a rect that is grown around its own
    // centre up to a stroke-aware minimum.
    //
    // The growth is a single uniform scale, not a per-axis clamp: clamping the
    // axes separately pushed the width of a distant player up to the same
    // minimum as the height and the box turned into a square. Scaling keeps the
    // player's proportions, so a far away box stays a small tall rectangle.
    const float kMinBoxSide = 3.0f * (thick + 1.0f) + 8.0f;      // vertical floor
    const float kMinBoxWidth = 2.0f * 1.5f + (thick + 1.5f);     // 2 stubs + a gap
    auto NormRect = [&](float& x1, float& y1, float& x2, float& y2) {
        if (x2 < x1) { float t = x1; x1 = x2; x2 = t; }
        if (y2 < y1) { float t = y1; y1 = y2; y2 = t; }
        float cx = (x1 + x2) * 0.5f, cy = (y1 + y2) * 0.5f;
        float w = x2 - x1, h = y2 - y1;
        if (!(h > 0.01f)) { h = kMinBoxSide; w = kMinBoxSide * game_offsets::PLAYER_BOX_WIDTH_RATIO; }
        if (h < kMinBoxSide) { float s = kMinBoxSide / h; h *= s; w *= s; }
        if (w < kMinBoxWidth) w = kMinBoxWidth; // only for absurdly narrow rects
        x1 = cx - w * 0.5f; x2 = cx + w * 0.5f;
        y1 = cy - h * 0.5f; y2 = cy + h * 0.5f;
    };
    // Corner box: the middle of every edge is cut out, only corners remain.
    // The corner length is capped per axis so the two segments of one edge can
    // never meet — at any distance a visible gap stays in the middle of the
    // top/bottom (and left/right) edges, which is what makes it read as a
    // corner box instead of a plain rectangle or a single fused line.
    auto CornerBox = [&](float x1, float y1, float x2, float y2, ImU32 col) {
        if (!std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2)) return;
        NormRect(x1, y1, x2, y2);
        float w = x2 - x1, h = y2 - y1;

        float len = (w < h ? w : h) * 0.28f;
        if (len > 42.f) len = 42.f;

        // Keep at least ~30% of every edge (and never less than one stroke
        // plus a pixel) empty in its middle.
        float gapX = w * 0.30f; if (gapX < thick + 1.5f) gapX = thick + 1.5f;
        float gapY = h * 0.30f; if (gapY < thick + 1.5f) gapY = thick + 1.5f;
        float lx = (w - gapX) * 0.5f; if (lx > len) lx = len;
        float ly = (h - gapY) * 0.5f; if (ly > len) ly = len;
        if (lx < 1.5f) lx = 1.5f;
        if (ly < 1.5f) ly = 1.5f;

        const ImVec2 pts[8][2] = {
            {{x1, y1}, {x1 + lx, y1}}, {{x1, y1}, {x1, y1 + ly}},
            {{x2 - lx, y1}, {x2, y1}}, {{x2, y1}, {x2, y1 + ly}},
            {{x1, y2 - ly}, {x1, y2}}, {{x1, y2}, {x1 + lx, y2}},
            {{x2 - lx, y2}, {x2, y2}}, {{x2, y2 - ly}, {x2, y2}},
        };
        for (int i = 0; i < 8; ++i)
            dl->AddLine(pts[i][0], pts[i][1], kVisOutline, thick + 1.0f);
        for (int i = 0; i < 8; ++i)
            dl->AddLine(pts[i][0], pts[i][1], col, thick);
    };

    for (const EspBox& box : boxes) {
        if (!std::isfinite(box.x1) || !std::isfinite(box.y1) || !std::isfinite(box.x2) || !std::isfinite(box.y2)) continue;

        // Rect every visual (box, tracer, labels) is anchored to: never smaller
        // than the stroke-aware minimum, so nothing collapses at long range.
        float bx1 = box.x1, by1 = box.y1, bx2 = box.x2, by2 = box.y2;
        NormRect(bx1, by1, bx2, by2);

        // Team mates / clan mates get their own colour so they read as
        // friendly at a glance (and the aimbot leaves them alone).
        const bool ally = g_state.esp_team && box.ally;

        if (g_state.esp_chams) {
            bool all_valid = true;
            for (int c = 0; c < 8; ++c) {
                if (!box.corner_visible[c] || !std::isfinite(box.corners[c][0]) || !std::isfinite(box.corners[c][1]) || box.corners[c][0] < 0 || box.corners[c][1] < 0) {
                    all_valid = false;
                    break;
                }
            }
            if (all_valid) {
                ImU32 col = ColU32(cfg::esp::box_col_invis);
                // 3D box edges collapse to a single line at long range because the
                // projected top/bottom (and left/right) faces sit within a pixel or
                // two. Measure the on-screen extent and, when it is below a stroke
                // aware minimum, draw a flat, centred box with that minimum span so
                // the horizontal rows stay visibly separated (same guarantee as the
                // corner box) instead of fusing into one line.
                float mnX = FLT_MAX, mnY = FLT_MAX, mxX = -FLT_MAX, mxY = -FLT_MAX;
                for (int c = 0; c < 8; ++c) {
                    float x = box.corners[c][0], y = box.corners[c][1];
                    if (x < mnX) mnX = x; if (x > mxX) mxX = x;
                    if (y < mnY) mnY = y; if (y > mxY) mxY = y;
                }
                float dMin = kMinBoxSide;
                if ((mxX - mnX) >= dMin && (mxY - mnY) >= dMin) {
                    for (const auto& edge : BOX_EDGES) {
                        int a = edge[0], b = edge[1];
                        dl->AddLine(
                            ImVec2(box.corners[a][0], box.corners[a][1]),
                            ImVec2(box.corners[b][0], box.corners[b][1]),
                            col, thick
                        );
                    }
                } else {
                    dl->AddRect(ImVec2(bx1, by1), ImVec2(bx2, by2), kVisOutline, 0.f, 0, thick + 1.0f);
                    dl->AddRect(ImVec2(bx1, by1), ImVec2(bx2, by2), col, 0.f, 0, thick);
                }
            }
        }

        if (g_state.esp_box) {
            CornerBox(bx1, by1, bx2, by2, ColU32(ally ? cfg::esp::ally_col : cfg::esp::box_col));
        }

        if (g_state.esp_skeleton && box.has_skeleton) {
            ImU32 skelCol = ColU32(cfg::esp::skeleton_col);
            auto validPt = [&](int b) {
                return b >= 0 && box.bone_valid[b] &&
                       std::isfinite(box.bones[b][0]) && std::isfinite(box.bones[b][1]);
            };
            auto lineTo = [&](int a, int b) {
                dl->AddLine(ImVec2(box.bones[a][0], box.bones[a][1]),
                            ImVec2(box.bones[b][0], box.bones[b][1]),
                            skelCol, thick);
            };
            // Draw each chain connecting consecutive valid bones, skipping
            // missing ones (0..5 torso, 6..9/10..13 arms, 14..17/18..21 legs).
            auto drawChain = [&](const int* chain, int n, int anchor) {
                int prev = validPt(anchor) ? anchor : -1;
                for (int i = 0; i < n; ++i) {
                    int b = chain[i];
                    if (!validPt(b)) continue;
                    if (prev >= 0) lineTo(prev, b);
                    prev = b;
                }
            };
            // Arms hang off the highest available spine bone.
            int chest = -1;
            for (int c : {3, 2, 1, 0}) { if (validPt(c)) { chest = c; break; } }

            static const int torso[] = {1, 2, 3, 4, 5};
            static const int armL[]  = {6, 7, 8, 9};
            static const int armR[]  = {10, 11, 12, 13};
            static const int legL[]  = {14, 15, 16, 17};
            static const int legR[]  = {18, 19, 20, 21};
            drawChain(torso, 5, 0);
            drawChain(armL, 4, chest);
            drawChain(armR, 4, chest);
            drawChain(legL, 4, 0);
            drawChain(legR, 4, 0);
        }



        if (g_state.esp_tracer) {
            // From the middle of the top edge of the screen to the head area.
            // Team mates get a green line so a glance at the tracer is enough.
            float tx = (bx1 + bx2) * 0.5f;
            ImU32 tracerCol = ColU32(ally ? cfg::esp::ally_tracer_col : cfg::esp::tracer_col);
            dl->AddLine(ImVec2(sw * 0.5f, 0.0f), ImVec2(tx, by1),
                        kVisOutline, thick + 1.0f);
            dl->AddLine(ImVec2(sw * 0.5f, 0.0f), ImVec2(tx, by1), tracerCol, thick);
        }

        // Name above the box; distance right under the box, weapon under the
        // distance.
        {
            float cx = (bx1 + bx2) * 0.5f;
            const float gap = 5.f;
            if (g_state.esp_name && box.has_name && box.name[0]) {
                // Clan tag in front of the nick, the way the game shows it.
                char label[56];
                if (box.has_tag && box.tag[0])
                    snprintf(label, sizeof(label), "[%s] %s", box.tag, box.name);
                else
                    snprintf(label, sizeof(label), "%s", box.name);
                float h = PillH(label);
                EspPill(cx, by1 - h - gap, label,
                        ColU32(ally ? cfg::esp::ally_col : cfg::esp::name_col));
            }
            float belowY = by2 + gap;
            if (g_state.esp_wall) {
                char label[32];
                if (box.distance >= 0.0f) snprintf(label, sizeof(label), "%.1fm", box.distance);
                else snprintf(label, sizeof(label), "PLAYER");
                EspPill(cx, belowY, label, ColU32(cfg::esp::distance_col));
                belowY += PillH(label) + 4.f;
            }
            if (g_state.esp_weapon && box.has_weapon && box.weapon[0]) {
                EspPill(cx, belowY, box.weapon, ColU32(cfg::esp::weapon_col));
                belowY += PillH(box.weapon) + 4.f;
            }
        }
    }

    // ---- World markers: ore nodes and animals ----------------------------
    // One pill with the resource / animal name at the object's position; the
    // scan itself is done by the game layer and reuses this frame's camera.
    if (g_state.esp_ore || g_state.esp_animal || g_state.esp_loot || g_state.esp_pickup) {
        // Smaller than the player labels (there are many more of them), with the
        // distance on a second line underneath.
        constexpr float kMarkerScale = 0.78f;
        // Elite crates pulse through the spectrum: one hue for all of them per
        // frame (a full turn every two seconds) so they cannot be missed.
        const float rainbow_hue = fmodf((float)ImGui::GetTime() * 0.5f, 1.0f);
        float rr = 1.f, rg = 1.f, rb = 1.f;
        ImGui::ColorConvertHSVtoRGB(rainbow_hue, 0.85f, 1.0f, rr, rg, rb);
        const ImU32 rainbow_col = IM_COL32((int)(rr * 255.f), (int)(rg * 255.f), (int)(rb * 255.f), 255);
        for (const EspMarker& marker : esp_get_markers()) {
            if (!marker.name[0]) continue;
            if (!std::isfinite(marker.x) || !std::isfinite(marker.y)) continue;
            ImU32 col = marker.rainbow ? rainbow_col
                : marker.has_color
                ? IM_COL32(marker.color_rgb[0], marker.color_rgb[1], marker.color_rgb[2], 255)
                : ColU32(marker.kind == ESP_MARKER_LOOT   ? cfg::esp::loot_col
                       : marker.kind == ESP_MARKER_PICKUP ? cfg::esp::pickup_col
                                                          : cfg::esp::animal_col);
            EspPill(marker.x, marker.y, marker.name, col, kMarkerScale);
            char label[24];
            snprintf(label, sizeof(label), "%.0fm", marker.distance);
            EspPill(marker.x, marker.y + PillH(marker.name, kMarkerScale) + 2.f, label,
                    ColU32(cfg::esp::distance_col), kMarkerScale);
        }
    }
}

static constexpr int  kMaxConfigs  = 12;
static constexpr uint8_t kXorKey   = 0xA7;

static const char* kCfgDir_() noexcept {
    static constexpr auto _s = xp::_mk("/storage/emulated/0/benzhack/");
    return _s.d();
}
#define kCfgDir (kCfgDir_())

struct ConfigEntry { char name[64] = {}; };
static ConfigEntry g_configs[kMaxConfigs] = {};
static int         g_configCount    = 0;
static int         g_configToDelete = -1;
static char        g_loadedConfigName[64] = {};
static float       g_cfgLoadAnim[kMaxConfigs] = {};
static int         g_cfgLoadedIdx = -1;

static std::string CfgPath(const char* name) {
    return std::string(kCfgDir) + name + XS(".cfg");
}

static void XorBuf(uint8_t* buf, size_t sz) {
    for (size_t i = 0; i < sz; i++) buf[i] ^= (kXorKey ^ (uint8_t)(i * 0x1D + 0x3B));
}

static void CfgScanDir() {
    mkdir(kCfgDir, 0777);
    g_configCount = 0;
    DIR* d = opendir(kCfgDir);
    if (!d) return;
    struct dirent* e;
    std::vector<std::string> names;
    while ((e = readdir(d)) != nullptr) {
        std::string fn = e->d_name;
        if (fn.size() > 4 && fn.substr(fn.size() - 4) == XS(".cfg"))
            names.push_back(fn.substr(0, fn.size() - 4));
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        if (g_configCount >= kMaxConfigs) break;
        snprintf(g_configs[g_configCount++].name, 64, "%s", n.c_str());
    }
}

static int g_inotifyFd  = -1;
static int g_inotifyWd  = -1;

static void CfgWatchInit() {
    g_inotifyFd = inotify_init1(IN_NONBLOCK);
    if (g_inotifyFd < 0) return;
    mkdir(kCfgDir, 0777);
    g_inotifyWd = inotify_add_watch(g_inotifyFd, kCfgDir,
        IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY);
}

static void CfgWatchFree() {
    if (g_inotifyFd >= 0) {
        if (g_inotifyWd >= 0) inotify_rm_watch(g_inotifyFd, g_inotifyWd);
        close(g_inotifyFd);
        g_inotifyFd = -1;
        g_inotifyWd = -1;
    }
}

static void CfgWatchTick() {
    if (g_inotifyFd < 0) return;
    char buf[512];
    ssize_t n = read(g_inotifyFd, buf, sizeof(buf));
    if (n > 0) CfgScanDir();
}

struct CfgBlob {
    uint32_t magic;
    uint32_t version;
    bool  aim_touch, aim_pos, aim_special;
    int   aim_bone;
    bool  aim_vis_check, aim_draw_fov;
    //   aim_smoothness -> aim_lead   (smoothing has its own gun_str slot,
    //                                 and this one was only ever a mirror).
    //                                 Reused: farm search range in metres
    //                                 (0 in old configs = use the default).
    float aim_fov, aim_lead;
    // Every field that a feature outgrew is renamed in place rather than
    // appended, so the byte layout — and with it version 4 and every config
    // already saved on a device — stays valid. Current renames:
    //   esp_hp        -> esp_ore          esp_ping        -> esp_animal
    //   esp_weapon_icon -> esp_team
    //   esp_health_col  -> esp_ally_col   esp_money_col   -> esp_animal_col
    //   esp_weapon_icon_col -> esp_loot_col
    //   esp_ping_col    -> esp_extra (packed scalars, see below)
    //   aim_smoothness  -> aim_lead
    bool  esp_box, esp_name, esp_ore, esp_wall, esp_chams;
    bool  esp_weapon, esp_team, esp_tracer, esp_skeleton;
    bool  aim_scope_only, esp_animal, esp_vis_check, esp_fill;
    float esp_thick, esp_stroke, esp_rounding, esp_fill_pct;
    float gun_str, gun_fov, gun_trigger_delay;
    bool  ui_fps, ui_dark_mode, ui_show_sep;
    ImVec4 esp_box_col, esp_box_col_invis, esp_name_col, esp_ally_col, esp_distance_col;
    ImVec4 esp_weapon_col, esp_loot_col, esp_tracer_col, esp_skeleton_col, esp_animal_col;
    // Four scalars that had no slot of their own:
    //   x = bit 0 loot ESP, bit 1 pickup ESP,
    //   y = marker draw distance (m),
    //   z = aim priority + 1 (so an old config's 1.0 still means "default"),
    //   w = pickup colour packed as r*65536 + g*256 + b (exact in a float,
    //       since 0xFFFFFF < 2^24); <= 1 means "never written, use default".
    ImVec4 esp_extra;
    int   esp_box_type;
    float esp_box_rounding;
};

static void ConfigSaveToPath(const std::string& path) {
    CfgBlob s;
    s.magic   = 0x58564345U;
    s.version = 4;
    s.aim_touch   = g_state.aim_touch;   s.aim_pos     = g_state.aim_pos;
    s.aim_special = g_state.aim_special;
    s.aim_bone    = g_state.aim_bone;
    s.aim_vis_check  = cfg::aim::vis_check;
    s.aim_draw_fov   = cfg::aim::draw_fov;
    s.aim_fov        = cfg::aim::fov;
    // Слот aim_lead давно свободен (см. CfgBlob) — теперь в нём живёт
    // дальность автофарма. Старые конфиги держат тут 0 и дадут дефолт.
    s.aim_lead       = g_state.farm_range;
    s.esp_box     = g_state.esp_box;     s.esp_name    = g_state.esp_name;
    s.esp_ore     = g_state.esp_ore;     s.esp_wall    = g_state.esp_wall;
    s.esp_chams   = g_state.esp_chams;
    s.esp_weapon      = g_state.esp_weapon;
    s.esp_team        = g_state.esp_team;
    s.esp_tracer      = g_state.esp_tracer;
    s.esp_skeleton    = g_state.esp_skeleton;
    s.aim_scope_only  = g_state.aim_scope_only;
    s.esp_animal      = g_state.esp_animal;
    s.esp_vis_check   = cfg::esp::vis_check;
    s.esp_fill        = cfg::esp::fill;
    s.esp_thick       = g_state.esp_thick;
    s.esp_stroke      = cfg::esp::stroke;
    s.esp_rounding    = cfg::esp::rounding;
    s.esp_fill_pct    = cfg::esp::fill_pct;
    s.gun_str     = g_state.gun_str;
    s.gun_fov     = g_state.gun_fov;
    s.gun_trigger_delay     = g_state.gun_trigger_delay;
    s.ui_fps      = g_state.ui_fps;      s.ui_dark_mode= g_state.ui_dark_mode;
    s.ui_show_sep = g_state.ui_show_sep;
    s.esp_box_col          = cfg::esp::box_col;
    s.esp_box_col_invis    = cfg::esp::box_col_invis;
    s.esp_name_col         = cfg::esp::name_col;
    s.esp_ally_col         = cfg::esp::ally_col;
    s.esp_distance_col     = cfg::esp::distance_col;
    s.esp_weapon_col       = cfg::esp::weapon_col;
    s.esp_loot_col         = cfg::esp::loot_col;
    s.esp_tracer_col       = cfg::esp::tracer_col;
    s.esp_skeleton_col     = cfg::esp::skeleton_col;
    s.esp_animal_col       = cfg::esp::animal_col;
    {
        const int flags = (g_state.esp_loot ? 1 : 0) | (g_state.esp_pickup ? 2 : 0);
        auto ch = [](float v) { int i = (int)(v * 255.f + 0.5f); return i < 0 ? 0 : (i > 255 ? 255 : i); };
        const float packed = (float)(ch(cfg::esp::pickup_col.x) * 65536
                                   + ch(cfg::esp::pickup_col.y) * 256
                                   + ch(cfg::esp::pickup_col.z));
        s.esp_extra = {(float)flags, g_state.marker_dist,
                       (float)(g_state.aim_priority + 1), packed};
    }
    s.esp_box_type         = cfg::esp::box_type;
    s.esp_box_rounding     = cfg::esp::box_rounding;
    uint8_t buf[sizeof(s)];
    memcpy(buf, &s, sizeof(s));
    XorBuf(buf, sizeof(s));
    std::ofstream f(path, std::ios::binary);
    if (f) { f.write((char*)buf, sizeof(buf)); f.close(); }
}

static void ConfigSave() {
    mkdir(kCfgDir, 0777);
    int idx = 1;
    char baseName[64];
    std::string cfgpath;
    do {
        snprintf(baseName, sizeof(baseName), XS("config%d"), idx++);
        cfgpath = CfgPath(baseName);
    } while (access(cfgpath.c_str(), F_OK) == 0);
    ConfigSaveToPath(cfgpath);
    CfgScanDir();
    ShowToast(XS("Конфиг создан"));
    PlaySound(SND_SUCCESS);
}

static void ConfigUpdate(int idx) {
    if (idx < 0 || idx >= g_configCount) return;
    std::string path = CfgPath(g_configs[idx].name);
    ConfigSaveToPath(path);
    ShowToast(XS("Конфиг сохранён"));
    PlaySound(SND_SUCCESS);
}

static void ConfigLoad(int idx) {
    if (idx < 0 || idx >= g_configCount) return;
    std::string path = CfgPath(g_configs[idx].name);
    std::ifstream f(path, std::ios::binary);
    if (!f) { ShowToast(XS("Файл не найден")); return; }

    CfgBlob s;

    uint8_t buf[sizeof(s)];
    f.read((char*)buf, sizeof(buf));
    size_t got = (size_t)f.gcount();
    f.close();
    if (got != sizeof(buf)) { ShowToast(XS("Несовместимый конфиг")); return; }
    XorBuf(buf, sizeof(s));
    memcpy(&s, buf, sizeof(s));
    if (s.magic != 0x58564345U || s.version != 4) { ShowToast(XS("Старый конфиг — пересохрани")); return; }

    g_state.aim_touch   = s.aim_touch;
    // «Только видимых» убран из меню — значение из конфига игнорируется.
    g_state.aim_pos     = false;
    g_state.aim_special = s.aim_special;
    g_state.aim_scope_only = s.aim_scope_only;
    cfg::aim::scope_only   = s.aim_scope_only;
    g_state.aim_bone    = s.aim_bone;
    cfg::aim::vis_check  = false;
    cfg::aim::draw_fov   = s.aim_draw_fov;
    cfg::aim::fov        = s.aim_fov;
    // Дальность автофарма переехала в бывший слот aim_lead; в старых
    // конфигах там 0 — тогда остаётся дефолт 100 м.
    g_state.farm_range   = (s.aim_lead >= 10.f && s.aim_lead <= 300.f) ? s.aim_lead : 100.f;
    g_state.esp_box     = s.esp_box;     g_state.esp_name    = s.esp_name;
    g_state.esp_wall    = s.esp_wall;
    g_state.esp_chams   = s.esp_chams;
    g_state.esp_weapon      = s.esp_weapon;
    g_state.esp_tracer      = s.esp_tracer;
    g_state.esp_skeleton    = s.esp_skeleton;
    g_state.esp_ore         = s.esp_ore;
    g_state.esp_animal      = s.esp_animal;
    g_state.esp_team        = s.esp_team;
    // Packed scalars. A config written before these existed holds the old ping
    // colour here, so each value is range-checked and falls back to its default.
    {
        const int flags = (s.esp_extra.x >= 0.f && s.esp_extra.x < 4.f) ? (int)(s.esp_extra.x + 0.5f) : 0;
        g_state.esp_loot   = (flags & 1) != 0;
        g_state.esp_pickup = (flags & 2) != 0;
        g_state.marker_dist = (s.esp_extra.y >= 25.f && s.esp_extra.y <= 300.f) ? s.esp_extra.y : 150.f;
        int pr = (int)(s.esp_extra.z + 0.5f) - 1;
        g_state.aim_priority = (pr >= 0 && pr <= 2) ? pr : 0;
        // Packed pickup colour. Configs written before it existed hold the old
        // ping alpha (exactly 1.0) here, which is why the check is "> 1".
        if (s.esp_extra.w > 1.f && s.esp_extra.w <= 16777215.f) {
            int rgb = (int)(s.esp_extra.w + 0.5f);
            cfg::esp::pickup_col = ImVec4(((rgb >> 16) & 0xFF) / 255.f,
                                          ((rgb >> 8) & 0xFF) / 255.f,
                                          (rgb & 0xFF) / 255.f, 1.f);
        }
    }
    g_state.esp_thick   = s.esp_thick;
    g_state.gun_str     = s.gun_str;
    g_state.gun_fov     = s.gun_fov;
    if (!(g_state.gun_fov >= 5.f)) g_state.gun_fov = 5.f;
    if (g_state.gun_fov > 180.f) g_state.gun_fov = 180.f;
    g_state.gun_trigger_delay     = s.gun_trigger_delay;
    g_state.ui_dark_mode= s.ui_dark_mode;
    // ui_fps и ui_show_sep из конфига игнорируются: счётчик FPS убран,
    // рамки карточек всегда включены.
    g_state.ui_fps      = false;
    g_state.ui_show_sep = true;
    cfg::esp::box_col          = s.esp_box_col;
    cfg::esp::box_col_invis    = s.esp_box_col_invis;
    cfg::esp::name_col         = s.esp_name_col;
    cfg::esp::distance_col     = s.esp_distance_col;
    cfg::esp::weapon_col       = s.esp_weapon_col;
    cfg::esp::tracer_col       = s.esp_tracer_col;
    cfg::esp::skeleton_col     = s.esp_skeleton_col;
    cfg::esp::animal_col       = s.esp_animal_col;
    cfg::esp::loot_col         = s.esp_loot_col;
    // The ally colour reuses a slot that older configs left pure black.
    if (s.esp_ally_col.x + s.esp_ally_col.y + s.esp_ally_col.z > 0.05f)
        cfg::esp::ally_col     = s.esp_ally_col;
    cfg::esp::box_type         = s.esp_box_type;
    cfg::esp::box_rounding     = s.esp_box_rounding;
    g_darkTheme = g_state.ui_dark_mode;
    snprintf(g_loadedConfigName, sizeof(g_loadedConfigName), "%s", g_configs[idx].name);
    g_cfgLoadedIdx = idx;
    for (int i = 0; i < kMaxConfigs; i++) g_cfgLoadAnim[i] = 0.f;
    ShowToast(XS("Конфиг загружен"));
    PlaySound(SND_SUCCESS);
}

static void ConfigDelete(int idx) {
    if (idx < 0 || idx >= g_configCount) return;
    std::string path = CfgPath(g_configs[idx].name);
    if (strcmp(g_configs[idx].name, g_loadedConfigName) == 0) {
        g_loadedConfigName[0] = '\0';
        g_cfgLoadedIdx = -1;
        for (int i = 0; i < kMaxConfigs; i++) g_cfgLoadAnim[i] = 0.f;
    }
    remove(path.c_str());
    CfgScanDir();
    ShowToast(XS("Конфиг удалён"));
    PlaySound(SND_CLICK);
}

struct ScrollState {
    float off      = 0.f;
    float vel      = 0.f;
    float lastTy   = 0.f;
    bool  drag     = false;
    bool  dragging = false;
    int   axis     = 0;
    float sb_alpha = 0.f;
    float sb_idle  = 0.f;
    float sb_w     = 3.f;
    float sb_y     = 0.f;
    float sb_h     = 48.f;
    bool  sb_hot   = false;
};

// Позиция «пилюли» активной вкладки на нижней панели (экранный X) + пружина.
static float pill_y   = -1.f;
static float pill_vel =  0.f;

struct TabRect { float sx; };
static TabRect tab_rects[kTabCount];

static void ScrollTick(ScrollState& s, bool mIn, bool blocked, float& maxScroll, float dt) {
    auto& io = ImGui::GetIO();

    if (!io.MouseDown[0]) {
        s.drag     = false;
        s.dragging = false;
        s.axis     = 0;
    }

    if (!blocked && mIn && io.MouseDown[0]) {
        if (io.MouseClicked[0]) {
            s.lastTy = io.MousePos.y;
            s.vel    = 0.f;
            s.axis   = 0;
        }

        if (s.axis == 0) {
            float dx = fabsf(io.MousePos.x - io.MouseClickedPos[0].x);
            float dy = fabsf(io.MousePos.y - io.MouseClickedPos[0].y);
            if (dx > 22.f || dy > 22.f)
                s.axis = (dy >= dx) ? 1 : 2;
            s.lastTy = io.MousePos.y;
        }

        if (s.axis == 1) {
            if (!s.drag) {
                s.drag     = true;
                s.dragging = true;
                s.lastTy   = io.MousePos.y;
                s.vel      = 0.f;
            }
            float dy = io.MousePos.y - s.lastTy;
            s.lastTy = io.MousePos.y;
            s.off    = ImClamp(s.off - dy, 0.f, maxScroll);
            float targetV = -dy * 18.f;
            s.vel = s.vel * 0.8f + targetV * 0.2f;
        }

        if (s.axis == 2) {
            s.lastTy = io.MousePos.y;
            s.vel    = 0.f;
        }
    }

    if (mIn && !blocked && io.MouseWheel != 0.f) {
        s.vel = 0.f;
        s.off = ImClamp(s.off - io.MouseWheel * 50.f, 0.f, maxScroll);
    }

    if (!s.drag && fabsf(s.vel) > 0.3f) {
        s.off += s.vel * dt;
        s.vel *= 0.88f;
    }

    s.off = ImClamp(s.off, 0.f, maxScroll);
    if (s.off <= 0.f || s.off >= maxScroll) s.vel = 0.f;

    bool isScrolling = s.drag || fabsf(s.vel) > 0.5f;
    if (isScrolling) s.sb_idle = 0.f;
    else s.sb_idle += dt;
    float sbTarget = (isScrolling || s.sb_idle < 0.8f) ? 1.f : 0.f;
    float sbSpd = sbTarget > s.sb_alpha ? 6.f : 3.5f;
    s.sb_alpha += (sbTarget - s.sb_alpha) * sbSpd * dt;
    s.sb_alpha = ImClamp(s.sb_alpha, 0.f, 1.f);
}

static ScrollState g_scrollMain;
static ScrollState g_scrollPop;

static inline bool IsScrollDragging() { return g_scrollMain.dragging || g_scrollPop.dragging; }

static ImVec2 g_popOpenClickPos = {-9999.f, -9999.f};

static const float WW_MIN = 840.f;
static const float WH_MIN = 720.f;

struct WindowState {
    ImVec2 pos              = {-1.f, -1.f};
    float  w                = WW_MIN;
    float  h                = WH_MIN;
    bool   dragging         = false;
    ImVec2 touchStart       = {0, 0};
    ImVec2 posStart         = {0, 0};
    bool   resizing         = false;
    ImVec2 resizeTouchStart = {0, 0};
    ImVec2 sizeStart        = {WW_MIN, WH_MIN};
};
static WindowState g_win;

struct Sheet {
    bool   visible    = false;
    float  anim       = 0.f;
    float  vel        = 0.f;
    bool   closing    = false;
    int    openFrames = 0;
    char   title[64]  = {};
    int    type       = 0;
    bool*  boolP      = nullptr;
    float* animP      = nullptr;
    float* slP        = nullptr;
    float  slMin      = 0;
    float  slMax      = 1;
    const char* slFmt = "%.1f";
};
static Sheet g_sheet;

void SheetOpen(const char* title, int type, bool* bp = nullptr, float* ap = nullptr,
               float* sp = nullptr, float mn = 0, float mx = 1, const char* fmt = "%.1f") {
    if (g_sheet.visible) return;
    snprintf(g_sheet.title, sizeof(g_sheet.title), "%s", title);
    g_sheet.type    = type; g_sheet.boolP = bp; g_sheet.animP = ap;
    g_sheet.slP     = sp;   g_sheet.slMin = mn; g_sheet.slMax = mx; g_sheet.slFmt = fmt;
    g_sheet.visible = true;  g_sheet.anim  = 0.f; g_sheet.vel = 0.f;
    g_sheet.closing = false; g_sheet.openFrames = 0;
}

void SheetClose() {
    if (!g_sheet.visible || g_sheet.closing) return;
    if (g_sheet.openFrames < 4) return;
    g_sheet.closing = true;
}

struct Popover {
    bool  visible    = false;
    bool  closing    = false;
    float anim       = 0.f;
    float vel        = 0.f;
    int   openFrames = 0;
    int   sectionId  = 0;
    char  title[64]  = {};
};
static Popover g_pop;

static ImVec4* g_colP = nullptr;
static bool    g_palSelReset = false;


static const ImVec4 kColorPalette[] = {
    {1.00f, 0.20f, 0.20f, 1.f}, {1.00f, 0.50f, 0.50f, 1.f}, {0.80f, 0.05f, 0.05f, 1.f},
    {1.00f, 0.60f, 0.10f, 1.f}, {1.00f, 0.75f, 0.30f, 1.f}, {0.90f, 0.40f, 0.00f, 1.f},
    {1.00f, 0.95f, 0.10f, 1.f}, {1.00f, 0.85f, 0.50f, 1.f}, {0.85f, 0.75f, 0.00f, 1.f},
    {0.20f, 0.85f, 0.35f, 1.f}, {0.50f, 1.00f, 0.50f, 1.f}, {0.05f, 0.55f, 0.15f, 1.f},
    {0.10f, 0.90f, 0.90f, 1.f}, {0.40f, 0.85f, 1.00f, 1.f}, {0.00f, 0.60f, 0.70f, 1.f},
    {0.10f, 0.50f, 1.00f, 1.f}, {0.50f, 0.70f, 1.00f, 1.f}, {0.00f, 0.25f, 0.75f, 1.f},
    {0.65f, 0.20f, 1.00f, 1.f}, {0.80f, 0.55f, 1.00f, 1.f}, {0.45f, 0.05f, 0.70f, 1.f},
    {1.00f, 0.30f, 0.80f, 1.f}, {1.00f, 0.60f, 0.90f, 1.f}, {0.80f, 0.05f, 0.50f, 1.f},
    {1.00f, 1.00f, 1.00f, 1.f}, {0.70f, 0.70f, 0.70f, 1.f}, {0.35f, 0.35f, 0.35f, 1.f},
    {0.12f, 0.12f, 0.12f, 1.f},
};
static constexpr int kPaletteCount = (int)(sizeof(kColorPalette)/sizeof(kColorPalette[0]));

void PopoverOpenColor(const char* title, ImVec4* cp) {
    if (g_pop.visible) return;
    snprintf(g_pop.title, sizeof(g_pop.title), "%s", title);
    g_pop.sectionId  = 4;
    g_pop.visible    = true;
    g_pop.closing    = false;
    g_pop.anim       = 0.f;
    g_pop.vel        = 0.f;
    g_pop.openFrames = 0;
    g_scrollPop      = {};
    g_colP = cp;
    g_palSelReset = true;
    g_popOpenClickPos = ImGui::GetIO().MousePos;
    auto& _io = ImGui::GetIO();
    Blur::Freeze((int)_io.DisplaySize.x, (int)_io.DisplaySize.y,
                 (int)g_win.pos.x, (int)g_win.pos.y,
                 (int)g_win.w,     (int)g_win.h);
}

void PopoverOpen(const char* title, int sid) {
    if (g_pop.visible) return;
    snprintf(g_pop.title, sizeof(g_pop.title), "%s", title);
    g_pop.sectionId  = sid;
    g_pop.visible    = true;
    g_pop.closing    = false;
    g_pop.anim       = 0.f;
    g_pop.vel        = 0.f;
    g_pop.openFrames = 0;
    g_scrollPop      = {};
    auto& _io = ImGui::GetIO();
    Blur::Freeze((int)_io.DisplaySize.x, (int)_io.DisplaySize.y,
                 (int)g_win.pos.x, (int)g_win.pos.y,
                 (int)g_win.w,     (int)g_win.h);
}

void PopoverClose() {
    if (!g_pop.visible || g_pop.closing) return;
    if (g_pop.openFrames < 1) return;
    g_pop.closing = true;
    g_input.touchConsumed = true;
}

static void DrawToggle(ImDrawList* dl, float ax, float cy, float t) {
    const float tW = 72.f, tH = 42.f, tR = tH * 0.5f, kR = tR - 3.5f;
    float tx = ax, ty = cy - tH * 0.5f;
    dl->AddRectFilled({tx, ty}, {tx + tW, ty + tH}, C::Mix(C::TrkOff(), C::Acc(), t), tR);
    float kx = Lerpf(tx + tR + 2.f, tx + tW - tR - 2.f, t);
    dl->AddCircleFilled({kx + 0.5f, cy + 1.5f}, kR + 1.f, IM_COL32(0, 0, 0, 35));
    dl->AddCircleFilled({kx, cy}, kR, IM_COL32(255, 255, 255, 255));
}

static void RenderToggleRowVisuals(ImDrawList* dl, ImFont* fn, float fs,
                                    float rowX, float rowY, float rowW,
                                    const char* lbl, float animT, float alpha,
                                    bool showSep, bool last) {
    const float inset = Layout::Inset, padX = Layout::PadX;
    float cX = rowX + inset, cW = rowW - inset * 2.f;
    const float rowH = Layout::RowH;
    float textX    = cX + padX;
    float textMaxX = cX + cW - 72.f - padX - 16.f;
    float textY    = rowY + (rowH - fs * 1.15f) * 0.5f;

    dl->PushClipRect({textX, rowY}, {textMaxX, rowY + rowH}, true);
    dl->AddText(fn, fs * 1.15f, {textX, textY}, C::UA(C::Txt(), alpha), lbl);
    dl->PopClipRect();

    DrawToggle(dl, cX + cW - 72.f - padX, rowY + rowH * 0.5f, EaseInOut(animT));

    if (!last && showSep)
        dl->AddLine({cX + padX, rowY + rowH - 0.5f},
                    {cX + cW - padX, rowY + rowH - 0.5f}, C::UA(C::Sep(), alpha), 0.8f);
}

static void RenderSliderVisuals(ImDrawList* dl, ImFont* fn, float fs,
                                 float rowX, float rowY, float rowW, float rowH,
                                 const char* lbl, const char* fmt, float v, float animPos,
                                 bool active, float alpha, bool showSep, bool last) {
    const float inset = Layout::Inset, padX = Layout::PadX;
    const float tH = 11.f, kR = 22.f, padH = 38.f;
    float cX = rowX + inset, cW = rowW - inset * 2.f;

    char valBuf[24]; snprintf(valBuf, 24, fmt, v);
    auto vsz   = fn->CalcTextSizeA(fs, FLT_MAX, 0, valBuf);
    float textY = rowY + padH * 0.5f;

    float sX = cX + padX + kR, sW = cW - padX * 2.f - kR * 2.f;
    float tY = rowY + rowH - padH;
    float kx = sX + animPos * sW;

    dl->PushClipRect({cX + padX, rowY}, {cX + cW - vsz.x - padX * 2.f, rowY + rowH * 0.55f}, true);
    dl->AddText(fn, fs * 1.15f, {cX + padX, textY}, C::UA(C::Txt(), alpha), lbl);
    dl->PopClipRect();
    dl->AddText(fn, fs * 1.15f, {cX + cW - vsz.x - padX, textY}, C::UA(C::Acc(), alpha), valBuf);

    dl->AddRectFilled({sX, tY - tH * 0.5f}, {sX + sW, tY + tH * 0.5f}, C::UA(C::TrkOff(), alpha), tH);
    dl->AddRectFilled({sX, tY - tH * 0.5f}, {kx,      tY + tH * 0.5f}, C::UA(C::Acc(),    alpha), tH);

    const float knobR = 16.f;
    dl->AddCircleFilled({kx + 0.5f, tY + 1.5f}, knobR + 1.f, IM_COL32(0, 0, 0, int(30 * alpha)));
    dl->AddCircleFilled({kx, tY}, knobR, IM_COL32(255, 255, 255, int(255 * alpha)));
    if (active)
        dl->AddCircle({kx, tY}, knobR + 4.f, C::UA(C::Light::Acc, 0.85f * alpha), 48, 3.f);

    if (!last && showSep)
        dl->AddLine({cX + padX, rowY + rowH - 0.5f},
                    {cX + cW - padX, rowY + rowH - 0.5f}, C::UA(C::Sep(), alpha), 0.8f);
}

bool ToggleRow(const char* id, const char* lbl, bool* v, float& anim,
               bool last = false, bool first = false) {
    auto* dl  = ImGui::GetWindowDrawList();
    float avW = ImGui::GetContentRegionAvail().x;
    const float rowH = Layout::RowH;
    auto pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, {avW, rowH});
    bool popBlocking = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

    bool triggered = WasTappedHere() && !popBlocking && !IsScrollDragging() && !g_input.touchConsumed;
    if (triggered) { *v=!*v; char _b[160]; snprintf(_b,sizeof(_b),XS("%s|%s"),lbl,*v?XS("ON"):XS("OFF")); ShowToast(_b); PlaySound(SND_CLICK); }

    RenderToggleRowVisuals(dl, ImGui::GetFont(), ImGui::GetFontSize(),
                           pos.x, pos.y, avW, lbl, anim, 1.f,
                           g_state.ui_show_sep, last);
    return triggered;
}

static void TickSliderAnim(AppState::SliderAnim& anim, float target, bool act, float dt) {
    if (anim.pos < 0.f) { anim.pos = target; anim.vel = 0.f; }
    const float stiff = act ? 900.f : 280.f;
    const float damp  = act ?  60.f :  28.f;
    float force = stiff * (target - anim.pos) - damp * anim.vel;
    anim.vel += force * dt;
    anim.pos += anim.vel * dt;
    anim.pos  = ImClamp(anim.pos, 0.f, 1.f);
    if (fabsf(target - anim.pos) < 0.0002f && fabsf(anim.vel) < 0.001f) {
        anim.pos = target; anim.vel = 0.f;
    }
}

bool SliderRow(const char* id, const char* lbl, float* v, float mn, float mx,
               const char* fmt, bool last, bool first,
               AppState::SliderAnim& anim, float dt) {
    auto* dl  = ImGui::GetWindowDrawList();
    auto& io  = ImGui::GetIO();
    float avW = ImGui::GetContentRegionAvail().x;
    const float rowH = Layout::SliderH, tH = 11.f, kR = 22.f, padH = 38.f;
    const float inset = Layout::Inset, padX = Layout::PadX;
    auto pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, {avW, rowH});
    bool itemActive = ImGui::IsItemActive();

    int scrollAxis = g_scrollMain.axis;
    bool scrollVertical = g_scrollMain.dragging || (itemActive && scrollAxis == 1);
    bool axisUndecided  = itemActive && scrollAxis == 0 && io.MouseDown[0];

    bool act = itemActive && !scrollVertical && !axisUndecided;

    if (act)
        *v = mn + Clamp01((io.MousePos.x - (pos.x + inset + padX + kR)) / (avW - inset * 2.f - padX * 2.f - kR * 2.f)) * (mx - mn);

    float target = Clamp01((*v - mn) / (mx - mn));

    TickSliderAnim(anim, target, act, dt);

    RenderSliderVisuals(dl, ImGui::GetFont(), ImGui::GetFontSize(),
                        pos.x, pos.y, avW, rowH,
                        lbl, fmt, *v, anim.pos, act, 1.f,
                        g_state.ui_show_sep, last);
    return act;
}

void CardBg(float h, ImDrawFlags flags = ImDrawFlags_RoundCornersAll) {
    auto* dl  = ImGui::GetWindowDrawList();
    auto  p0  = ImGui::GetCursorScreenPos();
    float w   = ImGui::GetContentRegionAvail().x;
    const float inset = Layout::Inset;
    ImVec2 bMin = {p0.x + inset, p0.y};
    ImVec2 bMax = {p0.x + w - inset, p0.y + h};
    dl->AddRectFilled(bMin, bMax, C::U(C::Card()), R::Card, flags);
    if (g_state.ui_show_sep)
        dl->AddRect(bMin, bMax, C::U(C::Sep()), R::Card, flags, 1.2f);
}

static bool TickSlideAnim(float& anim, float& vel, bool closing, float dt);

void SHdr(const char* t, float top = 18.f) {
    ImGui::Dummy({1.f, top});
    if (t && t[0]) {
        auto*  dl  = ImGui::GetWindowDrawList();
        auto*  fn  = ImGui::GetFont();
        float  fs  = ImGui::GetFontSize() * 1.15f;
        auto   pos = ImGui::GetCursorScreenPos();
        auto   tsz = fn->CalcTextSizeA(fs, FLT_MAX, 0, t);
        // Акцентная «капсула» слева от заголовка секции — визуально
        // связывает секции с индикатором активной вкладки в рейле.
        dl->AddRectFilled({pos.x + 18.f, pos.y + tsz.y * 0.14f},
                          {pos.x + 24.f, pos.y + tsz.y * 0.86f}, C::U(C::Acc()), 3.f);
        dl->AddText(fn, fs, {pos.x + 34.f, pos.y}, C::U(C::Dim()), t);
        ImGui::Dummy({1.f, tsz.y});
    }
    ImGui::Dummy({1.f, 8.f});
}

static void DrawToast(float dt) {
    if (!g_toast.visible) return;
    g_toast.life += dt;

    if (g_toast.hasNext && g_toast.textA < 0.04f) {
        snprintf(g_toast.msg, sizeof(g_toast.msg), "%s", g_toast.nextMsg);
        g_toast.hasNext = false;
        g_toast.barW = 1.f; g_toast.barVel = 0.f;
    }

    if (g_toast.life > 2.6f && !g_toast.closing && !g_toast.hasNext) g_toast.closing = true;

    if (!TickSlideAnim(g_toast.slidePos, g_toast.slideVel, g_toast.closing, dt)) {
        g_toast.slidePos = 0.f; g_toast.slideVel = 0.f; g_toast.visible = false; return;
    }

    float textTgt = (g_toast.hasNext ? 0.f : 1.f);
    g_toast.textA += (textTgt - g_toast.textA) * 14.f * dt;
    g_toast.textA = ImClamp(g_toast.textA, 0.f, 1.f);

    { float bt = g_toast.hasNext ? 1.f : ImMax(0.f, 1.f - g_toast.life / 2.6f);
      float f = 220.f * (bt - g_toast.barW) - 30.f * g_toast.barVel;
      g_toast.barVel += f * dt; g_toast.barW += g_toast.barVel * dt;
      g_toast.barW = ImClamp(g_toast.barW, 0.f, 1.f); }

    auto* fg = ImGui::GetForegroundDrawList();
    auto* fn = ImGui::GetFont();
    float fs  = ImGui::GetFontSize() * 1.55f;
    float scrW = 0.f, scrH = 0.f;
    VisibleScreen(scrW, scrH);

    const char* sep = strchr(g_toast.msg, 124);
    char np[128] = {}, sp[8] = {}; bool isOn = false;
    if (sep) {
        snprintf(np, sizeof(np), "%.*s", (int)(sep - g_toast.msg), g_toast.msg);
        snprintf(sp, sizeof(sp), "%s", sep + 1);
        isOn = (sp[0] == 'O' && sp[1] == 'N' && sp[2] == 0);
    } else {
        snprintf(np, sizeof(np), "%s", g_toast.msg);
    }

    auto nsz = fn->CalcTextSizeA(fs, FLT_MAX, 0, np);
    auto ssz = fn->CalcTextSizeA(fs, FLT_MAX, 0, XS(" - "));
    auto asz = fn->CalcTextSizeA(fs, FLT_MAX, 0, sp);

    float padH = 28.f, padV = 20.f;
    float tWTarget = padH + nsz.x + (sep ? ssz.x + asz.x : 0.f) + padH;
    float tH = nsz.y + padV * 2.f + 10.f;

    if (g_toast.widthAnim < 8.f) { g_toast.widthAnim = tWTarget; g_toast.widthVel = 0.f; }
    {
        float dw = tWTarget - g_toast.widthAnim;
        g_toast.widthVel += (dw * 220.f - g_toast.widthVel * 28.f) * dt;
        g_toast.widthAnim += g_toast.widthVel * dt;
        if (fabsf(dw) < 0.6f && fabsf(g_toast.widthVel) < 4.f) {
            g_toast.widthAnim = tWTarget; g_toast.widthVel = 0.f;
        }
    }
    float tW = g_toast.widthAnim;

    float ease  = EaseInOut(ImClamp(g_toast.slidePos, 0.f, 1.f));
    float offY  = (1.f - ease) * (-(tH + 20.f));
    float alpha = ease;

    float tx = scrW - tW - 16.f;
    float ty = 16.f + offY;

    ImVec4 card = C::Card(), txt = C::Txt(), dim = C::Dim(), acc = C::Acc();

    fg->AddRectFilled({tx + 2.f, ty + 4.f}, {tx + tW + 2.f, ty + tH + 4.f},
        IM_COL32(0, 0, 0, int(26 * alpha)), 26.f);
    fg->AddRectFilled({tx, ty}, {tx + tW, ty + tH},
        IM_COL32(int(card.x*255), int(card.y*255), int(card.z*255), int(alpha * 245)), 26.f);
    fg->AddRect({tx, ty}, {tx + tW, ty + tH},
        C::UA(acc, 0.32f * alpha), 26.f, 0, 1.3f);

    fg->PushClipRect({tx + 1.f, ty + 1.f}, {tx + tW - 1.f, ty + tH - 1.f}, true);

    float cx0 = tx + padH;
    float textY = ty + padV;
    float ta = g_toast.textA * alpha;

    fg->AddText(fn, fs, {cx0, textY},
        IM_COL32(int(txt.x*255), int(txt.y*255), int(txt.z*255), int(ta*255)), np);
    cx0 += nsz.x;

    if (sep) {
        fg->AddText(fn, fs, {cx0, textY},
            IM_COL32(int(dim.x*255), int(dim.y*255), int(dim.z*255), int(ta*170)), XS(" - "));
        cx0 += ssz.x;
        ImVec4 stCol = isOn ? ImVec4{0.196f, 0.843f, 0.294f, 1.f} : ImVec4{1.f, 0.231f, 0.188f, 1.f};
        fg->AddText(fn, fs, {cx0, textY},
            IM_COL32(int(stCol.x*255), int(stCol.y*255), int(stCol.z*255), int(ta*255)), sp);
    }

    {
        float bH  = 10.f, bR = 5.f;
        float bY  = ty + tH - 8.f - bH;
        float bx0 = tx + 22.f, bx1 = tx + tW - 22.f;
        float fx1 = bx0 + g_toast.barW * (bx1 - bx0);
        int ar = int(acc.x*255), ag = int(acc.y*255), ab = int(acc.z*255);
        if (fx1 > bx0 + bR * 2.f)
            fg->AddRectFilled({bx0, bY}, {fx1, bY + bH},
                IM_COL32(ar, ag, ab, int(alpha * 215)), bR);
    }

    fg->PopClipRect();
}

static float wm_ring   = 2.f;
static bool  menu_open = true;

void DrawWatermark(float dt) {
    auto& io  = ImGui::GetIO();
    auto* fn  = ImGui::GetFont();
    auto* fg  = ImGui::GetForegroundDrawList();

    wm_ring += dt * 3.5f;

    ImVec4 acc   = C::Acc();
    ImU32 bgCol  = C::U(C::Card());
    ImU32 brdCol = C::UA(acc, 0.6f);

    float scrW = 0.f, scrH = 0.f;
    VisibleScreen(scrW, scrH);

    // ---- Некликабельная пилюля с названием чита (слева сверху) ---------
    // Тапы по ней не обрабатываются вовсе, так что она «прозрачна» для
    // кликов и ничему не мешает.
    {
        const float fs = 38.f, padX = 24.f, padY = 14.f;
        const char* name = XS("benzhack");
        auto nSz = fn->CalcTextSizeA(fs, FLT_MAX, 0, name);
        float bW = padX * 2.f + nSz.x;
        float bH = padY * 2.f + nSz.y;
        const float bX = 12.f, bY = 12.f;
        fg->AddRectFilled({bX, bY}, {bX + bW, bY + bH}, C::UA(C::Card(), 0.8f), bH * 0.5f);
        fg->AddRect      ({bX, bY}, {bX + bW, bY + bH}, C::UA(acc, 0.5f), bH * 0.5f, 0, 1.5f);
        fg->AddText(fn, fs, {bX + padX, bY + padY}, C::UA(C::Txt(), 0.95f), name);
    }

    // ---- Пилюля-счётчик противников (по центру верха экрана) -----------
    // Показывает, сколько игроков видит ESP; тап по ней открывает/закрывает
    // меню.
    {
        int enemies = 0;
        if (g_esp_attached) {
            // Обновляем снимок кадра (кэшируется на кадр) и берём число
            // игроков вокруг на все 360° — не только тех, кто попал на экран.
            FrameBoxes(scrW, scrH);
            enemies = esp_nearby_player_count();
        }

        const float fs = 40.f, pad = 22.f, bR = 46.f;
        const char* lbl = XS("Противники");
        char cntBuf[16]; snprintf(cntBuf, sizeof(cntBuf), "%d", enemies);
        auto lSz = fn->CalcTextSizeA(fs, FLT_MAX, 0, lbl);
        auto cSz = fn->CalcTextSizeA(fs, FLT_MAX, 0, cntBuf);

        // Точка-индикатор + подпись + число.
        const float dotR = 7.f;
        float bW = pad + dotR * 2.f + 12.f + lSz.x + 14.f + cSz.x + pad;
        float bH = cSz.y + pad;
        float bX = (scrW - bW) * 0.5f;
        const float bY = 12.f;

        bool in = (io.MousePos.x >= bX && io.MousePos.x <= bX + bW &&
                   io.MousePos.y >= bY && io.MousePos.y <= bY + bH);
        if (in && io.MouseClicked[0]) { menu_open = !menu_open; wm_ring = 0.f; }

        if (wm_ring < 1.f) {
            float r = EaseOut3(wm_ring), ex = r * 22.f;
            fg->AddRectFilled({bX - ex, bY - ex}, {bX + bW + ex, bY + bH + ex},
                C::UA(acc, 0.18f * (1.f - r)), bR + ex);
        }

        fg->AddRectFilled({bX + 2.f, bY + 3.f}, {bX + bW + 2.f, bY + bH + 3.f},
            IM_COL32(0, 0, 0, 20), bR);
        fg->AddRectFilled({bX, bY}, {bX + bW, bY + bH}, bgCol, bR);
        fg->AddRect      ({bX, bY}, {bX + bW, bY + bH}, brdCol, bR, 0, 1.5f);

        // Зелёная точка, когда противники рядом есть; серая — когда никого.
        ImVec4 dotCol = enemies > 0 ? ImVec4{0.24f, 0.78f, 0.42f, 1.f} : C::Dim();
        float dcy = bY + bH * 0.5f;
        fg->AddCircleFilled({bX + pad + dotR, dcy}, dotR, C::U(dotCol), 24);
        if (enemies > 0)
            fg->AddCircle({bX + pad + dotR, dcy}, dotR + 3.f,
                C::UA(dotCol, 0.5f + 0.3f * sinf((float)ImGui::GetTime() * 5.f)), 24, 1.8f);

        float tY = bY + (bH - cSz.y) * 0.5f;
        float lX = bX + pad + dotR * 2.f + 12.f;
        fg->AddText(fn, fs, {lX, tY}, C::UA(C::Txt(), 0.85f), lbl);
        fg->AddText(fn, fs, {lX + lSz.x + 14.f, tY}, C::U(acc), cntBuf);
    }
}

bool CollapsibleHeader(const char* id, const char* lbl, int secId = 0) {
    auto* dl  = ImGui::GetWindowDrawList();
    float avW = ImGui::GetContentRegionAvail().x;
    const float rowH = Layout::RowH, inset = Layout::Inset, padX = Layout::PadX;
    auto pos = ImGui::GetCursorScreenPos();

    dl->AddRectFilled({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Card()), R::Card);
    if (g_state.ui_show_sep)
        dl->AddRect({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Sep()), R::Card, 0, 1.2f);

    ImGui::InvisibleButton(id, {avW, rowH});
    bool clicked = WasTappedHere() && !IsScrollDragging() && !g_input.touchConsumed;

    float fs    = ImGui::GetFontSize() * 1.15f;
    float textY = pos.y + (rowH - fs) * 0.5f;
    dl->AddText(ImGui::GetFont(), fs, {pos.x + inset + padX, textY}, C::U(C::Txt()), lbl);

    const float cBtnR = 22.f;
    float cx2 = pos.x + avW - inset - cBtnR - padX * 0.5f, cy2 = pos.y + rowH * 0.5f;
    dl->AddCircleFilled({cx2 + 0.5f, cy2 + 1.5f}, cBtnR + 1.f, IM_COL32(0, 0, 0, 28), 48);
    dl->AddCircleFilled({cx2, cy2}, cBtnR, C::U(C::Acc()), 48);
    {
        float t = EaseInOut(g_themeT);
        ImU32 ringCol = IM_COL32(
            int(Lerpf(85,  255, t)),
            int(Lerpf(70,  255, t)),
            int(Lerpf(200, 255, t)),
            int(Lerpf(130,  60, t))
        );
        dl->AddCircle({cx2, cy2}, cBtnR + 2.5f, ringCol, 48, 1.5f);
    }

    float chs = 10.f, thk = 3.f;
    dl->AddLine({cx2 - chs * 0.45f, cy2 - chs * 0.8f}, {cx2 + chs * 0.55f, cy2}, IM_COL32(255, 255, 255, 250), thk);
    dl->AddLine({cx2 + chs * 0.55f, cy2}, {cx2 - chs * 0.45f, cy2 + chs * 0.8f}, IM_COL32(255, 255, 255, 250), thk);

    if (clicked && !(g_pop.visible && !g_pop.closing)) {
        PopoverOpen(lbl, secId);
        g_popOpenClickPos = ImGui::GetIO().MousePos;
        PlaySound(SND_CLICK);
    }
    return clicked;
}

static bool TickSlideAnim(float& anim, float& vel, bool closing, float dt) {
    if (dt > 0.032f) dt = 0.032f;
    const float stiff = 120.f, damp = 22.f;
    if (!closing) {
        float acc = (1.f - anim) * stiff - vel * damp;
        vel  += acc * dt;
        anim += vel * dt;
        if (anim >= 0.9998f) { anim = 1.f; vel = 0.f; }
        anim = ImClamp(anim, 0.f, 1.f);
        return true;
    } else {
        float acc = (0.f - anim) * stiff - vel * damp;
        vel  += acc * dt;
        anim += vel * dt;
        if (anim <= 0.001f) { anim = 0.f; vel = 0.f; return false; }
        anim = ImClamp(anim, 0.f, 1.f);
        return true;
    }
}

static void DrawExitButtons(ImDrawList* fg, ImFont* fn, float fs, float alpha,
                             float b1X, float b2X, float bW, float bY, float btnH,
                             bool blocked,
                             const ImVec2& mousePos, const ImVec2& clickedPos,
                             bool mouseReleased,
                             bool isSheet) {
    fg->AddRectFilled({b1X, bY}, {b1X + bW, bY + btnH}, C::UA(C::Txt(), 0.08f * alpha), R::Btn);
    fg->AddRectFilled({b2X, bY}, {b2X + bW, bY + btnH}, C::UA(C::Red(), alpha), R::Btn);
    if (g_state.ui_show_sep) {
        fg->AddRect({b1X, bY}, {b1X + bW, bY + btnH}, C::UA(C::Txt(), 0.22f * alpha), R::Btn, 0, 1.2f);
        fg->AddRect({b2X, bY}, {b2X + bW, bY + btnH}, C::UA(C::Txt(), 0.22f * alpha), R::Btn, 0, 1.2f);
    }
    auto c1Sz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Отмена"));
    auto c2Sz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Выйти"));
    fg->AddText(fn, fs * 1.15f, {b1X + bW * 0.5f - c1Sz.x * 0.5f, bY + (btnH - fs * 1.15f) * 0.5f},
        C::UA(C::Txt(), alpha), XS("Отмена"));
    fg->AddText(fn, fs * 1.15f, {b2X + bW * 0.5f - c2Sz.x * 0.5f, bY + (btnH - fs * 1.15f) * 0.5f},
        IM_COL32(255, 255, 255, int(255 * alpha)), XS("Выйти"));
    if (!blocked && mouseReleased) {
        bool dH1 = clickedPos.x >= b1X && clickedPos.x <= b1X+bW && clickedPos.y >= bY && clickedPos.y <= bY+btnH;
        bool dH2 = clickedPos.x >= b2X && clickedPos.x <= b2X+bW && clickedPos.y >= bY && clickedPos.y <= bY+btnH;
        bool uH1 = mousePos.x >= b1X && mousePos.x <= b1X+bW && mousePos.y >= bY && mousePos.y <= bY+btnH;
        bool uH2 = mousePos.x >= b2X && mousePos.x <= b2X+bW && mousePos.y >= bY && mousePos.y <= bY+btnH;
        if (uH1 && dH1) { if (isSheet) SheetClose(); else PopoverClose(); }
        if (uH2 && dH2) main_thread_flag = false;
    }
}

void DrawSheet(float dt, ImVec2 menuPos, float WW, float WH) {
    if (!g_sheet.visible) return;

    auto* fg = ImGui::GetForegroundDrawList();
    auto& io = ImGui::GetIO();
    auto* fn = ImGui::GetFont();
    float fs = ImGui::GetFontSize();

    g_sheet.openFrames++;

    if (!TickSlideAnim(g_sheet.anim, g_sheet.vel, g_sheet.closing, dt)) {
        g_sheet.anim = 0.f; g_sheet.vel = 0.f;
        g_sheet.visible = false; g_sheet.closing = false;
        return;
    }

    float easeT  = EaseInOut(g_sheet.anim);
    float sheetH = (g_sheet.type == 2) ? 280.f : 380.f;
    float sheetW = WW - 24.f;
    float sheetX = menuPos.x + (WW - sheetW) * 0.5f;
    float sheetYFull   = (g_sheet.type == 2)
        ? menuPos.y + (WH - sheetH) * 0.5f
        : menuPos.y + WH - sheetH - 16.f;
    float sheetYHidden = menuPos.y + WH;
    float sheetY = Lerpf(sheetYHidden, sheetYFull, easeT);

    {
        const float fgR = R::Sheet;
        int ba = int(Lerpf(0.f, 180.f, easeT));
        fg->AddRectFilled(menuPos, {menuPos.x + WW, menuPos.y + WH}, C::UA(C::Bg(), ba / 255.f), fgR);
        fg->AddRectFilled({menuPos.x + 2, menuPos.y + 2}, {menuPos.x + WW - 2, menuPos.y + WH - 2}, C::UA(C::Bg(), ba * 0.6f / 255.f), fgR - 1);
        fg->AddRectFilled({menuPos.x + 4, menuPos.y + 4}, {menuPos.x + WW - 4, menuPos.y + WH - 4}, C::UA(C::Bg(), ba * 0.4f / 255.f), fgR - 2);
        fg->AddRectFilled(menuPos, {menuPos.x + WW, menuPos.y + WH}, IM_COL32(20, 20, 40, int(Lerpf(0.f, 60.f, easeT))), fgR);
        fg->AddRect(menuPos, {menuPos.x + WW, menuPos.y + WH}, IM_COL32(255, 255, 255, int(Lerpf(0.f, 40.f, easeT))), fgR, 0, 1.5f);
    }

    fg->PushClipRect({menuPos.x - 40.f, menuPos.y + 1.f}, {menuPos.x + WW + 40.f, menuPos.y + WH - 1.f}, true);

    fg->AddRectFilled({sheetX, sheetY}, {sheetX + sheetW, sheetY + sheetH}, C::U(C::Card()), R::Sheet);

    {
        float dhW = 60.f, dhH = 6.f;
        float dhX = sheetX + sheetW * 0.5f - dhW * 0.5f, dhY = sheetY + 12.f;
        fg->AddRectFilled({dhX, dhY}, {dhX + dhW, dhY + dhH}, C::UA(C::Dim(), 0.4f * easeT), dhH * 0.5f);
        bool inHandle = io.MousePos.x >= sheetX && io.MousePos.x <= sheetX + sheetW
                     && io.MousePos.y >= sheetY  && io.MousePos.y <= sheetY + 44.f;
        if (g_sheet.type != 2) {
            if (!g_sheet.closing && inHandle && io.MouseDown[0] && io.MouseDelta.y > 3.f)
                SheetClose();
        } else {
            if (io.MouseClicked[0] && inHandle) {
                g_win.dragging   = true;
                g_win.touchStart = io.MousePos;
                g_win.posStart   = g_win.pos;
            }
        }
    }

    float titleFS = fs * 1.45f;
    {
        auto tsz   = fn->CalcTextSizeA(titleFS, FLT_MAX, 0, g_sheet.title);
        fg->AddText(fn, titleFS, {sheetX + sheetW * 0.5f - tsz.x * 0.5f, sheetY + 32.f}, C::U(C::Txt()), g_sheet.title);
    }

    if (g_sheet.type != 2) {
        float cR  = 17.f, cx2 = sheetX + sheetW - cR - 16.f, cy2 = sheetY + 32.f + titleFS * 0.5f;
        fg->AddCircleFilled({cx2, cy2}, cR, C::U(C::Sep()), 32);
        float cs = 6.f;
        fg->AddLine({cx2 - cs, cy2 - cs}, {cx2 + cs, cy2 + cs}, C::U(C::Dim()), 2.4f);
        fg->AddLine({cx2 + cs, cy2 - cs}, {cx2 - cs, cy2 + cs}, C::U(C::Dim()), 2.4f);
        if (!g_sheet.closing && io.MouseClicked[0] &&
            fabsf(io.MousePos.x - cx2) < cR + 8.f && fabsf(io.MousePos.y - cy2) < cR + 8.f)
            SheetClose();
    }

    {
        float lineY = sheetY + 32.f + titleFS + 14.f;
        fg->AddLine({sheetX + 18.f, lineY}, {sheetX + sheetW - 18.f, lineY}, C::U(C::Sep()), 0.8f);
    }

    float cY = sheetY + 32.f + titleFS + 26.f;
    float padX = 28.f;

    if (g_sheet.type == 0 && g_sheet.boolP) {
        bool*  v  = g_sheet.boolP;
        float* a  = g_sheet.animP;
        float  t  = EaseInOut(*a);
        float  tW = 72.f, tH = 44.f;
        float  tx = sheetX + sheetW * 0.5f - tW * 0.5f, tcy = cY + 20.f + tH * 0.5f;
        DrawToggle(fg, tx, tcy, t);
        if (!g_sheet.closing && io.MouseClicked[0]) {
            if (io.MousePos.x >= tx - 12 && io.MousePos.x <= tx + tW + 12
             && io.MousePos.y >= cY + 8.f && io.MousePos.y <= cY + tH + 32.f)
                { *v=!*v; char _b[160]; snprintf(_b,sizeof(_b),XS("%s|%s"),g_sheet.title,*v?XS("ON"):XS("OFF")); ShowToast(_b); }
        }
        const char* st = *v ? XS("Включено") : XS("Выключено");
        auto stSz = fn->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0, st);
        fg->AddText(fn, fs * 1.1f, {sheetX + sheetW * 0.5f - stSz.x * 0.5f, cY + tH + 26.f},
            *v ? C::U(C::Acc()) : C::U(C::Dim()), st);
    } else if (g_sheet.type == 1 && g_sheet.slP) {
        float* v = g_sheet.slP; float mn = g_sheet.slMin, mx = g_sheet.slMax;
        char vb[24]; snprintf(vb, 24, g_sheet.slFmt, *v);
        auto vSz = fn->CalcTextSizeA(fs * 2.0f, FLT_MAX, 0, vb);
        fg->AddText(fn, fs * 2.0f, {sheetX + sheetW * 0.5f - vSz.x * 0.5f, cY + 10.f}, C::U(C::Acc()), vb);
        float trkY = cY + 78.f, trkX = sheetX + padX + 16.f;
        float trkW = sheetW - padX * 2.f - 32.f, kR2 = 20.f, trkH = 8.f;
        float frac = Clamp01((*v - mn) / (mx - mn)), kx = trkX + frac * trkW;
        fg->AddRectFilled({trkX, trkY - trkH * 0.5f}, {trkX + trkW, trkY + trkH * 0.5f}, C::U(C::TrkOff()), trkH);
        fg->AddRectFilled({trkX, trkY - trkH * 0.5f}, {kx,           trkY + trkH * 0.5f}, C::U(C::Acc()), trkH);
        static bool s_sheetSliderActive = false;
        if (!g_sheet.closing && io.MouseClicked[0] && fabsf(io.MousePos.y - trkY) < 56.f
            && io.MousePos.x >= trkX - 24.f && io.MousePos.x <= trkX + trkW + 24.f)
            s_sheetSliderActive = true;
        if (!io.MouseDown[0]) s_sheetSliderActive = false;
        bool near = s_sheetSliderActive && !g_sheet.closing && io.MouseDown[0];
        fg->AddCircleFilled({kx + 0.7f, trkY + 1.5f}, kR2, IM_COL32(0, 0, 0, 40));
        fg->AddCircleFilled({kx, trkY}, kR2, IM_COL32(255, 255, 255, 255));
        if (near) fg->AddCircle({kx, trkY}, kR2 + 4.f, C::UA(C::Light::Acc, 0.85f), 48, 3.f);
        if (near) { float dx = io.MousePos.x - trkX; *v = mn + Clamp01(dx / trkW) * (mx - mn); }
        char minB[16], maxB[16];
        snprintf(minB, 16, g_sheet.slFmt, mn); snprintf(maxB, 16, g_sheet.slFmt, mx);
        fg->AddText(fn, fs * 0.85f, {trkX, trkY + kR2 + 8.f}, C::U(C::Dim()), minB);
        auto mxSz = fn->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, maxB);
        fg->AddText(fn, fs * 0.85f, {trkX + trkW - mxSz.x, trkY + kR2 + 8.f}, C::U(C::Dim()), maxB);
    } else if (g_sheet.type == 2) {
        auto dSz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Приложение будет закрыто."));
        fg->AddText(fn, fs * 1.15f, {sheetX + sheetW * 0.5f - dSz.x * 0.5f, cY + 16.f},
            C::U(C::Dim()), XS("Приложение будет закрыто."));
    }

    {
        float btnH = 62.f, btnY = sheetY + sheetH - btnH - 24.f;
        if (g_sheet.type == 2) {
            float bW  = (sheetW - padX * 2.f - 12.f) * 0.5f;
            float b1X = sheetX + padX, b2X = b1X + bW + 12.f;
            DrawExitButtons(fg, fn, fs, 1.f,
                b1X, b2X, bW, btnY, btnH,
                g_sheet.closing,
                io.MousePos, io.MouseClickedPos[0],
                io.MouseReleased[0], true);
        } else {
            float bW = sheetW - padX * 2.f, bX = sheetX + padX;
            fg->AddRectFilled({bX, btnY}, {bX + bW, btnY + btnH}, C::U(C::Acc()), R::Btn);
            auto bSz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Готово"));
            fg->AddText(fn, fs * 1.15f, {bX + bW * 0.5f - bSz.x * 0.5f, btnY + (btnH - fs * 1.15f) * 0.5f},
                IM_COL32(255, 255, 255, 255), XS("Готово"));
            if (!g_sheet.closing && io.MouseClicked[0]
                && io.MousePos.x >= bX && io.MousePos.x <= bX + bW
                && io.MousePos.y >= btnY && io.MousePos.y <= btnY + btnH)
                SheetClose();
        }
    }
    fg->PopClipRect();
}

static float DrawPopoverContentFG(ImDrawList* fg, ImFont* fn, float fs, int secId, float dt,
                                   float cX, float cW, float curY, float alpha,
                                   bool blocked, const ImVec2& mousePos, const ImVec2& clickedPos,
                                   bool mouseReleased, bool mouseDown)
{
    const float inset = Layout::Inset, padX = Layout::PadX;
    const float rH = Layout::RowH;

    auto FgSHdr = [&](const char* t, float topPad = 18.f) {
        curY += topPad;
        if (t && t[0]) {
            auto tsz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, t);
            fg->AddText(fn, fs * 1.15f, {cX + 32.f, curY}, C::UA(C::Dim(), alpha), t);
            curY += tsz.y + 8.f;
        } else {
            curY += 8.f;
        }
    };

    auto FgCardBg = [&](float h, ImDrawFlags flags = ImDrawFlags_RoundCornersAll) {
        fg->AddRectFilled({cX + inset, curY}, {cX + cW - inset, curY + h}, C::UA(C::Card(), alpha), R::Card, flags);
        if (g_state.ui_show_sep)
            fg->AddRect({cX + inset, curY}, {cX + cW - inset, curY + h}, C::UA(C::Sep(), alpha), R::Card, flags, 1.2f);
    };

    auto FgToggleRow = [&](const char* lbl, bool* v, float& anim, bool last) {
        Tick(anim, *v, dt, 12.f);
        RenderToggleRowVisuals(fg, fn, fs, cX, curY, cW, lbl, anim, alpha,
                               g_state.ui_show_sep, last);
        if (!blocked && !g_scrollPop.dragging && mouseReleased
            && mousePos.x >= cX + inset && mousePos.x <= cX + cW - inset
            && mousePos.y >= curY && mousePos.y <= curY + rH
            && clickedPos.x >= cX + inset && clickedPos.x <= cX + cW - inset
            && clickedPos.y >= curY && clickedPos.y <= curY + rH) {
            *v = !*v;
            char _b[160]; snprintf(_b, sizeof(_b), XS("%s|%s"), lbl, *v ? XS("ON") : XS("OFF")); ShowToast(_b);
            PlaySound(SND_CLICK);
        }
        curY += rH;
    };

    auto FgRadioRow = [&](int idx, int* cur, AppState::RadioAnim& ra, float* fillAnim,
                           const char* lbl, bool last) {
        bool isSelected = (*cur == idx);
        Tick(*fillAnim, isSelected, dt, 10.f);
        float fill = EaseInOut(*fillAnim);

        if (!blocked && mouseReleased
            && mousePos.x >= cX + inset && mousePos.x <= cX + cW - inset
            && mousePos.y >= curY && mousePos.y <= curY + rH
            && clickedPos.y >= curY && clickedPos.y <= curY + rH
            && !isSelected) {
            ra.scaleVel = -8.f;
            ra.ring = 0.f;
            ra.ringVel = 5.5f;
            *cur = idx;
            PlaySound(SND_CLICK);
        }
        SpringTick(ra.scale, ra.scaleVel, 1.0f, dt);
        ra.scale = ImMax(0.6f, ra.scale);
        SpringTick(ra.ring, ra.ringVel, 3.5f, dt);

        const float rR = 22.f;
        float textX = cX + inset + padX, textY = curY + (rH - fs * 1.15f) * 0.5f;
        float textMaxX = cX + cW - inset - rR * 2.f - padX - 16.f;
        fg->PushClipRect({textX, curY}, {textMaxX, curY + rH}, true);
        fg->AddText(fn, fs * 1.15f, {textX, textY}, C::UA(C::Txt(), alpha), lbl);
        fg->PopClipRect();

        float cx2 = cX + cW - inset - rR - padX, cy2 = curY + rH * 0.5f;
        float r2 = rR * ra.scale;

        if (ra.ring > 0.05f && ra.ring < 3.4f) {
            float ringR  = rR + ra.ring * 10.f;
            float ringA  = (1.f - ra.ring / 3.5f) * 0.55f * alpha;
            fg->AddCircle({cx2, cy2}, ringR, C::UA(C::Acc(), ringA), 48, 2.5f);
        }

        ImU32 radioCol = IM_COL32(
            int(Lerpf(C::TrkOff().x, C::Acc().x, fill)*255),
            int(Lerpf(C::TrkOff().y, C::Acc().y, fill)*255),
            int(Lerpf(C::TrkOff().z, C::Acc().z, fill)*255),
            int(alpha*255));
        fg->AddCircleFilled({cx2, cy2}, r2, radioCol, 40);

        if (fill > 0.01f)
            fg->AddCircleFilled({cx2, cy2}, r2 * 0.46f * fill,
                IM_COL32(255,255,255,int(255*fill*alpha)), 32);

        if (fill < 0.99f)
            fg->AddCircle({cx2, cy2}, r2, C::UA(C::Sep(), (1.f-fill)*alpha), 40, 1.8f);

        if (!last && g_state.ui_show_sep)
            fg->AddLine({cX + inset + padX, curY + rH - 0.5f},
                        {cX + cW - inset - padX, curY + rH - 0.5f}, C::UA(C::Sep(), alpha), 0.8f);
        curY += rH;
    };

    auto FgSliderRow = [&](const char* lbl, float* v, float mn, float mx, const char* fmt,
                            bool last, AppState::SliderAnim& anim) {
        const float rowH = Layout::SliderH;
        float cX2 = cX + inset, cW2 = cW - inset * 2.f;

        bool act = !blocked && mouseDown && !g_scrollPop.dragging
            && clickedPos.x >= cX2 && clickedPos.x <= cX2 + cW2
            && clickedPos.y >= curY && clickedPos.y <= curY + rowH;

        if (act)
            *v = mn + Clamp01((mousePos.x - (cX2 + padX + 22.f)) / (cW2 - padX * 2.f - 44.f)) * (mx - mn);

        float target = Clamp01((*v - mn) / (mx - mn));
        TickSliderAnim(anim, target, act, dt);

        RenderSliderVisuals(fg, fn, fs, cX, curY, cW, rowH,
                            lbl, fmt, *v, anim.pos, act, alpha,
                            g_state.ui_show_sep, last);
        curY += rowH;
    };

    if (secId == 0) {
        FgSHdr(XS("Куда целиться"));
        FgCardBg(rH * 3);
        FgRadioRow(0, &g_state.aim_bone, g_state.ra_aim_head,   &g_state.a_aim_head,   XS("Голова"), false);
        FgRadioRow(1, &g_state.aim_bone, g_state.ra_aim_chest,  &g_state.a_aim_chest,  XS("Шея"), false);
        FgRadioRow(2, &g_state.aim_bone, g_state.ra_aim_pelvis, &g_state.a_aim_pelvis, XS("Тело"), true);

        FgSHdr(XS("Выбор цели"));
        FgCardBg(rH * 3);
        FgRadioRow(0, &g_state.aim_priority, g_state.ra_aim_pr0, &g_state.a_aim_pr0, XS("Умный"), false);
        FgRadioRow(1, &g_state.aim_priority, g_state.ra_aim_pr1, &g_state.a_aim_pr1, XS("Ближе к прицелу"), false);
        FgRadioRow(2, &g_state.aim_priority, g_state.ra_aim_pr2, &g_state.a_aim_pr2, XS("Ближе ко мне"), true);

    } else if (secId == 1) {
        FgSHdr(XS("Линии и боксы"));
        FgCardBg(Layout::SliderH);
        FgSliderRow(XS("Толщина"),   &g_state.esp_thick, 0.5f, 5.f,   "%.1f",     true, g_state.sl_esp_thick);

    } else if (secId == 2) {
        FgSHdr(nullptr, 12.f);
        auto dSz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Приложение будет закрыто."));
        fg->AddText(fn, fs * 1.15f,
            {cX + (cW - dSz.x) * 0.5f, curY},
            C::UA(C::Dim(), alpha), XS("Приложение будет закрыто."));
        curY += dSz.y + 28.f;

        const float btnH = 62.f, bPad = 28.f;
        float bW  = (cW - inset * 2.f - bPad * 2.f - 12.f) * 0.5f;
        float b1X = cX + inset + bPad, b2X = b1X + bW + 12.f;
        DrawExitButtons(fg, fn, fs, alpha,
            b1X, b2X, bW, curY, btnH,
            blocked, mousePos, clickedPos,
            mouseReleased, false);
        curY += btnH;
    } else if (secId == 3) {
        const char* cfgName = (g_configToDelete >= 0 && g_configToDelete < g_configCount)
            ? g_configs[g_configToDelete].name : XS("Конфиг");

        FgSHdr(nullptr, 40.f);

        {
            char nameBuf[96];
            snprintf(nameBuf, sizeof(nameBuf), XS("«%s»"), cfgName);
            float nameFS = fs * 1.55f;
            auto  nameSz = fn->CalcTextSizeA(nameFS, FLT_MAX, 0, nameBuf);
            fg->AddText(fn, nameFS,
                {cX + (cW - nameSz.x) * 0.5f, curY},
                C::UA(C::Txt(), alpha), nameBuf);
            curY += nameSz.y + 12.f;
        }

        {
            const char* sub = XS("Будет удалён безвозвратно");
            auto sSz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, sub);
            fg->AddText(fn, fs * 1.05f,
                {cX + (cW - sSz.x) * 0.5f, curY},
                C::UA(C::Red(), alpha * 0.85f), sub);
            curY += sSz.y + 36.f;
        }

        {
            const float btnH = 58.f, bPad = 28.f, gap = 12.f;
            float bW  = (cW - inset * 2.f - bPad * 2.f - gap) * 0.5f;
            float b1X = cX + inset + bPad;
            float b2X = b1X + bW + gap;

            fg->AddRectFilled({b1X, curY}, {b1X + bW, curY + btnH}, C::UA(C::Card(), alpha), R::Btn);
            fg->AddRect({b1X, curY}, {b1X + bW, curY + btnH}, C::UA(C::Sep(), alpha), R::Btn, 0, 1.5f);
            {
                const char* ct = XS("Отмена");
                auto ctsz = fn->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0, ct);
                fg->AddText(fn, fs * 1.1f,
                    {b1X + bW * 0.5f - ctsz.x * 0.5f, curY + (btnH - fs * 1.1f) * 0.5f},
                    C::UA(C::Txt(), alpha), ct);
            }

            fg->AddRectFilled({b2X, curY}, {b2X + bW, curY + btnH}, C::UA(C::Red(), alpha * 0.9f), R::Btn);
            fg->AddRect({b2X, curY}, {b2X + bW, curY + btnH}, C::UA(C::Red(), alpha), R::Btn, 0, 1.5f);
            {
                const char* dt2 = XS("Удалить");
                auto dtsz = fn->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0, dt2);
                fg->AddText(fn, fs * 1.1f,
                    {b2X + bW * 0.5f - dtsz.x * 0.5f, curY + (btnH - fs * 1.1f) * 0.5f},
                    IM_COL32(255, 255, 255, int(255 * alpha)), dt2);
            }

            if (!blocked && mouseReleased) {
                if (mousePos.x >= b1X && mousePos.x <= b1X + bW
                 && mousePos.y >= curY && mousePos.y <= curY + btnH
                 && clickedPos.x >= b1X && clickedPos.x <= b1X + bW
                 && clickedPos.y >= curY && clickedPos.y <= curY + btnH)
                    PopoverClose();

                if (mousePos.x >= b2X && mousePos.x <= b2X + bW
                 && mousePos.y >= curY && mousePos.y <= curY + btnH
                 && clickedPos.x >= b2X && clickedPos.x <= b2X + bW
                 && clickedPos.y >= curY && clickedPos.y <= curY + btnH) {
                    ConfigDelete(g_configToDelete);
                    g_configToDelete = -1;
                    PopoverClose();
                }
            }
            curY += btnH;
        }
    }

    curY += 12.f;
    return curY;
}

void DrawPopover(float dt, ImVec2 menuPos, float WW, float WH) {
    if (!g_pop.visible) return;

    g_pop.openFrames++;

    if (!TickSlideAnim(g_pop.anim, g_pop.vel, g_pop.closing, dt)) {
        g_pop.anim = 0.f; g_pop.vel = 0.f;
        g_pop.visible = false; g_pop.closing = false;
        g_popOpenClickPos = {-9999.f, -9999.f};
        g_scrollPop = {};
        Blur::Unfreeze();
        return;
    }

    auto& io    = ImGui::GetIO();
    float easeT = EaseInOut(g_pop.anim);
    auto* fn    = ImGui::GetFont();
    float fs    = ImGui::GetFontSize();

    float titleFS  = fs * 1.45f;
    float headerH  = 28.f + titleFS + 16.f + 8.f;

    float popW = WW * 0.92f, popH = WH * 0.88f;

    float sX     = menuPos.x + (WW - popW) * 0.5f;
    float sYFull = menuPos.y + (WH - popH) * 0.5f;
    float sY     = Lerpf(menuPos.y + WH, sYFull, easeT);
    float sW = popW, sH = popH;

    float contentY = sY + headerH;
    float areaH    = sH - headerH;
    float lineY    = sY + 28.f + titleFS + 16.f;

    auto* fg = ImGui::GetForegroundDrawList();

    Blur::Draw(fg, g_win.pos, {g_win.pos.x+g_win.w, g_win.pos.y+g_win.h}, easeT, R::Card);

    fg->PushClipRect({menuPos.x, menuPos.y}, {menuPos.x + WW, menuPos.y + WH}, true);

    fg->AddRectFilled({sX, sY}, {sX + sW, sY + sH}, C::U(C::Bg()), R::Sheet);

    {
        float dhW = 72.f, dhH = 7.f;
        float dhX = sX + sW * 0.5f - dhW * 0.5f, dhY = sY + 14.f;
        fg->AddRectFilled({dhX, dhY}, {dhX + dhW, dhY + dhH},
            C::UA(C::Dim(), 0.45f * easeT), dhH * 0.5f);
        if (io.MouseClicked[0]
            && io.MousePos.x >= sX && io.MousePos.x <= sX + sW
            && io.MousePos.y >= sY && io.MousePos.y <= sY + 44.f) {
            g_win.dragging   = true;
            g_win.touchStart = io.MousePos;
            g_win.posStart   = g_win.pos;
        }
    }

    bool inPop = io.MousePos.x >= sX && io.MousePos.x <= sX + sW
              && io.MousePos.y >= contentY && io.MousePos.y <= sY + sH - 4.f;
    bool clickInPop = io.MouseClickedPos[0].x >= sX && io.MouseClickedPos[0].x <= sX + sW
                   && io.MouseClickedPos[0].y >= contentY && io.MouseClickedPos[0].y <= sY + sH - 4.f;
    bool mIn = inPop && clickInPop;

    static float s_popMaxScroll = 0.f;
    bool colorPop = (g_pop.sectionId == 4);
    if (!colorPop) ScrollTick(g_scrollPop, mIn, g_pop.closing, s_popMaxScroll, dt);
    else { g_scrollPop.off = 0.f; g_scrollPop.vel = 0.f; s_popMaxScroll = 0.f; }

    if (colorPop && g_colP) {

        const float PAD   = 16.f;
        const float btnH  = 62.f, btnMar = 10.f;
        float X  = sX + PAD, W = sW - PAD * 2.f;
        float bY = sY + sH - btnH - btnMar;

        const int   COLS     = 7;
        const float CELL_GAP = 10.f;
        int   ROWS   = (kPaletteCount + COLS - 1) / COLS;
        float availH = bY - contentY - 16.f;
        float cellW = (W - CELL_GAP * (COLS - 1)) / (float)COLS;
        float cellH = (availH - CELL_GAP * (ROWS - 1)) / (float)ROWS;
        float cellSz = cellW < cellH ? cellW : cellH;
        if (cellSz < 22.f) cellSz = 22.f;
        float gridW  = COLS * cellSz + (COLS - 1) * CELL_GAP;
        float gridH  = ROWS * cellSz + (ROWS - 1) * CELL_GAP;
        float gridX  = X + (W - gridW) * 0.5f;
        float gridY  = contentY + (bY - contentY - gridH) * 0.5f;
        if (gridY + gridH > bY - 8.f) gridY = bY - 8.f - gridH;
        if (gridY < contentY + 8.f) gridY = contentY + 8.f;
        fg->PushClipRect({sX + 8.f, contentY}, {sX + sW - 8.f, bY - 6.f}, true);

        const float CELL_R = 10.f;

        static int   s_palSel  = -1;
        static float s_selOffX = 0.f, s_selOffY = 0.f;
        static float s_selVelX = 0.f, s_selVelY = 0.f;
        static float s_selSz   = 0.f, s_selVelSz = 0.f;
        if (g_colP) {
            float bestDist = 1e9f;
            int   bestIdx  = -1;
            for (int pi = 0; pi < kPaletteCount; pi++) {
                float dr = kColorPalette[pi].x - g_colP->x;
                float dg = kColorPalette[pi].y - g_colP->y;
                float db = kColorPalette[pi].z - g_colP->z;
                float d  = dr*dr + dg*dg + db*db;
                if (d < bestDist) { bestDist = d; bestIdx = pi; }
            }
            if (bestDist < 0.02f) s_palSel = bestIdx; else s_palSel = -1;
        }

        auto cellCenter = [&](int pi) -> ImVec2 {
            int r = pi / COLS, c = pi % COLS;
            return { c * (cellSz + CELL_GAP) + cellSz * 0.5f,
                     r * (cellSz + CELL_GAP) + cellSz * 0.5f };
        };

        if (s_palSel >= 0) {
            ImVec2 tc = cellCenter(s_palSel);
            if (s_selSz < 0.001f || g_palSelReset) {
                s_selOffX = tc.x; s_selOffY = tc.y;
                s_selVelX = 0.f;  s_selVelY = 0.f;
                if (g_palSelReset) { s_selSz = 1.f; s_selVelSz = 0.f; }
            }
            SpringTick(s_selOffX, s_selVelX, tc.x, dt);
            SpringTick(s_selOffY, s_selVelY, tc.y, dt);
            SpringTick(s_selSz, s_selVelSz, 1.f, dt);
        } else {
            SpringTick(s_selSz, s_selVelSz, 0.f, dt);
        }
        g_palSelReset = false;

        for (int pi = 0; pi < kPaletteCount; pi++) {
            int row = pi / COLS, col = pi % COLS;
            float cx0 = gridX + col * (cellSz + CELL_GAP);
            float cy0 = gridY + row * (cellSz + CELL_GAP);
            float cx1 = cx0 + cellSz, cy1 = cy0 + cellSz;

            ImVec4 cv = kColorPalette[pi];
            ImU32  fill = IM_COL32(int(cv.x*255), int(cv.y*255), int(cv.z*255), 255);

            fg->AddRectFilled({cx0+1.5f, cy0+1.5f}, {cx1+1.5f, cy1+1.5f},
                IM_COL32(0,0,0, int(18*easeT)), CELL_R);
            fg->AddRectFilled({cx0, cy0}, {cx1, cy1}, fill, CELL_R);

            if (!g_pop.closing && io.MouseReleased[0]) {
                ImVec2 mp = io.MousePos, cp = io.MouseClickedPos[0];
                bool hit = mp.x >= cx0 && mp.x <= cx1 && mp.y >= cy0 && mp.y <= cy1
                        && cp.x >= cx0 && cp.x <= cx1 && cp.y >= cy0 && cp.y <= cy1;
                if (hit) {
                    *g_colP = kColorPalette[pi];
                    s_palSel = pi;
                    g_input.touchConsumed = true;
                }
            }
        }

        if (s_selSz > 0.001f) {
            float ax = gridX + s_selOffX, ay = gridY + s_selOffY;
            float r = (cellSz * 0.5f + 5.f) * s_selSz;
            fg->AddRect({ax - r, ay - r}, {ax + r, ay + r},
                IM_COL32(0,0,0, int(220*easeT*s_selSz)), CELL_R + 5.f * s_selSz, 0, 4.f);
            fg->AddRect({ax - r + 2.5f, ay - r + 2.5f}, {ax + r - 2.5f, ay + r - 2.5f},
                IM_COL32(255,255,255, int(255*easeT*s_selSz)), CELL_R + 2.5f * s_selSz, 0, 1.5f);
        }
        fg->PopClipRect();

        fg->AddRectFilled({X, bY}, {X+W, bY+btnH}, C::UA(C::Acc(), easeT), R::Btn);
        auto bsz = fn->CalcTextSizeA(fs*1.2f, FLT_MAX, 0, XS("Готово"));
        fg->AddText(fn, fs*1.2f,
            {X + W*0.5f - bsz.x*0.5f, bY + (btnH-bsz.y)*0.5f},
            IM_COL32(255,255,255,int(255*easeT)), XS("Готово"));

        if (io.MouseDown[0] || io.MouseReleased[0])
            g_input.touchConsumed = true;

        if (!g_pop.closing && io.MouseReleased[0]) {
            ImVec2 mp = io.MousePos, cp = io.MouseClickedPos[0];
            bool doneTap = mp.x >= X && mp.x <= X+W && mp.y >= bY && mp.y <= bY+btnH
                        && cp.x >= X && cp.x <= X+W && cp.y >= bY && cp.y <= bY+btnH;
            if (doneTap) PopoverClose();
        }

    } else {
        fg->PushClipRect({sX, contentY}, {sX + sW, sY + sH - 4.f}, true);

        float drawY = contentY - g_scrollPop.off;
        float endY  = DrawPopoverContentFG(fg, fn, fs, g_pop.sectionId, dt,
                                            sX, sW, drawY, easeT,
                                            g_pop.closing,
                                            io.MousePos, io.MouseClickedPos[0],
                                            io.MouseReleased[0] && !g_scrollPop.dragging,
                                            io.MouseDown[0]);

        float contentTotalH = endY - drawY;
        s_popMaxScroll = ImMax(0.f, contentTotalH - (areaH - 4.f));

        fg->PopClipRect();
    }

    {
        auto tsz = fn->CalcTextSizeA(titleFS, FLT_MAX, 0, g_pop.title);
        fg->AddText(fn, titleFS,
            {sX + sW * 0.5f - tsz.x * 0.5f, sY + 28.f},
            C::UA(C::Txt(), easeT), g_pop.title);
    }
    {
        float cR = 26.f, bcx = sX + sW - cR - 16.f, bcy = sY + 28.f + titleFS * 0.5f;
        fg->AddCircleFilled({bcx, bcy}, cR, C::UA(C::Sep(), easeT), 32);
        float cs = 9.f;
        fg->AddLine({bcx-cs, bcy-cs}, {bcx+cs, bcy+cs}, C::UA(C::Dim(), easeT), 3.2f);
        fg->AddLine({bcx+cs, bcy-cs}, {bcx-cs, bcy+cs}, C::UA(C::Dim(), easeT), 3.2f);

        if (!g_pop.closing && io.MouseReleased[0]
            && fabsf(io.MousePos.x - bcx) < cR + 12.f
            && fabsf(io.MousePos.y - bcy) < cR + 12.f) {
            PopoverClose();
            g_input.touchConsumed = true;
        }
    }
    fg->AddLine({sX + 18.f, lineY}, {sX + sW - 18.f, lineY}, C::UA(C::Sep(), easeT), 2.5f);

    fg->PopClipRect();
}

float TabContent(int tab, float dt, float cW) {
    float sY = ImGui::GetCursorPosY();

    if (tab == 1) {

        cfg::aim::enabled    = g_state.aim_touch;
        cfg::aim::vis_check  = g_state.aim_pos;
        cfg::aim::draw_fov   = g_state.aim_special;
        cfg::aim::scope_only = g_state.aim_scope_only;
        cfg::aim::fov        = g_state.gun_fov;
        cfg::aim::smoothness = g_state.gun_str;
        cfg::aim::bone       = g_state.aim_bone;
        cfg::aim::trigger_delay  = g_state.gun_trigger_delay;

        SHdr(XS("Аим"));
        CardBg(Layout::RowH * 3);
        ToggleRow("##ta1", XS("Аим"),          &g_state.aim_touch,   g_state.a_aim_touch, false, true);
        ToggleRow("##ta6", XS("Только в прицеле"), &g_state.aim_scope_only, g_state.a_aim_scope, false);
        ToggleRow("##ta3", XS("Круг FOV"),         &g_state.aim_special, g_state.a_aim_spec,  true);

        SHdr(XS("Наводка"));
        CardBg(Layout::SliderH * 2);
        SliderRow("##afov", XS("Радиус"), &g_state.gun_fov, 5.f, 180.f, XS("%.0f°"), false, true, g_state.sl_gun_fov, dt);
        SliderRow("##asmt", XS("Скорость"), &g_state.gun_str, 1.f, 10.f, "%.0f", true, false, g_state.sl_gun_str, dt);

        ImGui::Dummy({1.f, 12.f});

} else if (tab == 2) {
        cfg::esp::box          = g_state.esp_box;
        cfg::esp::name_esp     = g_state.esp_name;
        cfg::esp::distance     = g_state.esp_wall;
        cfg::esp::weapon       = g_state.esp_weapon;
        cfg::esp::tracer       = g_state.esp_tracer;
        cfg::esp::skeleton     = g_state.esp_skeleton;
        cfg::esp::ore          = g_state.esp_ore;
        cfg::esp::animal       = g_state.esp_animal;
        cfg::esp::loot         = g_state.esp_loot;
        cfg::esp::team         = g_state.esp_team;
        cfg::esp::pickup       = g_state.esp_pickup;



        auto* dl  = ImGui::GetWindowDrawList();
        auto* fn  = ImGui::GetFont();
        float avW = ImGui::GetContentRegionAvail().x;
        float fs  = ImGui::GetFontSize();
        const float inset = Layout::Inset, padX = Layout::PadX, rowH = Layout::RowH;
        bool popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

        static const char* s_colorHoldId = nullptr;

        auto EspToggleColorRow = [&](const char* id, const char* lbl, bool* v, float* a, ImVec4* col, bool last) {
            auto pos = ImGui::GetCursorScreenPos();
            float cX = pos.x + inset, cW2 = avW - inset * 2.f;
            float cy = pos.y + rowH * 0.5f;
            auto& io2 = ImGui::GetIO();

            ImGui::InvisibleButton(id, {avW, rowH});

            float dotR = 13.f;
            float dotX = 0.f;
            if (col) {
                auto lblSz = fn->CalcTextSizeA(fs*1.15f, FLT_MAX, 0, lbl);
                dotX = cX + padX + lblSz.x + 14.f + dotR;
            }
            bool openedColor = false;
            if (col && !popBlk && !g_pop.visible) {
                float cx0 = io2.MouseClickedPos[0].x, cy0 = io2.MouseClickedPos[0].y;
                float dx = io2.MousePos.x - cx0, dy = io2.MousePos.y - cy0;
                float moved = dx*dx + dy*dy;
                bool onDot = (cx0 - dotX)*(cx0 - dotX) + (cy0 - cy)*(cy0 - cy) <= (dotR + 18.f)*(dotR + 18.f);
                if (io2.MouseReleased[0] && onDot && PtInClip({dotX, cy}) && moved < 28.f*28.f && s_colorHoldId != id) {
                    s_colorHoldId = id;
                    PopoverOpenColor(lbl, col);
                    PlaySound(SND_CLICK);
                    g_input.touchConsumed = true;
                    openedColor = true;
                }
            }
            if (!io2.MouseDown[0] && s_colorHoldId == id) s_colorHoldId = nullptr;

            bool tapped = !openedColor && WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed;
            if (tapped) { *v = !*v; char b[160]; snprintf(b,sizeof(b),XS("%s|%s"),lbl,*v?XS("ON"):XS("OFF")); ShowToast(b); PlaySound(SND_CLICK); }
            if (col) {
                ImVec4& c = *col;
                dl->AddCircleFilled({dotX, cy}, dotR, IM_COL32((int)(c.x*255),(int)(c.y*255),(int)(c.z*255),255), 32);
                dl->AddCircle({dotX, cy}, dotR + 2.f, IM_COL32(0,0,0,80), 32, 2.f);
                dl->AddCircle({dotX, cy}, dotR + 1.f, IM_COL32(255,255,255,180), 32, 1.2f);
            }
            float textY = cy - fs * 1.15f * 0.5f;
            dl->AddText(fn, fs * 1.15f, {cX + padX, textY}, C::U(C::Txt()), lbl);
            DrawToggle(dl, cX + cW2 - 72.f - padX, cy, EaseInOut(*a));
            if (!last && g_state.ui_show_sep)
                dl->AddLine({cX+padX, pos.y+rowH-0.5f},{cX+cW2-padX, pos.y+rowH-0.5f}, C::UA(C::Sep(),0.35f), 0.8f);
        };

        SHdr(XS("Игроки"));
        {
            struct ERow { const char* id; const char* lbl; bool* v; float* a; ImVec4* col; };
            ERow rows[] = {
                {"##vb",  XS("Боксы"),      &g_state.esp_box,          &g_state.a_esp_box,          &cfg::esp::box_col},
                {"##v3",  XS("3D боксы"),   &g_state.esp_chams,        &g_state.a_esp_chams,        &cfg::esp::box_col_invis},
                {"##vn",  XS("Ники"),       &g_state.esp_name,         &g_state.a_esp_name,         &cfg::esp::name_col},
                {"##vd",  XS("Дистанция"),  &g_state.esp_wall,         &g_state.a_esp_wall,         &cfg::esp::distance_col},
                {"##vw",  XS("Оружие"),     &g_state.esp_weapon,       &g_state.a_esp_weapon,       &cfg::esp::weapon_col},
                {"##vtr", XS("Линии"),      &g_state.esp_tracer,       &g_state.a_esp_tracer,       &cfg::esp::tracer_col},
                {"##vsk", XS("Скелеты"),    &g_state.esp_skeleton,     &g_state.a_esp_skeleton,     &cfg::esp::skeleton_col},
                {"##vtm", XS("Свои"),       &g_state.esp_team,         &g_state.a_esp_team,         &cfg::esp::ally_col},
            };
            constexpr int NP = 8;
            CardBg(rowH * NP);
            for (int i = 0; i < NP; i++)
                EspToggleColorRow(rows[i].id, rows[i].lbl, rows[i].v, rows[i].a, rows[i].col, i == NP-1);
        }

        SHdr(XS("Мир"));
        {
            struct ERow { const char* id; const char* lbl; bool* v; float* a; ImVec4* col; };
            ERow rows[] = {
                // No colour dot: every resource paints itself (stone grey,
                // metal orange, sulfur yellow).
                {"##vor", XS("Руда"),      &g_state.esp_ore,          &g_state.a_esp_ore,          nullptr},
                {"##van", XS("Животные"), &g_state.esp_animal,       &g_state.a_esp_animal,       &cfg::esp::animal_col},
                {"##vlt", XS("Ящики"),    &g_state.esp_loot,         &g_state.a_esp_loot,         &cfg::esp::loot_col},
                {"##vpk", XS("Предметы"), &g_state.esp_pickup,       &g_state.a_esp_pickup,       &cfg::esp::pickup_col},
            };
            constexpr int NW = 4;
            CardBg(rowH * NW);
            for (int i = 0; i < NW; i++)
                EspToggleColorRow(rows[i].id, rows[i].lbl, rows[i].v, rows[i].a, rows[i].col, i == NW-1);
        }

        SHdr(XS("Дальность"));
        CardBg(Layout::SliderH);
        SliderRow("##vmd", XS("Показывать до"), &g_state.marker_dist,
                  25.f, 300.f, XS("%.0f м"), true, true, g_state.sl_marker_dist, dt);

        ImGui::Dummy({1.f, 8.f});
        CollapsibleHeader("##veh1", XS("Ещё настройки"), 1);

        ImGui::Dummy({1.f, 12.f});

    } else if (tab == 4) {
        // Конфиги (saved profiles). Kept at index 4 so the Мемори tab sits
        // directly above it in the tab bar.

        auto* dl  = ImGui::GetWindowDrawList();
        auto* fn  = ImGui::GetFont();
        float avW = ImGui::GetContentRegionAvail().x;
        const float inset = Layout::Inset, padX = Layout::PadX;
        const float fs = ImGui::GetFontSize();

        SHdr(XS("Новый конфиг"));
        {
            const float btnH = 72.f;
            auto pos = ImGui::GetCursorScreenPos();
            float cardX0 = pos.x + inset, cardX1 = pos.x + avW - inset;

            dl->AddRectFilled({cardX0, pos.y}, {cardX1, pos.y + btnH},
                C::U(C::Card()), R::Btn);
            if (g_state.ui_show_sep)
                dl->AddRect({cardX0, pos.y}, {cardX1, pos.y + btnH},
                    C::UA(C::Sep(), 0.9f), R::Btn, 0, 1.2f);

            ImGui::InvisibleButton("##cfgcreate", {avW, btnH});
            auto& io2 = ImGui::GetIO();
            bool popBlocking2 = (g_pop.visible && !g_pop.closing) || g_sheet.visible;
            bool tapped = WasTappedHere() && !popBlocking2 && !IsScrollDragging() && !g_input.touchConsumed;
            if (tapped) ConfigSave();

            float cy2 = pos.y + btnH * 0.5f;
            float icX = cardX0 + 28.f + 20.f;
            dl->AddCircleFilled({icX, cy2}, 20.f, C::UA(C::Acc(), 0.15f), 32);
            dl->AddCircle({icX, cy2}, 20.f, C::UA(C::Acc(), 0.7f), 32, 1.5f);
            dl->AddLine({icX - 9.f, cy2}, {icX + 9.f, cy2}, C::U(C::Acc()), 3.f);
            dl->AddLine({icX, cy2 - 9.f}, {icX, cy2 + 9.f}, C::U(C::Acc()), 3.f);

            const char* createTxt = XS("Создать конфиг");
            float textY = pos.y + (btnH - fs * 1.25f) * 0.5f;
            dl->AddText(fn, fs * 1.25f, {icX + 32.f, textY}, C::U(C::Acc()), createTxt);
        }

        SHdr(XS("Сохранённые"));

        if (g_configCount == 0) {
            const float emptyH = 120.f;
            auto ep = ImGui::GetCursorScreenPos();
            dl->AddRectFilled({ep.x + inset, ep.y}, {ep.x + avW - inset, ep.y + emptyH},
                C::U(C::Card()), R::Card);
            if (g_state.ui_show_sep)
                dl->AddRect({ep.x + inset, ep.y}, {ep.x + avW - inset, ep.y + emptyH},
                    C::UA(C::Sep(), 0.9f), R::Card, 0, 1.2f);
            ImGui::Dummy({avW, emptyH});
            const char* emptyTxt = XS("Нет конфигов");
            auto eSz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, emptyTxt);
            dl->AddText(fn, fs * 1.05f,
                {ep.x + (avW - eSz.x) * 0.5f, ep.y + (emptyH - fs * 1.05f) * 0.5f},
                C::U(C::Dim()), emptyTxt);
        } else {
            auto& io2 = ImGui::GetIO();
            bool popBlocking2 = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

            const float padH    = 12.f;
            const float padTop  = 14.f;
            const float padBot  = 14.f;
            const float icSz    = 62.f;
            const float nameGap = 12.f;
            const float midGap  = 10.f;
            const float btnH2   = 46.f;
            const float btnGapX =  6.f;
            const float cardGap = 10.f;

            float nameFS = fs * 1.15f;
            float subFS  = fs * 0.78f;
            float textH  = fn->CalcTextSizeA(nameFS, FLT_MAX, 0, XS("A")).y;
            float subH   = fn->CalcTextSizeA(subFS,  FLT_MAX, 0, XS("A")).y;
            float topH   = ImMax(icSz, textH + 6.f + subH);
            float cardH  = padTop + topH + midGap + btnH2 + padBot;

            for (int ci = 0; ci < g_configCount; ci++) {
                float loadA = (ci < kMaxConfigs) ? g_cfgLoadAnim[ci] : 0.f;
                float easedA = loadA * loadA * (3.f - 2.f * loadA);

                auto pos = ImGui::GetCursorScreenPos();
                float cx0 = pos.x + inset, cx1 = pos.x + avW - inset;
                float cw  = cx1 - cx0;

                {
                    ImVec4 bg = C::Card(), ac = C::Acc();
                    float mix = (g_darkTheme ? 0.14f : 0.06f) * easedA;
                    ImU32 cardFill = IM_COL32(
                        int(bg.x*255*(1.f-mix) + ac.x*255*mix),
                        int(bg.y*255*(1.f-mix) + ac.y*255*mix),
                        int(bg.z*255*(1.f-mix) + ac.z*255*mix), 255);
                    dl->AddRectFilled({cx0, pos.y}, {cx1, pos.y + cardH}, cardFill, R::Card);

                    float borderA = Lerpf(g_state.ui_show_sep ? 0.9f : 0.f, 0.65f, easedA);
                    ImVec4 borderCol = g_state.ui_show_sep
                        ? ImVec4{Lerpf(C::Sep().x, ac.x, easedA), Lerpf(C::Sep().y, ac.y, easedA), Lerpf(C::Sep().z, ac.z, easedA), borderA}
                        : ImVec4{ac.x, ac.y, ac.z, borderA};
                    if (borderA > 0.01f)
                        dl->AddRect({cx0, pos.y}, {cx1, pos.y + cardH}, C::U(borderCol), R::Card, 0, Lerpf(1.2f, 2.2f, easedA));
                }

                float rowCY = pos.y + padTop + topH * 0.5f;
                float icX  = cx0 + padH;
                float icY  = rowCY - icSz * 0.5f;
                float icCX = icX + icSz * 0.5f;

                if (g_tabIcons[4]) {
                    dl->AddImageRounded((ImTextureID)(intptr_t)g_tabIcons[4],
                        {icX, icY}, {icX + icSz, icY + icSz},
                        {0,0}, {1,1}, IM_COL32(255,255,255,255), 12.f);
                } else {
                    float icR = 14.f;
                    ImU32 icBg = g_darkTheme
                        ? IM_COL32(120, 105, 240, 255)
                        : IM_COL32(108, 92, 231, 255);
                    dl->AddRectFilled({icX, icY}, {icX + icSz, icY + icSz}, icBg, icR);
                    float lh = icSz * 0.20f;
                    float lx = icCX - lh * 0.45f, ly = rowCY + lh * 0.42f;
                    dl->AddLine({icCX - lh, rowCY}, {lx, ly},      IM_COL32(255,255,255,245), 3.2f);
                    dl->AddLine({lx, ly}, {icCX + lh, rowCY - lh}, IM_COL32(255,255,255,245), 3.2f);
                }

                const char* badgeTxt = XS("Загружен");
                auto btsz    = fn->CalcTextSizeA(subFS, FLT_MAX, 0, badgeTxt);
                float dotR   = 3.5f;
                float bPadX  = 8.f, bPadY = 5.f;
                float bW     = dotR*2.f + 5.f + btsz.x + bPadX * 2.f;
                float bH     = btsz.y + bPadY * 2.f;

                float nameAreaR = easedA > 0.01f ? (cx1 - bW * easedA - 10.f) : (cx1 - padH);
                float nameX = icX + icSz + nameGap;
                float nameY = rowCY - (textH + 6.f + subH) * 0.5f;

                dl->PushClipRect({nameX, pos.y}, {nameAreaR, pos.y + cardH}, true);
                dl->AddText(fn, nameFS, {nameX, nameY}, C::U(C::Txt()), g_configs[ci].name);
                dl->AddText(fn, subFS, {nameX, nameY + textH + 6.f}, C::UA(C::Dim(), 0.7f), XS("config"));
                dl->PopClipRect();

                if (easedA > 0.01f) {
                    ImVec4 ac = C::Acc();
                    float bX  = cx1 - bW - 10.f;
                    float bY2 = rowCY - bH * 0.5f;
                    float a   = easedA;

                    dl->AddRectFilled({bX, bY2}, {bX + bW, bY2 + bH},
                        C::UA(ac, (g_darkTheme ? 0.20f : 0.10f) * a), bH * 0.5f);
                    dl->AddRect({bX, bY2}, {bX + bW, bY2 + bH},
                        C::UA(ac, 0.55f * a), bH * 0.5f, 0, 1.3f);
                    dl->AddCircleFilled({bX + bPadX + dotR, bY2 + bH*0.5f}, dotR, C::UA(ac, 0.95f * a), 16);
                    dl->AddText(fn, subFS,
                        {bX + bPadX + dotR*2.f + 5.f, bY2 + bPadY},
                        C::UA(ac, 0.95f * a), badgeTxt);
                }

                float bY  = pos.y + padTop + topH + midGap;
                float totalBtnW = cw - padH * 2.f;
                float bw3 = (totalBtnW - btnGapX * 2.f) / 3.f;
                float b1X = cx0 + padH;
                float b2X = b1X + bw3 + btnGapX;
                float b3X = b2X + bw3 + btnGapX;

                auto DrawBtn = [&](float bx, float bw, ImU32 fillL, ImU32 fillD, ImU32 txtCol, const char* label) {
                    ImU32 fill = g_darkTheme ? fillD : fillL;
                    dl->AddRectFilled({bx, bY}, {bx + bw, bY + btnH2}, fill, R::Btn);
                    auto lsz = fn->CalcTextSizeA(fs * 0.93f, FLT_MAX, 0, label);
                    dl->PushClipRect({bx+3.f, bY}, {bx+bw-3.f, bY+btnH2}, true);
                    dl->AddText(fn, fs * 0.93f,
                        {bx + bw*0.5f - lsz.x*0.5f, bY + (btnH2 - lsz.y)*0.5f},
                        txtCol, label);
                    dl->PopClipRect();
                };

                DrawBtn(b1X, bw3,
                    IM_COL32(238, 235, 255, 255),
                    IM_COL32(58,  48,  140, 255),
                    g_darkTheme ? IM_COL32(170, 158, 255, 255) : IM_COL32(96, 80, 220, 255),
                    XS("Загрузить"));

                DrawBtn(b2X, bw3,
                    IM_COL32(232, 248, 237, 255),
                    IM_COL32(20,  80,  40,  255),
                    g_darkTheme ? IM_COL32(80, 210, 120, 255) : IM_COL32(25, 140, 60, 255),
                    XS("Сохранить"));

                DrawBtn(b3X, bw3,
                    IM_COL32(255, 237, 236, 255),
                    IM_COL32(100, 20,  20,  255),
                    g_darkTheme ? IM_COL32(255, 100, 90, 255) : IM_COL32(210, 30, 20, 255),
                    XS("Удалить"));

                ImGui::InvisibleButton(("##cfg_" + std::to_string(ci)).c_str(), {avW, cardH});

                if (!popBlocking2 && !IsScrollDragging() && !g_input.touchConsumed && io2.MouseReleased[0]
                    && PtInClip(io2.MouseClickedPos[0])) {
                    auto mp  = io2.MousePos;
                    auto cp2 = io2.MouseClickedPos[0];
                    if (mp.x  >= b1X && mp.x  <= b1X+bw3 && mp.y  >= bY && mp.y  <= bY+btnH2
                     && cp2.x >= b1X && cp2.x <= b1X+bw3 && cp2.y >= bY && cp2.y <= bY+btnH2)
                        ConfigLoad(ci);
                    else if (mp.x  >= b2X && mp.x  <= b2X+bw3 && mp.y  >= bY && mp.y  <= bY+btnH2
                     && cp2.x >= b2X && cp2.x <= b2X+bw3 && cp2.y >= bY && cp2.y <= bY+btnH2)
                        ConfigUpdate(ci);
                    else if (mp.x  >= b3X && mp.x  <= b3X+bw3 && mp.y  >= bY && mp.y  <= bY+btnH2
                     && cp2.x >= b3X && cp2.x <= b3X+bw3 && cp2.y >= bY && cp2.y <= bY+btnH2) {
                        g_configToDelete = ci;
                        PopoverOpen(XS("Удалить конфиг?"), 3);
                    }
                }

                if (ci < g_configCount - 1)
                    ImGui::Dummy({1.f, cardGap});
            }
        }

    } else if (tab == 5) {
        // Опции (interface + system) — now the bottom-most tab.
        // Счётчик FPS и «Рамки карточек» убраны: рамки включены принудительно.
        SHdr(XS("Интерфейс"));
        CardBg(Layout::RowH * 1);
        if (ToggleRow("##ud2", XS("Тёмная тема"), &g_state.ui_dark_mode, g_state.a_ui_dark, true, true))
            g_darkTheme = g_state.ui_dark_mode;

        // ---- Положение панели вкладок: слева или снизу ------------------
        // Перенесено сюда из удалённой вкладки «Меню».
        SHdr(XS("Панель вкладок"));
        {
            auto* dl  = ImGui::GetWindowDrawList();
            auto* fn  = ImGui::GetFont();
            float avW = ImGui::GetContentRegionAvail().x;
            const float inset = Layout::Inset;
            const float fs = ImGui::GetFontSize();

            const float cardH = 120.f;
            const float gap   = 10.f;
            auto  pos   = ImGui::GetCursorScreenPos();
            float x0    = pos.x + inset;
            float cardW = (avW - inset * 2.f - gap) * 0.5f;
            bool  popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

            struct POpt { const char* lbl; bool left; };
            const POpt opts[2] = { { XS("Слева"), true }, { XS("Снизу"), false } };

            for (int oi = 0; oi < 2; oi++) {
                float ox0 = x0 + oi * (cardW + gap);
                float ox1 = ox0 + cardW;
                bool  sel = (g_state.ui_panel_left == opts[oi].left);

                dl->AddRectFilled({ox0, pos.y}, {ox1, pos.y + cardH}, C::U(C::Card()), R::Card);
                if (sel) {
                    dl->AddRectFilled({ox0, pos.y}, {ox1, pos.y + cardH},
                        C::UA(C::Acc(), g_darkTheme ? 0.16f : 0.10f), R::Card);
                    dl->AddRect({ox0, pos.y}, {ox1, pos.y + cardH}, C::UA(C::Acc(), 0.8f), R::Card, 0, 2.f);
                } else if (g_state.ui_show_sep) {
                    dl->AddRect({ox0, pos.y}, {ox1, pos.y + cardH}, C::U(C::Sep()), R::Card, 0, 1.2f);
                }

                // Мини-схема окна: прямоугольник с панелью слева или снизу.
                float mw = 74.f, mh = 52.f;
                float mx = ox0 + (cardW - mw) * 0.5f;
                float my = pos.y + 14.f;
                ImU32 frameCol = C::UA(sel ? C::Acc() : C::Dim(), sel ? 0.95f : 0.6f);
                dl->AddRect({mx, my}, {mx + mw, my + mh}, frameCol, 6.f, 0, 2.f);
                if (opts[oi].left)
                    dl->AddRectFilled({mx + 3.f, my + 3.f}, {mx + 3.f + 16.f, my + mh - 3.f},
                                      C::UA(sel ? C::Acc() : C::Dim(), sel ? 0.85f : 0.4f), 4.f);
                else
                    dl->AddRectFilled({mx + 3.f, my + mh - 3.f - 12.f}, {mx + mw - 3.f, my + mh - 3.f},
                                      C::UA(sel ? C::Acc() : C::Dim(), sel ? 0.85f : 0.4f), 4.f);

                auto lsz = fn->CalcTextSizeA(fs * 1.0f, FLT_MAX, 0, opts[oi].lbl);
                dl->AddText(fn, fs * 1.0f,
                    {ox0 + (cardW - lsz.x) * 0.5f, pos.y + cardH - 16.f - lsz.y},
                    C::U(sel ? C::Acc() : C::Txt()), opts[oi].lbl);

                char oid[16]; snprintf(oid, sizeof(oid), "##pstyle%d", oi);
                ImGui::SetCursorScreenPos({ox0, pos.y});
                ImGui::InvisibleButton(oid, {cardW, cardH});
                if (WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed
                    && g_state.ui_panel_left != opts[oi].left) {
                    g_state.ui_panel_left = opts[oi].left;
                    pill_y = -1.f; pill_vel = 0.f;   // «пилюля» меняет ось — сброс пружины
                    ShowToast(opts[oi].left ? XS("Панель слева") : XS("Панель снизу"));
                    PlaySound(SND_CLICK);
                }
            }
            ImGui::SetCursorScreenPos({pos.x, pos.y + cardH});
            ImGui::Dummy({avW, 0.f});
        }

        SHdr(XS("Система"));
        {
            auto* dl  = ImGui::GetWindowDrawList();
            float avW = ImGui::GetContentRegionAvail().x;
            const float rowH = Layout::RowH, inset = Layout::Inset;
            auto pos = ImGui::GetCursorScreenPos();
            dl->AddRectFilled({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Card()), R::Card);
            if (g_state.ui_show_sep)
                dl->AddRect({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Sep()), R::Card, 0, 1.2f);
            ImGui::InvisibleButton("##exit", {avW, rowH});
            if (WasTappedHere() && !IsScrollDragging())
                PopoverOpen(XS("Выйти?"), 2);
            const char* exitTxt = XS("Выйти");
            float exitFS = ImGui::GetFontSize() * 1.15f;
            auto  tsz = ImGui::GetFont()->CalcTextSizeA(exitFS, FLT_MAX, 0, exitTxt);
            float tx  = pos.x + (avW - tsz.x) * 0.5f;
            float ty  = pos.y + (rowH - exitFS) * 0.5f;
            dl->AddText(ImGui::GetFont(), exitFS, {tx, ty}, C::U(C::Red()), exitTxt);
        }
    } else if (tab == 3) {
        // Разное: автофарм ресурсов.
        auto* dl  = ImGui::GetWindowDrawList();
        auto* fn  = ImGui::GetFont();
        float avW = ImGui::GetContentRegionAvail().x;
        const float inset = Layout::Inset;
        const float fs = ImGui::GetFontSize();

        SHdr(XS("Автофарм"));
        CardBg(Layout::RowH * 1);
        ToggleRow("##fm0", XS("Автофарм"), &g_state.farm_on, g_state.a_farm_on, true, true);

        SHdr(XS("Что добывать"));
        CardBg(Layout::RowH * 4);
        ToggleRow("##fm1", XS("Дерево"), &g_state.farm_wood,   g_state.a_farm_wood,   false, true);
        ToggleRow("##fm2", XS("Камень"), &g_state.farm_stone,  g_state.a_farm_stone,  false);
        ToggleRow("##fm3", XS("Металл"), &g_state.farm_metal,  g_state.a_farm_metal,  false);
        ToggleRow("##fm4", XS("Сера"),   &g_state.farm_sulfur, g_state.a_farm_sulfur, true);

        // Дальность поиска: насколько далеко бот согласен идти за ресурсом.
        SHdr(XS("Дальность"));
        CardBg(Layout::SliderH);
        SliderRow("##fmr", XS("Искать до"), &g_state.farm_range,
                  10.f, 300.f, XS("%.0f м"), true, true, g_state.sl_farm_range, dt);

        // Зоны бота: куда жать джойстик движения и кнопку огня. Раскладка
        // управления у всех разная — без калибровки бот может мимо попадать.
        extern int g_farmCalib; // определён ниже, рядом с UpdateFarm
        SHdr(XS("Зоны бота"));
        {
            bool popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;
            const float rowH = Layout::RowH;

            struct ZoneRow {
                const char* id;
                const char* lbl;
                int   calib;         // g_farmCalib для этой зоны
                float zx, zy;        // сохранённые доли экрана (-1 = нет)
            };
            const ZoneRow rows[2] = {
                {"##fz1", XS("Зона джойстика"), 1, g_state.farm_joy_x,  g_state.farm_joy_y},
                {"##fz2", XS("Зона огня"),      2, g_state.farm_fire_x, g_state.farm_fire_y},
            };

            CardBg(rowH * 2);
            for (int zi = 0; zi < 2; ++zi) {
                const ZoneRow& z = rows[zi];
                auto pos = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton(z.id, {avW, rowH});
                if (WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed) {
                    g_farmCalib = z.calib;
                    PlaySound(SND_CLICK);
                }

                // Название слева.
                float cy2 = pos.y + rowH * 0.5f;
                dl->AddText(fn, fs * 1.15f,
                    {pos.x + inset + Layout::PadX, cy2 - fs * 1.15f * 0.5f},
                    C::U(C::Txt()), z.lbl);

                // Справа — статус зоны: «Задать» или процент экрана + точка.
                char st[32];
                bool set = z.zx >= 0.f;
                if (set) snprintf(st, sizeof(st), "%d%% %d%%", (int)(z.zx * 100.f), (int)(z.zy * 100.f));
                else     snprintf(st, sizeof(st), "%s", XS("Задать"));
                auto stsz = fn->CalcTextSizeA(fs * 1.0f, FLT_MAX, 0, st);
                float stx = pos.x + avW - inset - Layout::PadX - stsz.x;
                dl->AddText(fn, fs * 1.0f, {stx, cy2 - stsz.y * 0.5f},
                    set ? C::U(C::Acc()) : C::U(C::Dim()), st);
                if (set)
                    dl->AddCircleFilled({stx - 16.f, cy2}, 5.f, C::U(C::Acc()), 16);

                if (zi == 0)
                    dl->AddLine({pos.x + inset + 12.f, pos.y + rowH},
                                {pos.x + avW - inset - 12.f, pos.y + rowH},
                                C::UA(C::Sep(), 0.7f), 1.f);
            }

            // Сброс зон к дефолту (если наставил мимо).
            if (g_state.farm_joy_x >= 0.f || g_state.farm_fire_x >= 0.f) {
                ImGui::Dummy({1.f, 8.f});
                CardBg(rowH * 1);
                auto rp = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##fz0", {avW, rowH});
                if (WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed) {
                    g_state.farm_joy_x = g_state.farm_joy_y = -1.f;
                    g_state.farm_fire_x = g_state.farm_fire_y = -1.f;
                    ShowToast(XS("Зоны сброшены"));
                    PlaySound(SND_CLICK);
                }
                const char* rt = XS("Сбросить зоны");
                auto rsz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, rt);
                dl->AddText(fn, fs * 1.05f,
                    {rp.x + (avW - rsz.x) * 0.5f, rp.y + (rowH - rsz.y) * 0.5f},
                    C::U(C::Dim()), rt);
            }
        }

        // Статус: что фарм делает прямо сейчас + детали для диагностики.
        SHdr(XS("Статус"));
        {
            const float stH = Layout::RowH * 1.35f;
            auto sp = ImGui::GetCursorScreenPos();
            dl->AddRectFilled({sp.x + inset, sp.y}, {sp.x + avW - inset, sp.y + stH},
                C::U(C::Card()), R::Card);
            if (g_state.ui_show_sep)
                dl->AddRect({sp.x + inset, sp.y}, {sp.x + avW - inset, sp.y + stH},
                    C::U(C::Sep()), R::Card, 0, 1.2f);

            const char* st;
            ImVec4 stCol = C::Dim();
            char detail[96] = {};
            if (!g_state.farm_on) {
                st = XS("Выключен");
            } else if (!Touch_CanInject()) {
                st = XS("Нет доступа к тачу");
                snprintf(detail, sizeof(detail), "%s", XS("тач в режиме чтения — перезапусти чит от root"));
            } else if (!g_esp_attached) {
                st = XS("Жду игру...");
            } else if (!g_farmActive) {
                // Почему цели нет — иначе «ничего не происходит» не отладить.
                switch (g_farmReason) {
                    case 2:  st = XS("Жду камеру игры...");
                             snprintf(detail, sizeof(detail), "%s", XS("зайди в мир, открой обзор")); break;
                    case 3:  st = XS("Ресурсы не найдены");
                             snprintf(detail, sizeof(detail), "%s", XS("рядом нет выбранных ресурсов")); break;
                    case 4:  st = XS("Всё далеко или выбито");
                             snprintf(detail, sizeof(detail), XS("в кэше узлов: %d"), g_farmNodes); break;
                    case 5:  st = XS("Нет позиции камеры");
                             snprintf(detail, sizeof(detail), "%s", XS("двинь камеру пальцем")); break;
                    default: st = XS("Ищу ресурс рядом...");
                             snprintf(detail, sizeof(detail), XS("в кэше узлов: %d"), g_farmNodes); break;
                }
            } else {
                const char* kindName = g_farmTgtKind == 0 ? XS("дерево")
                                     : g_farmTgtKind == 1 ? XS("камень")
                                     : g_farmTgtKind == 2 ? XS("металл") : XS("сера");
                if (g_farmPhase == 3)      st = XS("Добываю");
                else if (g_farmPhase == 2) st = XS("Иду к ресурсу");
                else                       st = XS("Поворачиваюсь");
                stCol = C::Acc();
                snprintf(detail, sizeof(detail), XS("%s, %.0f м"), kindName, g_farmTgtDist);
            }
            // Пульсирующая точка-индикатор слева от текста.
            float cy2 = sp.y + stH * 0.5f;
            float pulse = g_state.farm_on && g_farmActive
                ? 0.55f + 0.45f * sinf((float)ImGui::GetTime() * 5.f) : 1.f;
            dl->AddCircleFilled({sp.x + inset + Layout::PadX, cy2}, 8.f,
                C::UA(stCol, pulse), 20);
            float tx0 = sp.x + inset + Layout::PadX + 24.f;
            if (detail[0]) {
                auto ssz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, st);
                float totalH = ssz.y + 4.f + fs * 0.9f;
                float ty0 = sp.y + (stH - totalH) * 0.5f;
                dl->AddText(fn, fs * 1.05f, {tx0, ty0}, C::U(stCol), st);
                dl->AddText(fn, fs * 0.90f, {tx0, ty0 + ssz.y + 4.f}, C::U(C::Dim()), detail);
            } else {
                auto ssz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, st);
                dl->AddText(fn, fs * 1.05f, {tx0, cy2 - ssz.y * 0.5f}, C::U(stCol), st);
            }
            ImGui::Dummy({avW, stH});
        }

        // Подсказка, как этим пользоваться.
        {
            ImGui::Dummy({1.f, 10.f});
            const char* h1 = XS("Возьми в руки инструмент и включи автофарм.");
            const char* h2 = XS("Бот сам идёт к ближайшему ресурсу и бьёт по крестикам.");
            auto p = ImGui::GetCursorScreenPos();
            dl->AddText(fn, fs * 0.92f, {p.x + inset + 4.f, p.y}, C::U(C::Dim()), h1);
            dl->AddText(fn, fs * 0.92f, {p.x + inset + 4.f, p.y + fs}, C::U(C::Dim()), h2);
            ImGui::Dummy({1.f, fs * 2.f + 6.f});
        }

        ImGui::Dummy({1.f, 12.f});
    }

    ImGui::Dummy({1.f, 12.f});
    return ImGui::GetCursorPosY() - sY;
}

static uint32_t g_menu_orient = 0xFFFFFFFFu;
static int g_menu_dw = 0, g_menu_dh = 0;

static void VisibleScreen(float& w, float& h) {
    float dw = (float)displayInfo.width;
    float dh = (float)displayInfo.height;
    if (dw < 100.f) dw = (float)native_window_screen_x;
    if (dh < 100.f) dh = (float)native_window_screen_y;
    float mx = dw > dh ? dw : dh;
    float mn = dw < dh ? dw : dh;
    if (mx < 100.f) mx = 1080.f;
    if (mn < 100.f) mn = mx;
    bool land = (displayInfo.orientation == 1 || displayInfo.orientation == 3);
    if (dw > dh) land = true;
    else if (dh > dw && (displayInfo.orientation == 0 || displayInfo.orientation == 2)) land = false;
    w = land ? mx : mn;
    h = land ? mn : mx;
}

static void CenterMenuOnDisplay() {
    float dw = 0.f, dh = 0.f;
    VisibleScreen(dw, dh);
    if (dw < 100.f || dh < 100.f) return;
    if (g_win.w > dw - 16.f) g_win.w = ImMax(160.f, dw - 16.f);
    if (g_win.h > dh - 16.f) g_win.h = ImMax(160.f, dh - 16.f);
    g_win.pos.x = (dw - g_win.w) * 0.5f;
    g_win.pos.y = (dh - g_win.h) * 0.5f;
    g_win.dragging = false;
    g_win.resizing = false;
}

// ============================ Aimbot ============================
//
// Drives the game camera with a synthetic "look" finger on the right half of
// the screen. The target is the exact bone world position (head / chest /
// pelvis) projected through the live camera matrices, expressed as a yaw/pitch
// offset from the camera forward axis. The controller is closed-loop: every
// frame it measures how many degrees the crosshair actually moved per pixel
// of finger travel and adapts its gain, so it converges in a handful of
// frames regardless of the in-game sensitivity setting.

struct AimTarget {
    bool  valid = false;
    unsigned long long id = 0;
    float yaw = 0.f, pitch = 0.f;   // degrees from crosshair (+right, +up)
    float sx = 0.f, sy = 0.f;       // screen position (px)
    float dist = 0.f;               // pixel distance from crosshair
    float world_dist = 0.f;
};

static void AimReleaseFinger(bool& fingerDown) {
    if (fingerDown) { Touch_Up(); fingerDown = false; }
}

// File-scope so the auto-farm can yield the camera while the aimbot is
// actively pulling onto a player.
static bool s_fingerDown = false;

static void UpdateAim(float dt) {
    static float s_fx = 0.f, s_fy = 0.f;         // finger position (px)
    static float s_lastCamYaw = 0.f, s_lastCamPitch = 0.f; // absolute camera angles
    static bool  s_haveLast = false;
    static float s_lastDx = 0.f, s_lastDy = 0.f; // finger delta applied last frame
    static float s_gainYaw = 0.f, s_gainPitch = 0.f; // deg per px, learned
    static unsigned long long s_lastId = 0;        // sticky target
    static int   s_lostFrames = 0;
    static int   s_holdFrames = 0;

    const bool menuOpen = g_sheet.visible || (g_pop.visible && !g_pop.closing);
    bool active = g_state.aim_touch && g_esp_attached && !menuOpen;

    // "Только с прицелом": only steer while the local player is ADS.
    if (active && g_state.aim_scope_only && !esp_local_player_is_aiming())
        active = false;

    if (dt <= 0.f || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    if (!active) {
        AimReleaseFinger(s_fingerDown);
        s_haveLast = false; s_lastId = 0; s_lostFrames = 0; s_holdFrames = 0;
        return;
    }

    float sw = (float) native_window_screen_x;
    float sh = (float) native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float) displayInfo.width;  sh = (float) displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float) displayInfo.height; sh = (float) displayInfo.width;
    }
    if (sw < 100.f || sh < 100.f) { AimReleaseFinger(s_fingerDown); return; }

    const std::vector<EspBox>& boxes = FrameBoxes(sw, sh);

    const float crossX = sw * 0.5f, crossY = sh * 0.5f;
    const float fovR = AimFovRadiusPx(sw, sh);
    float camFov = esp_camera_fov_deg();
    if (!(camFov > 1.f && camFov < 179.f)) camFov = 60.f;
    // degrees per pixel at the screen centre (vertical axis)
    const float degPerPx = camFov / sh;

    const int wantBone = (g_state.aim_bone < 0 || g_state.aim_bone > 2) ? 0 : g_state.aim_bone;

    // ---- choose target ----
    AimTarget best;
    float bestScore = 1e18f;
    for (const EspBox& b : boxes) {
        // Never pull onto a team mate / clan mate while that ESP category is on.
        if (g_state.esp_team && b.ally) continue;
        AimTarget t;
        // Exact bone, with graceful fallback to the next-best bone.
        // Slots: 0 head, 1 neck, 2 chest. Never fall back below the chest.
        static const int order[3][3] = {{0, 1, 2}, {1, 0, 2}, {2, 1, 0}};
        int usedBone = -1;
        for (int k = 0; k < 3; ++k) {
            int bi = order[wantBone][k];
            if (b.aim_valid[bi]) { usedBone = bi; break; }
        }
        if (usedBone >= 0) {
            t.yaw = b.aim_yaw[usedBone];  t.pitch = b.aim_pitch[usedBone];
            t.sx  = b.aim_pts[usedBone][0]; t.sy = b.aim_pts[usedBone][1];
            t.valid = std::isfinite(t.yaw) && std::isfinite(t.pitch) &&
                      std::isfinite(t.sx) && std::isfinite(t.sy);
        } else {
            // Box estimate (nothing else resolved): derive angles from pixels.
            // The box itself is already crouch-aware (KCC pose height).
            if (!std::isfinite(b.x1) || !std::isfinite(b.y1) || !std::isfinite(b.x2) || !std::isfinite(b.y2)) continue;
            float h = b.y2 - b.y1;
            if (h < 4.f || h > sh * 4.f) continue;
            const float frac = (wantBone == 0) ? 0.07f : (wantBone == 1) ? 0.15f : 0.30f;
            t.sx = (b.x1 + b.x2) * 0.5f;
            t.sy = b.y1 + frac * h;
            float ex = t.sx - crossX, ey = t.sy - crossY;
            t.yaw = atanf(ex / (sh * 0.5f) * tanf(camFov * 0.5f * (float)M_PI / 180.f)) * 180.f / (float)M_PI;
            t.pitch = -atanf(ey / (sh * 0.5f) * tanf(camFov * 0.5f * (float)M_PI / 180.f)) * 180.f / (float)M_PI;
            t.valid = true;
        }
        if (!t.valid) continue;
        t.id = b.id;
        const bool sticky = (s_lastId != 0 && b.id == s_lastId);
        if (t.sx < -sw || t.sx > sw * 2.f || t.sy < -sh || t.sy > sh * 2.f) continue;
        if (g_state.aim_pos && !sticky && (t.sx < 0.f || t.sy < 0.f || t.sx > sw || t.sy > sh)) continue;
        float dx = t.sx - crossX, dy = t.sy - crossY;
        t.dist = sqrtf(dx * dx + dy * dy);
        t.world_dist = b.distance;
        // The target we are already pulling to may briefly leave the FOV
        // circle (overshoot while the gain is still being learned) — keep it.
        if (t.dist > (sticky ? fovR * 2.f : fovR)) continue;

        // Target priority (see "Приоритет цели"):
        //   0 balanced  — crosshair distance and range, both normalised
        //                 (FOV radius, 120 m) and summed;
        //   1 crosshair — purely the pixel distance from the crosshair;
        //   2 range     — purely the world distance, crosshair only breaks ties.
        // Whatever the mode, the current target gets a strong preference so the
        // aim does not flip between two players standing next to each other.
        const float pixelTerm = t.dist / (fovR > 1.f ? fovR : 1.f);
        const bool  haveRange = (t.world_dist >= 0.f) && std::isfinite(t.world_dist);
        const float rangeTerm = haveRange ? (t.world_dist / 120.f) : 1.f;
        float score;
        switch (g_state.aim_priority) {
            case 1:  score = pixelTerm; break;
            case 2:  score = rangeTerm * 4.f + pixelTerm * 0.05f; break;
            default: score = pixelTerm + rangeTerm * 0.8f; break;
        }
        if (sticky) score *= 0.35f;
        if (score < bestScore) { bestScore = score; best = t; }
    }

    if (!best.valid) {
        // Keep the finger down briefly so a momentary read failure does not
        // register as a tap (tap-to-shoot in some layouts) or reset momentum.
        if (++s_lostFrames > 6) {
            AimReleaseFinger(s_fingerDown);
            s_haveLast = false; s_lastId = 0;
        }
        return;
    }
    s_lostFrames = 0;
    if (best.id != s_lastId) s_haveLast = false; // do not learn gain across a target switch

    // Lead a moving target: the game applies our finger delta next frame, by
    // which time the target has moved on. Use the target's angular velocity
    // relative to the camera (with the camera's own rotation removed) and
    // aim one frame ahead. Reset on target switch.
    static float s_prevTgtYaw = 0.f, s_prevTgtPitch = 0.f, s_prevCamYawT = 0.f, s_prevCamPitchT = 0.f;
    static bool  s_havePrevTgt = false;
    {
        float cy = 0.f, cp = 0.f;
        bool haveC = esp_camera_angles(cy, cp);
        if (best.id == s_lastId && s_havePrevTgt && haveC) {
            float dCamYaw = cy - s_prevCamYawT;
            while (dCamYaw > 180.f) dCamYaw -= 360.f;
            while (dCamYaw < -180.f) dCamYaw += 360.f;
            float dCamPitch = cp - s_prevCamPitchT;
            // world-space angular motion of the target = change in offset + camera rotation
            float vYaw = (best.yaw - s_prevTgtYaw) + dCamYaw;
            float vPitch = (best.pitch - s_prevTgtPitch) + dCamPitch;
            if (std::isfinite(vYaw) && std::isfinite(vPitch) && fabsf(vYaw) < 10.f && fabsf(vPitch) < 10.f) {
                s_prevTgtYaw = best.yaw; s_prevTgtPitch = best.pitch;
                // Below this the "motion" is bone animation jitter (breathing,
                // sway), which at long range is larger than the head itself.
                // Extrapolating it would double the error, so only lead real
                // movement. Aim more than a frame ahead for fast movers so the
                // crosshair stays on a laterally running target (the controller
                // smoothing otherwise makes it trail behind).
                const float leadMin = degPerPx * 2.f;
                float vMag = sqrtf(vYaw * vYaw + vPitch * vPitch);
                if (vMag > leadMin) {
                    float k = 1.1f * (1.f - leadMin / vMag);
                    if (k > 1.5f) k = 1.5f;
                    best.yaw += vYaw * k;
                    best.pitch += vPitch * k;
                }
            } else {
                s_prevTgtYaw = best.yaw; s_prevTgtPitch = best.pitch;
            }
        } else {
            s_prevTgtYaw = best.yaw; s_prevTgtPitch = best.pitch;
        }
        if (haveC) { s_prevCamYawT = cy; s_prevCamPitchT = cp; s_havePrevTgt = true; }
        else s_havePrevTgt = false;
    }
    s_lastId = best.id;

    // ---- learn finger gain (deg per px) from the previous frame ----
    // Measured from the camera's own rotation, so a moving target does not
    // pollute the estimate.
    float camYaw = 0.f, camPitch = 0.f;
    const bool haveCam = esp_camera_angles(camYaw, camPitch);
    if (haveCam && s_haveLast && s_fingerDown) {
        float camYawDelta = camYaw - s_lastCamYaw;
        while (camYawDelta > 180.f) camYawDelta -= 360.f;
        while (camYawDelta < -180.f) camYawDelta += 360.f;
        float camPitchDelta = camPitch - s_lastCamPitch;
        // Signed gains: a negative value simply means the game inverts that
        // axis (e.g. "invert Y" enabled) and the controller follows suit.
        // Adopt the measurement outright when it disagrees strongly with the
        // current estimate (sensitivity changed / first sample), else smooth.
        auto learn = [](float& gain, float measured) {
            float m = fabsf(measured);
            if (!std::isfinite(measured) || m < 0.005f || m > 2.0f) return;
            if (gain == 0.f || (measured > 0.f) != (gain > 0.f) ||
                m > fabsf(gain) * 1.3f || m < fabsf(gain) * 0.7f) gain = measured;
            else gain = gain * 0.7f + measured * 0.3f;
        };
        // Finger right (dx > 0) turns right => yaw increases.
        // Moves are exact device-grid steps now, so even 1-2 px moves give a
        // clean measurement; keep a floor so noise never dominates.
        if (fabsf(s_lastDx) >= 1.f) learn(s_gainYaw, camYawDelta / s_lastDx);
        // Finger down (dy > 0) looks down => pitch decreases.
        if (fabsf(s_lastDy) >= 1.f) learn(s_gainPitch, -camPitchDelta / s_lastDy);
    }
    if (haveCam) { s_lastCamYaw = camYaw; s_lastCamPitch = camPitch; s_haveLast = true; }
    else s_haveLast = false;

    // ---- input quantum ----
    // The finger can only rest on the digitizer grid, so the camera can only
    // be steered in steps of (gain / units-per-pixel) degrees. At long range
    // one such step can exceed the size of a head; the controller therefore
    // has to settle on the NEAREST reachable position and hold still there,
    // never hunt back and forth across the target.
    float unitsPerPx = Touch_DeviceUnitsPerPixel();
    if (!(unitsPerPx >= 0.25f && unitsPerPx <= 16.f)) unitsPerPx = 1.f;
    const float gridPx = 1.f / unitsPerPx;               // screen px per device unit
    const bool  gainKnownYaw = (s_gainYaw != 0.f), gainKnownPitch = (s_gainPitch != 0.f);
    const float qYaw   = gainKnownYaw   ? fabsf(s_gainYaw)   * gridPx : 0.f; // deg per device unit
    const float qPitch = gainKnownPitch ? fabsf(s_gainPitch) * gridPx : 0.f;

    // ---- dead zone ----
    // Target: ~3 cm at the target's range (well inside a head), but never
    // tighter than what the input grid can actually reach (0.55 step), and
    // never wider than 1.5 screen px.
    float deadBase = degPerPx * 1.5f;
    if (best.world_dist > 1.f && std::isfinite(best.world_dist)) {
        float d = atanf(0.03f / best.world_dist) * 180.f / (float)M_PI;
        if (d < deadBase) deadBase = d;
    }
    float deadYaw = deadBase, deadPitch = deadBase;
    if (gainKnownYaw   && deadYaw   < qYaw   * 0.55f) deadYaw   = qYaw   * 0.55f;
    if (gainKnownPitch && deadPitch < qPitch * 0.55f) deadPitch = qPitch * 0.55f;
    if (fabsf(best.yaw) < deadYaw && fabsf(best.pitch) < deadPitch) {
        s_lastDx = s_lastDy = 0.f;
        if (s_fingerDown) Touch_Move(s_fx, s_fy); // hold still, keep the touch alive
        return;
    }

    auto snapGrid = [&](float v) { return roundf(v * unitsPerPx) / unitsPerPx; };

    if (!s_fingerDown) {
        s_fx = snapGrid(sw * 0.74f);
        s_fy = snapGrid(sh * 0.50f);
        Touch_Down(s_fx, s_fy);
        s_fingerDown = true;
        s_lastDx = s_lastDy = 0.f;
        s_holdFrames = 0;
        return; // let the game register the touch before moving it
    }
    if (s_holdFrames < 1) { ++s_holdFrames; Touch_Move(s_fx, s_fy); return; }

    // ---- controller ----
    // Speed slider keeps the old direction (higher = faster):
    //   1 = gentle (~25% of the remaining error per frame), 10 = snap.
    float sm = g_state.gun_str;
    if (!(sm >= 1.f)) sm = 1.f;
    if (sm > 10.f) sm = 10.f;
    float frac = 0.25f + (sm - 1.f) / 9.f * 0.75f;   // 0.25 .. 1.00 per frame @60fps
    // Frame-rate independent: convert per-frame fraction to a rate.
    float k = 1.f - powf(1.f - frac, dt * 60.f);
    if (k > 1.f) k = 1.f;
    if (k < 0.05f) k = 0.05f;

    // Gain (deg per px). Until it has been measured, assume a HIGH in-game
    // sensitivity so the first probe move can only under-shoot; the true gain
    // is learned from that move and the next frame snaps the rest of the way.
    const float probeGain = 0.35f;
    const bool  learned = (s_gainYaw != 0.f);
    float gy = learned ? s_gainYaw : probeGain;
    float gp = (s_gainPitch != 0.f) ? s_gainPitch : (learned ? fabsf(s_gainYaw) : probeGain);

    float dx =  best.yaw   * k / gy;
    float dy = -best.pitch * k / gp;

    // Final approach: within a few input steps of the target, stop smoothing
    // and jump straight to the nearest reachable grid position. Smoothing
    // here would either creep for many frames or, once rounded, overshoot
    // and oscillate by a full step around the head.
    if (gainKnownYaw && fabsf(best.yaw) < qYaw * 3.f)
        dx = roundf((best.yaw / gy) * unitsPerPx) / unitsPerPx;
    if (gainKnownPitch && fabsf(best.pitch) < qPitch * 3.f)
        dy = roundf((-best.pitch / gp) * unitsPerPx) / unitsPerPx;

    // Clamp per-frame travel so a bad gain estimate never slingshots.
    const float maxStep = learned ? sh * 0.15f : sh * 0.05f;
    if (dx >  maxStep) dx =  maxStep;
    if (dx < -maxStep) dx = -maxStep;
    if (dy >  maxStep) dy =  maxStep;
    if (dy < -maxStep) dy = -maxStep;

    // Move in whole device units so the applied delta is exactly what we
    // measure next frame (gain learning) and the finger never accumulates a
    // hidden sub-unit remainder that later pops out as an unplanned step.
    float nx = snapGrid(s_fx + dx), ny = snapGrid(s_fy + dy);
    dx = nx - s_fx; dy = ny - s_fy;
    if (dx == 0.f && dy == 0.f) {
        s_lastDx = s_lastDy = 0.f;
        Touch_Move(s_fx, s_fy);
        return;
    }

    // Keep the finger in the look area. If it drifts to an edge, lift and
    // re-place it in the centre of the area instead of getting stuck.
    const float minX = sw * 0.56f, maxX = sw * 0.97f, minY = sh * 0.12f, maxY = sh * 0.88f;
    if (nx < minX || nx > maxX || ny < minY || ny > maxY) {
        Touch_Up();
        s_fingerDown = false;
        s_fx = snapGrid(sw * 0.74f); s_fy = snapGrid(sh * 0.50f);
        s_lastDx = s_lastDy = 0.f;
        s_haveLast = false;
        return;
    }
    s_fx = nx; s_fy = ny;
    s_lastDx = dx; s_lastDy = dy;
    Touch_Move(s_fx, s_fy);
}

// ============================ Auto-farm =============================
//
// Fully touch-driven: three synthetic fingers (move joystick, camera swipe,
// attack tap) — the game layer only tells us where the nearest selected
// resource node is (yaw/pitch/distance). The camera finger reuses the same
// gain the aimbot learned, or probes carefully with a fixed one.
//
// State machine per frame:
//   TURN  — swipe the camera towards the node until it is roughly centred;
//   WALK  — hold the move joystick forward (steering with the camera) until
//           the node is within reach;
//   MINE  — stand still, keep the crosshair on the node (glowing X when the
//           game shows one) and tap-attack repeatedly;
//   node depleted / lost -> pick the next one automatically.

bool g_farmActive = false;   // for the status line in the menu
int  g_farmPhase = 0;        // 0 idle, 1 turn, 2 walk, 3 mine
int  g_farmNodes = 0;        // nodes found by the last registry scan
int  g_farmReason = 1;       // idle reason from esp_farm_debug()
float g_farmTgtDist = 0.f;   // distance to the current target (m)
int  g_farmTgtKind = 0;      // current target resource kind

// Калибровка зон бота: 0 — нет, 1 — ждём тап по джойстику, 2 — по кнопке
// огня. Пока калибровка активна, меню скрыто и первый тап по экрану
// записывает позицию (в долях экрана) в g_state.
int  g_farmCalib = 0;

static void UpdateFarm(float dt) {
    static bool  s_moveDown = false;  // finger 0: move joystick
    static bool  s_lookDown = false;  // finger 1: camera swipe
    static bool  s_tapDown  = false;  // finger 2: attack taps
    static float s_lookX = 0.f, s_lookY = 0.f;
    static int   s_lookHold = 0;
    static int   s_tapTimer = 0;
    static float s_gainYaw = 0.f;     // deg per px, learned from our own swipes
    static float s_lastCamYaw = 0.f;
    static float s_lastDx = 0.f;
    static bool  s_haveLast = false;
    static unsigned long long s_nodeId = 0;
    static float s_stuckTime = 0.f;   // seconds without closing distance
    static float s_lastDist = 1e9f;
    static float s_mineTime = 0.f;    // seconds spent mining this node
    static float s_fracStart = -1.f;
    static float s_sinceDrain = 0.f;  // seconds since the node last lost HP
    static float s_evadeTime = 0.f;   // >0: sidestep manoeuvre in progress
    static float s_evadeDir = 1.f;    // +1 right, -1 left
    static int   s_evadeCount = 0;    // manoeuvres tried on this node
    static float s_settle = 0.f;      // pause between targets (fingers up)
    static float s_stickPx = 0.f, s_stickPy = 0.f; // smoothed stick position

    auto releaseAll = [&]() {
        if (s_moveDown) { Touch_Up_N(0); s_moveDown = false; }
        if (s_lookDown) { Touch_Up_N(1); s_lookDown = false; }
        if (s_tapDown)  { Touch_Up_N(2); s_tapDown = false; }
        s_haveLast = false;
        s_lookHold = 0;
    };

    if (dt <= 0.f || !std::isfinite(dt)) dt = 1.f / 60.f;
    if (dt > 0.1f) dt = 0.1f;

    unsigned mask = 0;
    if (g_state.farm_on) {
        if (g_state.farm_wood)   mask |= 1u;
        if (g_state.farm_stone)  mask |= 2u;
        if (g_state.farm_metal)  mask |= 4u;
        if (g_state.farm_sulfur) mask |= 8u;
    }
    esp_farm_set_resources(mask);
    esp_farm_set_range(g_state.farm_range);

    const bool menuBlocked = g_sheet.visible || (g_pop.visible && !g_pop.closing);
    // The aimbot owns the camera while it is on a player — farm yields fully.
    // g_farmCalib: пока пользователь тапает зоны, бот молчит.
    bool active = mask != 0 && g_esp_attached && !menuBlocked && !s_fingerDown && g_farmCalib == 0;

    if (!active) {
        releaseAll();
        g_farmActive = false; g_farmPhase = 0;
        s_nodeId = 0; s_stuckTime = 0.f; s_mineTime = 0.f;
        s_evadeTime = 0.f; s_evadeCount = 0; s_sinceDrain = 0.f; s_settle = 0.f;
        return;
    }

    float sw = (float)native_window_screen_x;
    float sh = (float)native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float)displayInfo.width;  sh = (float)displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float)displayInfo.height; sh = (float)displayInfo.width;
    }
    if (sw < 100.f || sh < 100.f) { releaseAll(); g_farmActive = false; return; }

    // The farm target is measured against the camera state published by
    // esp_get_boxes(); make sure this frame's snapshot exists even when the
    // ESP overlay and the aimbot did not request one.
    FrameBoxes(sw, sh);

    FarmTarget tgt;
    bool haveTgt = esp_farm_get_target(tgt) && tgt.valid;
    esp_farm_debug(g_farmNodes, g_farmReason);
    if (!haveTgt) {
        releaseAll();
        g_farmActive = false; g_farmPhase = 0;
        s_nodeId = 0; s_stuckTime = 0.f; s_mineTime = 0.f;
        s_evadeTime = 0.f; s_evadeCount = 0; s_sinceDrain = 0.f; s_settle = 0.f;
        return;
    }
    g_farmActive = true;
    g_farmTgtDist = tgt.dist;
    g_farmTgtKind = tgt.kind;

    if (tgt.id != s_nodeId) {
        // Switching nodes: lift every finger and stand still for a moment.
        // Without this the old walk/look inputs keep replaying against the
        // new target for a few frames — the frantic stomping-and-shaking
        // right after a node is finished.
        bool hadNode = s_nodeId != 0;
        s_nodeId = tgt.id;
        s_stuckTime = 0.f; s_lastDist = tgt.dist;
        s_mineTime = 0.f;  s_fracStart = tgt.fraction;
        s_sinceDrain = 0.f; s_evadeTime = 0.f; s_evadeCount = 0;
        if (hadNode) { releaseAll(); s_settle = 0.7f; }
    }

    // A node that is already mined out gets blacklisted on the spot instead
    // of being circled: the picker would only fall back to it when nothing
    // else is in range, and dancing around an empty stump helps nobody.
    if (tgt.fraction >= 0.f && tgt.fraction < 0.03f) {
        esp_farm_blacklist(tgt.id, 120.f);
        releaseAll();
        s_settle = 0.7f;
        s_nodeId = 0;
        g_farmPhase = 0;
        return;
    }

    // Settle pause between targets: fingers stay up, the camera stops, and
    // the next target starts from a clean slate.
    if (s_settle > 0.f) {
        s_settle -= dt;
        releaseAll();
        return;
    }

    // On-screen mark on the exact point the farm is working: the glowing spot
    // when one is found, otherwise the node body. Doubles as debug output —
    // if the mark is on the wrong object, the target picker is what to fix.
    if (tgt.on_screen) {
        auto* fg = ImGui::GetForegroundDrawList();
        ImU32 mc = tgt.has_spot ? IM_COL32(80, 255, 120, 230) : IM_COL32(255, 200, 60, 230);
        float r = 14.f;
        fg->AddCircle({tgt.sx, tgt.sy}, r, mc, 24, 3.f);
        fg->AddLine({tgt.sx - r * 1.6f, tgt.sy}, {tgt.sx - r * 0.5f, tgt.sy}, mc, 3.f);
        fg->AddLine({tgt.sx + r * 0.5f, tgt.sy}, {tgt.sx + r * 1.6f, tgt.sy}, mc, 3.f);
        fg->AddLine({tgt.sx, tgt.sy - r * 1.6f}, {tgt.sx, tgt.sy - r * 0.5f}, mc, 3.f);
        fg->AddLine({tgt.sx, tgt.sy + r * 0.5f}, {tgt.sx, tgt.sy + r * 1.6f}, mc, 3.f);
    }

    // ---- camera gain: learn from our own swipe, fall back to the aimbot's ----
    float camYaw = 0.f, camPitch = 0.f;
    bool haveCam = esp_camera_angles(camYaw, camPitch);
    if (haveCam && s_haveLast && fabsf(s_lastDx) >= 1.f) {
        float dyaw = camYaw - s_lastCamYaw;
        while (dyaw > 180.f) dyaw -= 360.f;
        while (dyaw < -180.f) dyaw += 360.f;
        float measured = dyaw / s_lastDx;
        float m = fabsf(measured);
        if (std::isfinite(measured) && m > 0.005f && m < 2.f) {
            if (s_gainYaw == 0.f || m > fabsf(s_gainYaw) * 1.5f || m < fabsf(s_gainYaw) * 0.5f)
                s_gainYaw = measured;
            else
                s_gainYaw = s_gainYaw * 0.8f + measured * 0.2f;
        }
    }
    if (haveCam) s_lastCamYaw = camYaw;
    s_haveLast = haveCam;
    s_lastDx = 0.f;

    float gain = (s_gainYaw != 0.f) ? s_gainYaw : 0.25f; // deg per px, safe probe

    // ---- decide the phase ----
    // reachDist is deliberately tight for trees (a thin trunk holds its node
    // position dead centre, and stopping 3+ m away leaves melee short); rocks
    // are physically bigger, so their centre sits further from where the
    // player can stand. While mining the move finger keeps nudging forward
    // until walkUntil, closing the last step on its own.
    const bool  isTree    = (tgt.kind == 0);
    const float reachDist = isTree ? 2.6f : 3.4f; // close enough to swing
    const float walkUntil = isTree ? 1.6f : 2.6f; // keep stepping in until this
    const float aimedYaw  = (g_farmPhase == 3) ? 8.f : 14.f; // deg tolerance
    bool inReach = tgt.dist <= reachDist;
    bool aimed   = fabsf(tgt.yaw) <= aimedYaw;

    int phase = inReach ? 3 : (aimed ? 2 : 1);
    g_farmPhase = phase;

    // ---- finger 1: camera swipe (yaw always; pitch only while mining) ----
    {
        float wantYawPx = tgt.yaw / gain;
        // While mining also pull the crosshair down/up onto the node/spot.
        float wantPitchPx = 0.f;
        if (phase == 3) {
            float gp = fabsf(gain);
            wantPitchPx = -tgt.pitch / gp;
        }
        // Dead zones in degrees with hysteresis: a swipe only starts when the
        // error is clearly outside, and stops well inside. This is what keeps
        // the camera from twitching left-right around the centre.
        float startDeg = (phase == 3) ? 4.0f : 10.f;
        float stopDeg  = (phase == 3) ? 1.5f : 4.f;
        float errDeg = fmaxf(fabsf(tgt.yaw), (phase == 3) ? fabsf(tgt.pitch) : 0.f);
        bool needTurn = s_lookDown ? (errDeg > stopDeg) : (errDeg > startDeg);

        if (needTurn) {
            if (!s_lookDown) {
                s_lookX = sw * 0.74f; s_lookY = sh * 0.42f;
                Touch_Down_N(1, s_lookX, s_lookY);
                s_lookDown = true;
                s_lookHold = 0;
            } else if (s_lookHold < 1) {
                ++s_lookHold; // let the game register the touch first
                Touch_Down_N(1, s_lookX, s_lookY);
            } else {
                // Proportional step: cover ~28% of the remaining error per
                // frame, capped. Fast on big errors, glides into the centre
                // without the stair-step jerks of fixed-size increments.
                float maxStep = sh * 0.075f;
                float dx = wantYawPx * 0.28f;
                if (dx >  maxStep) dx =  maxStep;
                if (dx < -maxStep) dx = -maxStep;
                float dy = wantPitchPx * 0.28f;
                float maxStepY = maxStep * 0.5f;
                if (dy >  maxStepY) dy =  maxStepY;
                if (dy < -maxStepY) dy = -maxStepY;
                float nx = s_lookX + dx, ny = s_lookY + dy;
                // Edge: lift and re-centre rather than dragging off-screen.
                if (nx < sw * 0.56f || nx > sw * 0.97f || ny < sh * 0.12f || ny > sh * 0.88f) {
                    Touch_Up_N(1); s_lookDown = false; s_haveLast = false;
                } else {
                    s_lookX = nx; s_lookY = ny;
                    Touch_Down_N(1, s_lookX, s_lookY);
                    s_lastDx = dx;
                }
            }
        } else if (s_lookDown) {
            Touch_Up_N(1); s_lookDown = false;
        }
    }

    // ---- finger 0: move joystick (bottom-left), held while walking ----
    {
        // Keep pressing in while mining until we are right at the node, so
        // thin trees (node centre inside the trunk) end up in melee range.
        // walkUntil gets hysteresis: press while further than +0.5 m, release
        // only once actually inside — no down/up flapping at the boundary
        // (the "stomping in place" bug).
        float pressAt = s_moveDown ? walkUntil : walkUntil + 0.5f;
        bool wantWalk = (phase == 2) ||
                        (phase == 1 && fabsf(tgt.yaw) < 70.f && tgt.dist > reachDist * 2.f) ||
                        (phase == 3 && tgt.dist > pressAt) ||
                        // Swinging for a while with zero drain = just out of
                        // melee reach (thin tree) — press in regardless.
                        (phase == 3 && s_sinceDrain > 3.f);
        if (s_evadeTime > 0.f) wantWalk = true; // manoeuvre drives the stick itself
        if (wantWalk) {
            // Virtual stick centre and a forward push, slightly steered
            // towards the node so small yaw errors do not need camera swipes.
            // Centre: calibrated position when set, sensible default otherwise.
            float cx = (g_state.farm_joy_x >= 0.f) ? sw * g_state.farm_joy_x : sw * 0.165f;
            float cy = (g_state.farm_joy_y >= 0.f) ? sh * g_state.farm_joy_y : sh * 0.70f;
            float r = sh * 0.16f;
            float px, py;
            if (s_evadeTime > 0.f) {
                // Obstacle manoeuvre: back off briefly, then strafe hard to
                // one side while still angled a bit forward, to slide around
                // walls/rocks the straight-line walk keeps bumping into.
                s_evadeTime -= dt;
                if (s_evadeTime > 1.1f) {          // first ~0.6 s: step back
                    px = cx;
                    py = cy + r * 0.9f;
                } else {                            // then: diagonal sidestep
                    px = cx + r * 0.95f * s_evadeDir;
                    py = cy - r * 0.35f;
                }
                if (s_evadeTime <= 0.f) { s_evadeTime = 0.f; s_stuckTime = 0.f; s_lastDist = 1e9f; }
            } else {
                float steer = tgt.yaw / 70.f;
                if (steer >  0.6f) steer =  0.6f;
                if (steer < -0.6f) steer = -0.6f;
                px = cx + r * steer;
                py = cy - r * sqrtf(1.f - steer * steer);
                if (phase == 3) {
                    // Final approach: gentle forward nudge straight at the node.
                    px = cx + r * 0.35f * ((tgt.yaw > 0.f) ? 1.f : -1.f) * fminf(fabsf(tgt.yaw) / 45.f, 1.f);
                    py = cy - r * 0.75f;
                }
            }
            if (!s_moveDown) {
                Touch_Down_N(0, cx, cy);      // land on the stick centre first
                s_moveDown = true;
                s_stickPx = cx; s_stickPy = cy;
            } else {
                // Glide the stick towards the wanted deflection instead of
                // teleporting it: some devices/game builds latch a huge jump
                // as a sideways flick, which sent the bot strafing off-line.
                float k = 1.f - expf(-14.f * dt);   // ~90% of the way in 0.16 s
                s_stickPx += (px - s_stickPx) * k;
                s_stickPy += (py - s_stickPy) * k;
                Touch_Down_N(0, s_stickPx, s_stickPy);
            }
        } else if (s_moveDown) {
            Touch_Up_N(0); s_moveDown = false;
        }
    }

    // ---- finger 2: attack taps while in reach ----
    {
        if (phase == 3) {
            s_mineTime += dt;
            // Tap rhythm: ~85 ms down, ~230 ms up — a believable fast tapper
            // that also matches melee swing cadence (extra taps are ignored
            // by the game, they just queue the next swing).
            s_tapTimer -= (int)roundf(dt * 1000.f);
            if (s_tapTimer <= 0) {
                if (!s_tapDown) {
                    // Attack tap: calibrated fire button when set, otherwise
                    // the right half of the screen clear of the look finger.
                    float fx = (g_state.farm_fire_x >= 0.f) ? sw * g_state.farm_fire_x : sw * 0.88f;
                    float fy = (g_state.farm_fire_y >= 0.f) ? sh * g_state.farm_fire_y : sh * 0.66f;
                    Touch_Down_N(2, fx, fy);
                    s_tapDown = true;
                    s_tapTimer = 85;
                } else {
                    Touch_Up_N(2);
                    s_tapDown = false;
                    s_tapTimer = 230;
                }
            }
        } else {
            if (s_tapDown) { Touch_Up_N(2); s_tapDown = false; }
            s_tapTimer = 0;
            s_mineTime = 0.f;
        }
    }

    // ---- watchdogs ----
    if (phase == 2 || phase == 1) {
        // No progress towards the node -> ran into an obstacle. First try to
        // walk around it (back off + sidestep, alternating sides); only when
        // the manoeuvres keep failing does the node get blacklisted.
        if (tgt.dist < s_lastDist - 0.25f) {
            s_lastDist = tgt.dist;
            s_stuckTime = 0.f;
        } else if (s_evadeTime <= 0.f) {
            s_stuckTime += dt;
            if (s_stuckTime > 3.f) {
                if (s_evadeCount < 4) {
                    s_evadeTime = 1.7f;                       // ~0.6 s back + ~1.1 s strafe
                    s_evadeDir = (s_evadeCount % 2 == 0) ? 1.f : -1.f;
                    ++s_evadeCount;
                    s_stuckTime = 0.f;
                } else {
                    esp_farm_blacklist(tgt.id, 30.f);
                    s_stuckTime = 0.f; s_lastDist = 1e9f; s_nodeId = 0;
                    s_evadeCount = 0; s_evadeTime = 0.f;
                }
            }
        }
    } else if (phase == 3) {
        s_evadeCount = 0; s_evadeTime = 0.f; // reached the node — obstacles cleared
        // Swinging but the node is not draining -> standing a hair too far
        // (thin trees) or wrong tool. The walk-in nudge handles the former;
        // if HP still will not move, give up sooner rather than later.
        bool draining = (tgt.fraction >= 0.f && s_fracStart >= 0.f && tgt.fraction < s_fracStart - 0.01f);
        if (draining) { s_fracStart = tgt.fraction; s_mineTime = 0.f; s_sinceDrain = 0.f; }
        else {
            s_sinceDrain += dt;
            if (s_mineTime > 14.f) {
                esp_farm_blacklist(tgt.id, 60.f);
                s_mineTime = 0.f; s_sinceDrain = 0.f; s_nodeId = 0;
            }
        }
    }
}

void RenderMenu() {
    auto& io = ImGui::GetIO();
    float dt = io.DeltaTime;
    if (dt > 0.1f) dt = 0.1f;

    if (g_menu_orient != displayInfo.orientation || g_menu_dw != (int)displayInfo.width || g_menu_dh != (int)displayInfo.height) {
        g_menu_orient = displayInfo.orientation;
        g_menu_dw = (int)displayInfo.width;
        g_menu_dh = (int)displayInfo.height;
        CenterMenuOnDisplay();
    }

    g_menuFadeIn += dt * 1.2f;
    if (g_menuFadeIn > 1.f) g_menuFadeIn = 1.f;

    CfgWatchTick();
    Tick(g_themeT, g_darkTheme, dt, 6.f);
    Tick(g_state.a_aim_touch,  g_state.aim_touch,          dt);
    Tick(g_state.a_aim_pos,    g_state.aim_pos,            dt);
    Tick(g_state.a_aim_head,   g_state.aim_bone == 0,      dt);
    Tick(g_state.a_aim_chest,  g_state.aim_bone == 1,      dt);
    Tick(g_state.a_aim_pelvis, g_state.aim_bone == 2,      dt);
    Tick(g_state.a_aim_spec,   g_state.aim_special,        dt);
    Tick(g_state.a_aim_scope,  g_state.aim_scope_only,     dt);
    Tick(g_state.a_esp_box,    g_state.esp_box,            dt);
    Tick(g_state.a_esp_name,   g_state.esp_name,           dt);
    Tick(g_state.a_esp_wall,   g_state.esp_wall,           dt);
    Tick(g_state.a_esp_chams,  g_state.esp_chams,          dt);
    Tick(g_state.a_esp_weapon, g_state.esp_weapon,         dt);
    Tick(g_state.a_esp_ore,    g_state.esp_ore,            dt);
    Tick(g_state.a_esp_animal, g_state.esp_animal,         dt);
    Tick(g_state.a_esp_loot,   g_state.esp_loot,           dt);
    Tick(g_state.a_esp_pickup, g_state.esp_pickup,         dt);
    Tick(g_state.a_esp_team,   g_state.esp_team,           dt);
    Tick(g_state.a_aim_pr0,    g_state.aim_priority == 0,  dt);
    Tick(g_state.a_aim_pr1,    g_state.aim_priority == 1,  dt);
    Tick(g_state.a_aim_pr2,    g_state.aim_priority == 2,  dt);
    Tick(g_state.a_esp_tracer, g_state.esp_tracer,         dt);
    Tick(g_state.a_esp_skeleton, g_state.esp_skeleton,     dt);
    Tick(g_state.a_ui_dark,    g_state.ui_dark_mode,       dt);
    Tick(g_state.a_farm_on,     g_state.farm_on,     dt);
    Tick(g_state.a_farm_wood,   g_state.farm_wood,   dt);
    Tick(g_state.a_farm_stone,  g_state.farm_stone,  dt);
    Tick(g_state.a_farm_metal,  g_state.farm_metal,  dt);
    Tick(g_state.a_farm_sulfur, g_state.farm_sulfur, dt);
    ApplyTheme();

    if (g_cfgLoadedIdx >= 0 && g_cfgLoadedIdx < kMaxConfigs)
        g_cfgLoadAnim[g_cfgLoadedIdx] += (1.f - g_cfgLoadAnim[g_cfgLoadedIdx]) * 10.f * dt;

    bool anyOverlayOpen = g_sheet.visible || (g_pop.visible && !g_pop.closing);

    if (io.MouseClicked[0] && anyOverlayOpen) g_input.touchConsumed = true;

    bool anyOverlayVisible = g_sheet.visible || g_pop.visible;
    if (!io.MouseDown[0] && !anyOverlayVisible) g_input.touchConsumed = false;

    DrawWatermark(dt);
    DrawToast(dt);

    // ---- Режим калибровки зон автофарма ------------------------------------
    // Меню спрятано; первый тап по экрану записывает позицию зоны (в долях
    // экрана). Затем — следующая зона или выход из режима.
    if (g_farmCalib != 0) {
        float dw = 0.f, dh = 0.f;
        VisibleScreen(dw, dh);
        if (dw < 100.f || dh < 100.f) { g_farmCalib = 0; return; }
        auto* fg = ImGui::GetForegroundDrawList();

        // Затемнение + рамка-акцент.
        fg->AddRectFilled({0, 0}, {dw, dh}, IM_COL32(0, 0, 0, 90));
        fg->AddRect({4.f, 4.f}, {dw - 4.f, dh - 4.f}, C::UA(C::Acc(), 0.9f), 10.f, 0, 3.f);

        // Подпись, что тапать.
        auto* fn = ImGui::GetFont();
        const char* title = (g_farmCalib == 1)
            ? XS("Тапни по центру джойстика движения")
            : XS("Тапни по кнопке огня / атаки");
        const char* sub = XS("Тап записывает зону. Меню откроется само.");
        float tfs = ImGui::GetFontSize() * 1.5f;
        auto tsz = fn->CalcTextSizeA(tfs, FLT_MAX, 0, title);
        auto ssz = fn->CalcTextSizeA(tfs * 0.62f, FLT_MAX, 0, sub);
        float ty = dh * 0.16f;
        // Плашка под текстом, чтобы читалось на любом фоне.
        float px0 = (dw - ImMax(tsz.x, ssz.x)) * 0.5f - 26.f;
        float px1 = (dw + ImMax(tsz.x, ssz.x)) * 0.5f + 26.f;
        fg->AddRectFilled({px0, ty - 18.f}, {px1, ty + tsz.y + 10.f + ssz.y + 18.f},
                          C::UA(C::Card(), 0.92f), 18.f);
        fg->AddText(fn, tfs, {(dw - tsz.x) * 0.5f, ty}, C::U(C::Txt()), title);
        fg->AddText(fn, tfs * 0.62f, {(dw - ssz.x) * 0.5f, ty + tsz.y + 10.f}, C::U(C::Dim()), sub);

        // Пульсирующий маркер текущей сохранённой зоны (если есть).
        {
            float zx = -1.f, zy = -1.f;
            if (g_farmCalib == 1 && g_state.farm_joy_x >= 0.f) { zx = g_state.farm_joy_x * dw; zy = g_state.farm_joy_y * dh; }
            if (g_farmCalib == 2 && g_state.farm_fire_x >= 0.f) { zx = g_state.farm_fire_x * dw; zy = g_state.farm_fire_y * dh; }
            if (zx >= 0.f) {
                float pr = 34.f + 6.f * sinf((float)ImGui::GetTime() * 4.f);
                fg->AddCircle({zx, zy}, pr, C::UA(C::Acc(), 0.85f), 40, 3.f);
                fg->AddCircleFilled({zx, zy}, 7.f, C::U(C::Acc()), 20);
            }
        }

        // Тап (отпускание пальца) — записываем зону.
        if (io.MouseReleased[0]) {
            float rx = io.MousePos.x / dw, ry = io.MousePos.y / dh;
            if (rx > 0.f && rx < 1.f && ry > 0.f && ry < 1.f) {
                if (g_farmCalib == 1) {
                    g_state.farm_joy_x = rx; g_state.farm_joy_y = ry;
                    ShowToast(XS("Зона джойстика сохранена"));
                } else {
                    g_state.farm_fire_x = rx; g_state.farm_fire_y = ry;
                    ShowToast(XS("Зона огня сохранена"));
                }
                PlaySound(SND_CLICK);
                g_farmCalib = 0;
                menu_open = true;
            }
        }
        return; // пока калибруемся, меню не рисуем
    }

    if (!menu_open) return;

    const float WW = g_win.w, WH = g_win.h;
    const float hH_ = Layout::HeaderH;

    {
        bool inResize = io.MousePos.x >= g_win.pos.x + g_win.w - R::Card - 14.f
                     && io.MousePos.x <= g_win.pos.x + g_win.w + 14.f
                     && io.MousePos.y >= g_win.pos.y + g_win.h - R::Card - 14.f
                     && io.MousePos.y <= g_win.pos.y + g_win.h + 14.f;

        if (!io.MouseDown[0]) g_win.resizing = false;

        bool anyOverlay = g_sheet.visible || (g_pop.visible && !g_pop.closing);
        if (!anyOverlay && io.MouseClicked[0] && inResize) {
            g_win.resizing         = true;
            g_win.resizeTouchStart = io.MousePos;
            g_win.sizeStart        = {g_win.w, g_win.h};
        }

        if (g_win.resizing && io.MouseDown[0]) {
            float dx = io.MousePos.x - g_win.resizeTouchStart.x;
            float dy = io.MousePos.y - g_win.resizeTouchStart.y;
            g_win.w = ImMax(WW_MIN, g_win.sizeStart.x + dx);
            g_win.h = ImMax(WH_MIN, g_win.sizeStart.y + dy);
        }
    }

    {
        if (!io.MouseDown[0]) g_win.dragging = false;

        if (!g_win.dragging && io.MouseDown[0] && !(g_sheet.visible || (g_pop.visible && !g_pop.closing)) && !g_win.resizing) {
            float tx = io.MouseClickedPos[0].x, ty = io.MouseClickedPos[0].y;
            // Тащим окно за верхнюю шапку (по всей ширине).
            bool inHdr = tx >= g_win.pos.x && tx < g_win.pos.x + g_win.w
                      && ty >= g_win.pos.y && ty < g_win.pos.y + hH_;
            if (inHdr) {
                float dx = io.MousePos.x - io.MouseClickedPos[0].x;
                float dy = io.MousePos.y - io.MouseClickedPos[0].y;
                if (fabsf(dx) + fabsf(dy) > 8.f) {
                    g_win.dragging   = true;
                    g_win.touchStart = io.MouseClickedPos[0];
                    g_win.posStart   = g_win.pos;
                }
            }
        }

        if (g_win.dragging && io.MouseDown[0]) {
            float dx   = io.MousePos.x - g_win.touchStart.x;
            float dy   = io.MousePos.y - g_win.touchStart.y;
            g_win.pos.x = g_win.posStart.x + dx;
            g_win.pos.y = g_win.posStart.y + dy;
        }
        float dw = 0.f, dh = 0.f;
        VisibleScreen(dw, dh);
        if (dw > 100.f && dh > 100.f) {
            g_win.pos.x = ImClamp(g_win.pos.x, 0.f, ImMax(0.f, dw - g_win.w));
            g_win.pos.y = ImClamp(g_win.pos.y, 0.f, ImMax(0.f, dh - g_win.h));
        }
    }

    ImGui::SetNextWindowSize({g_win.w, g_win.h}, ImGuiCond_Always);
    ImGui::SetNextWindowPos(g_win.pos, ImGuiCond_Always);

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoCollapse
                        | ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoScrollbar
                        | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground
                        | ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoNav;

    ImGui::Begin("##menu", nullptr, wf);
    {
        auto  bgP  = g_win.pos;
        auto* bgDL = ImGui::GetWindowDrawList();
        bgDL->AddRectFilled(bgP, {bgP.x + WW, bgP.y + WH}, C::U(C::Bg()), R::Card);
    }

    ImVec2 wp = g_win.pos;
    // Панель вкладок: слева (вертикальный столбец) или снизу (строка по
    // центру) — выбирается в «Опциях». Контент занимает остальную площадь.
    const bool  panelLeft = g_state.ui_panel_left;
    const float botH = Layout::BottomH;
    const float railW = Layout::RailW;
    const float cH = panelLeft ? g_win.h : g_win.h - botH;
    const float cW = panelLeft ? g_win.w - railW : g_win.w;
    const float cX0 = panelLeft ? railW : 0.f;

    g_state.tab_alpha += (1.f - g_state.tab_alpha) * 5.f * dt;
    if (g_state.tab_alpha > .999f) g_state.tab_alpha = 1.f;
    SpringTick(g_state.tab_slide, g_state.tab_slide_vel, 0.f, dt);

    // Короткие и понятные названия вкладок (индекс = id вкладки).
    const char* tabNames[kTabCount] = {
        XS("Меню"), XS("Аим"), XS("ESP"), XS("Разное"), XS("Конфиги"), XS("Опции")
    };
    // Вкладка «Меню» (id 0) удалена: её функционал переехал в «Опции».
    // id контента вкладок не меняются — конфиги и логика остаются как были.
    static constexpr int kTabShown = 5;
    static constexpr int kTabOrder[kTabShown] = {1, 2, 3, 4, 5};

    // Иконка вкладки + подпись, по центру ячейки (общая для обеих панелей).
    auto DrawTabCell = [&](ImDrawList* fdl, int i, float cellX, float cellY,
                           float cellW, float cellH, float iconSize, float lblFS) {
        bool   active = (i == g_state.cur_tab);
        ImVec4 col    = active ? C::Acc() : C::Dim();
        auto   tsz    = ImGui::GetFont()->CalcTextSizeA(lblFS, FLT_MAX, 0, tabNames[i]);
        float  blockH = iconSize + 6.f + tsz.y;
        float  iconY  = cellY + (cellH - blockH) * 0.5f;
        float  cxr    = cellX + cellW * 0.5f;
        if (i == 3) {
            // У «Разное» своя векторная иконка (сетка 2x2), чтобы не
            // совпадала с иконкой «Конфиги» из общего атласа.
            float half = iconSize * 0.5f;
            float gx0 = cxr - half, gy0 = iconY;
            float cell = iconSize * 0.44f, gap2 = iconSize - cell * 2.f;
            ImU32 gcol = C::UA(C::Acc(), active ? 1.f : 0.55f);
            float rr = cell * 0.3f;
            fdl->AddRectFilled({gx0, gy0}, {gx0 + cell, gy0 + cell}, gcol, rr);
            fdl->AddRectFilled({gx0 + cell + gap2, gy0}, {gx0 + iconSize, gy0 + cell}, gcol, rr);
            fdl->AddRectFilled({gx0, gy0 + cell + gap2}, {gx0 + cell, gy0 + iconSize}, gcol, rr);
            // Четвёртый квадрат — контурный, чтобы иконка читалась как «прочее».
            fdl->AddRect({gx0 + cell + gap2, gy0 + cell + gap2}, {gx0 + iconSize, gy0 + iconSize}, gcol, rr, 0, 2.f);
        } else if (g_tabIcons[i]) {
            ImVec2 iMin = {cxr - iconSize * 0.5f, iconY};
            ImVec2 iMax = {cxr + iconSize * 0.5f, iconY + iconSize};
            fdl->AddImageRounded((ImTextureID)(intptr_t)g_tabIcons[i], iMin, iMax,
                {0,0}, {1,1}, IM_COL32(255, 255, 255, active ? 255 : 165), 9.f);
        } else {
            // Заглушка: скруглённый квадрат с первой буквой вкладки.
            ImVec2 iMin = {cxr - iconSize * 0.5f, iconY};
            ImVec2 iMax = {cxr + iconSize * 0.5f, iconY + iconSize};
            fdl->AddRectFilled(iMin, iMax, C::UA(C::Acc(), active ? 0.9f : 0.35f), 9.f);
            char letter[8] = {};
            int gl = 0;
            letter[gl++] = tabNames[i][0];
            if ((unsigned char)tabNames[i][0] >= 0xC0) letter[gl++] = tabNames[i][1];
            float gfs = iconSize * 0.55f;
            auto gsz = ImGui::GetFont()->CalcTextSizeA(gfs, FLT_MAX, 0, letter);
            fdl->AddText(ImGui::GetFont(), gfs,
                {cxr - gsz.x * 0.5f, iconY + (iconSize - gsz.y) * 0.5f},
                IM_COL32(255, 255, 255, active ? 255 : 200), letter);
        }
        fdl->AddText(ImGui::GetFont(), lblFS,
            {cxr - tsz.x * 0.5f, iconY + iconSize + 6.f}, C::U(col), tabNames[i]);
    };

    auto TabTap = [&](int i) {
        if (WasTappedHere() && !IsScrollDragging() && !g_input.touchConsumed && g_state.cur_tab != i) {
            g_state.cur_tab       = i;
            g_state.tab_alpha     = 0.f;
            g_state.tab_slide     = 50.f;
            g_state.tab_slide_vel = 0.f;
            g_scrollMain          = {};
            PlaySound(SND_CLICK);
        }
    };

    if (panelLeft) {
        // ---- Левая панель вкладок (вертикальный столбец по центру) ------
        ImGui::SetCursorPos({0, 0});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##lp", {railW, WH}, false, ImGuiWindowFlags_NoScrollbar);
        auto  lpPos = ImGui::GetWindowPos();
        auto* ldl   = ImGui::GetWindowDrawList();
        ldl->AddRectFilled(lpPos, {lpPos.x + railW, lpPos.y + WH}, C::U(C::LeftBg()), R::Card,
                           ImDrawFlags_RoundCornersLeft);
        ldl->AddLine({lpPos.x + railW, lpPos.y + 12.f}, {lpPos.x + railW, lpPos.y + WH - 12.f},
                     C::UA(C::Sep(), 0.8f), 1.f);

        const float tabH   = Layout::TabHV;
        const float colH   = tabH * kTabShown;
        const float startY = ImMax(0.f, (WH - colH) * 0.5f);

        for (int s = 0; s < kTabShown; s++) {
            const int i = kTabOrder[s];
            ImGui::SetCursorPos({0, startY + s * tabH});
            auto pos2 = ImGui::GetCursorScreenPos();
            tab_rects[i] = {pos2.y};
            char tabId[16];
            snprintf(tabId, sizeof(tabId), "##tab%d", i);
            ImGui::InvisibleButton(tabId, {railW, tabH});
            TabTap(i);
        }

        // Пружинная «пилюля» активной вкладки скользит по вертикали.
        {
            float targetRel = tab_rects[g_state.cur_tab].sx - wp.y;
            if (pill_y < 0.f) { pill_y = targetRel; pill_vel = 0.f; }
            SpringTick(pill_y, pill_vel, targetRel, dt);
        }
        float pillY = wp.y + pill_y;

        {
            auto*       fdl = ImGui::GetForegroundDrawList();
            const float pR  = R::Pill;
            float px0 = lpPos.x + 9.f,  px1 = lpPos.x + railW - 9.f;
            float py0 = pillY + 7.f,    py1 = pillY + tabH - 7.f;
            fdl->AddRectFilled({px0, py0}, {px1, py1}, C::U(C::Card()), pR);
            fdl->AddRectFilled({px0, py0}, {px1, py1}, C::UA(C::Acc(), g_darkTheme ? 0.16f : 0.10f), pR);
            fdl->AddRect({px0, py0}, {px1, py1}, C::UA(C::Acc(), 0.5f), pR, 0, 1.5f);
            // Короткая акцентная полоска на левом краю окна.
            float icy = (py0 + py1) * 0.5f;
            fdl->AddRectFilled({lpPos.x - 1.f, icy - 16.f}, {lpPos.x + 3.f, icy + 16.f}, C::U(C::Acc()), 2.f);
        }

        {
            auto* fdl = ImGui::GetForegroundDrawList();
            for (int s = 0; s < kTabShown; s++) {
                const int i = kTabOrder[s];
                DrawTabCell(fdl, i, lpPos.x, tab_rects[i].sx, railW, tabH, 56.f,
                            ImGui::GetFontSize() * 0.88f);
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        // ---- Нижняя панель вкладок (строка по центру) --------------------
        ImGui::SetCursorPos({0, cH});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##lp", {WW, botH}, false, ImGuiWindowFlags_NoScrollbar);
        auto  lpPos = ImGui::GetWindowPos();
        auto* ldl   = ImGui::GetWindowDrawList();
        ldl->AddRectFilled(lpPos, {lpPos.x + WW, lpPos.y + botH}, C::U(C::LeftBg()), R::Card,
                           ImDrawFlags_RoundCornersBottom);
        ldl->AddLine({lpPos.x + 14.f, lpPos.y}, {lpPos.x + WW - 14.f, lpPos.y},
                     C::UA(C::Sep(), 0.8f), 1.f);

        // Ряд вкладок строго по центру панели.
        const float tabW   = Layout::TabW;
        const float rowW   = tabW * kTabShown;
        const float startX = (WW - rowW) * 0.5f;

        for (int s = 0; s < kTabShown; s++) {
            const int i = kTabOrder[s];   // id вкладки в этой позиции панели
            ImGui::SetCursorPos({startX + s * tabW, 0});
            auto pos2 = ImGui::GetCursorScreenPos();
            tab_rects[i] = {pos2.x};
            char tabId[16];
            snprintf(tabId, sizeof(tabId), "##tab%d", i);
            ImGui::InvisibleButton(tabId, {tabW, botH});
            TabTap(i);
        }

        // Пружинная «пилюля» активной вкладки скользит по горизонтали.
        {
            float targetRelX = tab_rects[g_state.cur_tab].sx - wp.x;
            if (pill_y < 0.f) { pill_y = targetRelX; pill_vel = 0.f; }
            SpringTick(pill_y, pill_vel, targetRelX, dt);
        }
        float pillX = wp.x + pill_y;
        float barY  = lpPos.y;

        {
            auto*       fdl = ImGui::GetForegroundDrawList();
            const float pR  = R::Pill;
            float px0 = pillX + 8.f,        px1 = pillX + tabW - 8.f;
            float py0 = barY + 12.f,        py1 = barY + botH - 14.f;
            fdl->AddRectFilled({px0, py0}, {px1, py1}, C::U(C::Card()), pR);
            fdl->AddRectFilled({px0, py0}, {px1, py1}, C::UA(C::Acc(), g_darkTheme ? 0.16f : 0.10f), pR);
            fdl->AddRect({px0, py0}, {px1, py1}, C::UA(C::Acc(), 0.5f), pR, 0, 1.5f);
            // Короткая акцентная полоска сверху — указывает на активную вкладку.
            float icx = (px0 + px1) * 0.5f;
            fdl->AddRectFilled({icx - 16.f, barY - 1.f}, {icx + 16.f, barY + 3.f}, C::U(C::Acc()), 2.f);
        }

        {
            auto* fdl = ImGui::GetForegroundDrawList();
            for (int s = 0; s < kTabShown; s++) {
                const int i = kTabOrder[s];
                DrawTabCell(fdl, i, tab_rects[i].sx, barY, tabW, botH, 62.f,
                            ImGui::GetFontSize() * 0.95f);
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    ImGui::SetCursorPos({cX0, 0});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##cp", {cW, cH}, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    {
        // Шапка: заголовок вкладки по центру.
        const char* titles[kTabCount] = {XS("Меню"), XS("Аим"), XS("ESP"), XS("Разное"), XS("Конфиги"), XS("Опции")};
        auto*  cdl = ImGui::GetWindowDrawList();
        auto   hp  = ImGui::GetWindowPos();
        const float hH = Layout::HeaderH;
        const float titleFS = ImGui::GetFontSize() * 1.45f;
        auto tsz = ImGui::GetFont()->CalcTextSizeA(titleFS, FLT_MAX, 0, titles[g_state.cur_tab]);
        cdl->AddText(ImGui::GetFont(), titleFS,
            {hp.x + (cW - tsz.x) * 0.5f, hp.y + (hH - tsz.y) * 0.5f}, C::U(C::Txt()), titles[g_state.cur_tab]);
        ImGui::Dummy({cW, hH});
    }

    {
        const float areaH = cH - Layout::HeaderH;
        auto cpPos  = ImGui::GetWindowPos(); cpPos.y += Layout::HeaderH;
        const float cpRight = cpPos.x + cW;

        bool sheetBlocking = g_sheet.visible || (g_pop.visible && !g_pop.closing);
        static float s_maxScroll = 0.f;

        bool clickInRight = (io.MouseClickedPos[0].x >= cpPos.x)
                         && (io.MouseClickedPos[0].x <= cpRight)
                         && (io.MouseClickedPos[0].y >= cpPos.y)
                         && (io.MouseClickedPos[0].y <= cpPos.y + areaH);
        bool mIn = clickInRight
            && io.MousePos.x >= cpPos.x  && io.MousePos.x <= cpRight
            && io.MousePos.y >= cpPos.y  && io.MousePos.y <= cpPos.y + areaH;

        auto* cdl = ImGui::GetWindowDrawList();
        cdl->PushClipRect(cpPos, {cpRight, cpPos.y + areaH - 4.f}, true);

        float slideOffset = g_state.tab_slide;
        ImGui::SetCursorPosY(Layout::HeaderH - g_scrollMain.off + slideOffset);
        ImGui::SetCursorPosX(12.f);

        if (sheetBlocking) ImGui::BeginDisabled(true);
        float contentH = TabContent(g_state.cur_tab, dt, cW);
        if (sheetBlocking) ImGui::EndDisabled();

        if (g_state.tab_alpha < 0.999f) {
            float fadeA = 1.f - g_state.tab_alpha;
            cdl->AddRectFilled(cpPos, {cpRight, cpPos.y + areaH},
                C::UA(C::Bg(), fadeA), R::Card, ImDrawFlags_RoundCornersTop);
        }

        s_maxScroll = ImMax(0.f, contentH - (areaH - 4.f));
        ScrollTick(g_scrollMain, mIn, sheetBlocking, s_maxScroll, dt);

        if (s_maxScroll > 0.f) {
            const float sbPad  = 5.f;
            const float sbVPad = 8.f;
            const float sbMinH = 36.f;
            const float sbW    = 8.f;

            float ratio     = (areaH * 0.28f) / (areaH + s_maxScroll);
            float sbHTarget = ImMax(sbMinH, areaH * ratio);
            float sbTrack   = areaH - sbHTarget - sbVPad * 2.f;
            float sbTTarget = (s_maxScroll > 0.f) ? (g_scrollMain.off / s_maxScroll) * sbTrack : 0.f;

            g_scrollMain.sb_w  = sbW;
            g_scrollMain.sb_y += (sbTTarget - g_scrollMain.sb_y) * 10.f * dt;
            g_scrollMain.sb_h += (sbHTarget - g_scrollMain.sb_h) * 10.f * dt;

            float sbX0 = cpRight - sbPad - sbW;
            float sbX1 = sbX0 + sbW;
            float sbY0 = cpPos.y + sbVPad + g_scrollMain.sb_y;
            float sbY1 = sbY0 + g_scrollMain.sb_h;

            ImU32 thumbCol = C::UA(C::Dim(), 0.55f);

            auto* fdl = ImGui::GetForegroundDrawList();
            fdl->PushClipRect(cpPos, {cpRight, cpPos.y + areaH}, true);
            fdl->AddRectFilled({sbX0, sbY0}, {sbX1, sbY1}, thumbCol, sbW * 0.5f);
            fdl->PopClipRect();
        }

        cdl->PopClipRect();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    {
        static constexpr float kResizePad       = 22.f;
        static constexpr float kResizeStroke     = 10.f;
        static constexpr float kResizeGlowStroke = 6.f;
        static constexpr float kResizeGlowAlpha  = 0.13f;
        static constexpr float kArcStroke        = 14.f;
        static constexpr float kArcA0            = -0.25f;
        static constexpr float kArcA1            = IM_PI * 0.5f + 0.25f;
        static constexpr float kCornerOffset     = 10.f;

        auto* rdl = ImGui::GetForegroundDrawList();

        if (g_win.resizing) {
            const float pad = kResizePad, rr = R::Card + pad;
            float t = EaseInOut(g_themeT);
            ImVec4 bc = {Lerpf(1.f, C::Dark::Acc.x, t), Lerpf(1.f, C::Dark::Acc.y, t), Lerpf(1.f, C::Dark::Acc.z, t), Lerpf(0.9f, 0.8f, t)};
            rdl->AddRect({wp.x - pad,       wp.y - pad},
                         {wp.x + g_win.w + pad, wp.y + g_win.h + pad},
                         C::U(bc), rr, 0, kResizeStroke);
            rdl->AddRect({wp.x - pad - 8.f,       wp.y - pad - 8.f},
                         {wp.x + g_win.w + pad + 8.f, wp.y + g_win.h + pad + 8.f},
                         C::UA(bc, kResizeGlowAlpha), rr + 8.f, 0, kResizeGlowStroke);
        }

        float cx = wp.x + g_win.w - R::Card - kCornerOffset;
        float cy = wp.y + g_win.h - R::Card - kCornerOffset;

        ImU32       col   = g_darkTheme ? IM_COL32(200, 200, 210, 255) : IM_COL32(22, 22, 24, 255);
        const float r     = R::Card;
        const float thick = kArcStroke;
        const float a0    = kArcA0;
        const float a1    = kArcA1;

        rdl->PathArcTo({cx, cy}, r, a0, a1, 48);
        rdl->PathStroke(col, 0, thick);

        float capR = thick * 0.5f;
        rdl->AddCircleFilled({cx + r * cosf(a0), cy + r * sinf(a0)}, capR, col, 24);
        rdl->AddCircleFilled({cx + r * cosf(a1), cy + r * sinf(a1)}, capR, col, 24);
    }

    DrawSheet(dt, wp, WW, WH);
    DrawPopover(dt, wp, WW, WH);
    ImGui::End();
    if (g_menuFadeIn < 1.f) {
        float mA = 1.f - EaseInOut(g_menuFadeIn);
        auto* ofg = ImGui::GetForegroundDrawList();
        ImVec4 bgC = C::Bg();
        ofg->AddRectFilled(g_win.pos, {g_win.pos.x + WW, g_win.pos.y + WH},
            IM_COL32(int(bgC.x*255), int(bgC.y*255), int(bgC.z*255), int(mA * 255)), R::Card);
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT,  [](int) { main_thread_flag.store(false); });
    signal(SIGTERM, [](int) { main_thread_flag.store(false); });
    signal(SIGHUP,  [](int) { main_thread_flag.store(false); });

    prot::Init();
    screen_config();
    int abs_ScreenX = displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width;
    int abs_ScreenY = displayInfo.height < displayInfo.width ? displayInfo.height : displayInfo.width;

    g_sw = static_cast<float>(abs_ScreenX);
    g_sh = static_cast<float>(abs_ScreenY);

    native_window_screen_x = abs_ScreenX;
    native_window_screen_y = abs_ScreenY;
    if (!initGUI_draw(native_window_screen_x, native_window_screen_x, true)) return -1;
    Blur::Init();
    CfgWatchInit();
    AudioInit();
    if (!Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, false))
        Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);
    start_attach_thread();
    LoadAnimeImage();
    LoadTabIcons();
    ApplyTheme();
    CfgScanDir();
    CenterMenuOnDisplay();
    g_menuFadeIn = 0.f;

    while (main_thread_flag) {
        g_frame_done.store(false);
        drawBegin();

        ui::bar::set_game_alpha(0.f);
        DrawEspOverlay();
        UpdateAim(ImGui::GetIO().DeltaTime);
        UpdateFarm(ImGui::GetIO().DeltaTime);
        RenderMenu();
        drawEnd();
        g_frame_done.store(true);
    }
    while (!g_frame_done.load()) {}
    stop_attach_thread();
    if (g_esp_attached) {
        esp_reset();
        g_esp_attached = false;
    }
    Blur::Free();
    CfgWatchFree();
    AudioFree();
    shutdown(); Touch_Close(); return 0;
}
