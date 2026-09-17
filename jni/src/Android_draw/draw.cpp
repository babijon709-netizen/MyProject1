#include "Android_draw/draw.h"
#include "ImGui/Font/Font.h"
#include "Blur/Blur.h"
#include "app/media.h"
#include "app/diag_log.h"
#include "app/screen.h"
#include <dirent.h>
#include <sys/system_properties.h>
#include <time.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

EGLDisplay display = EGL_NO_DISPLAY;
EGLConfig  config;
EGLSurface surface = EGL_NO_SURFACE;
EGLContext context = EGL_NO_CONTEXT;

ANativeWindow *native_window;

int native_window_screen_x = 0;
int native_window_screen_y = 0;
anwc::ANativeWindowCreator::DisplayInfo displayInfo{0};
uint32_t orientation  = 0;
bool     g_Initialized = false;
ImGuiWindow *g_window  = nullptr;
// Темп оверлея: 60 кадров/с, а не пик дисплея.
//
// Оверлей — это своё окно (свой ANativeWindow «Surface») и свой EGL-контекст,
// vsync у него выключен (eglSwapInterval(display, 0)), поэтому без ограничения
// цикл крутился на пиковой герцовке панели: на устройстве вышло ~118 кадров/с
// при 8.5 мс на кадр, то есть ПОСТОЯННО занятое ядро и вдвое больше
// полноэкранных отрисовок, чем делает сама игра (а игра у пользователя capped
// на 60 ФПС). Показывать оверлею чаще, чем обновляется игра, всё равно нечего —
// каждый наш кадр читает память игры (syscall'ы на каждую позицию/кость) и
// рисует всю ESP-сцену заново. Отсюда и лаги: чем меньше работы на кадр (фарм
// выключен), тем больше витков в секунду и тем сильнее мы отъедаем ядро, GPU и
// композитор у игры — пользователь это и видел как «с выключенным фармом лагает
// ещё сильнее». Плюс 120 Гц на панели = вдвое больше тепла, а троттлинг на
// телефоне добивал остаток: в логе первые 90 с шли ровно, потом кадр вырос до
// 40-100 мс.
//
// Поэтому: темп жёстко 60 Гц (совпадает с частотой кадров игры), и режим
// высокой герцовки у дисплея больше не выпрашивается — игре он не нужен, а
// композитор из-за него работает вдвое чаще. g_peak_hz остаётся только для
// диагностики (пишется в шапку лога).
// ---- Живучесть окна оверлея ------------------------------------------------
//
// Оверлей — это собственная поверхность (ANativeWindow через
// SurfaceComposerClient) и собственный EGL-контекст. Поверхность может умереть
// снаружи, без нашего участия: система сменила конфигурацию дисплея (поворот,
// другое разрешение, складное устройство), панель ушла в сон/на экран
// блокировки и композитор снял слой, SurfaceFlinger прибил «висящий» слой при
// смене режима или после тяжёлого кадра. Дальше eglSwapBuffers возвращает
// EGL_FALSE, картинки нет — и в программе НЕ БЫЛО ничего, что вернуло бы её
// обратно: цикл продолжал рисовать в мёртвый контекст до перезапуска чита.
// Ровно это пользователь и описывает как «чит сам выключился через некоторое
// время» — и, что показательно, «особенно на падах»: там смена конфигурации
// (поворот, сплит-скрин, чехол) происходит чаще.
//
// Поэтому окно теперь пересобирается НА ХОДУ, между кадрами:
//   * по ошибке eglSwapBuffers (3 кадра подряд),
//   * когда размер экрана разошёлся с размером поверхности (поворот/режим).
// ImGui-контекст при этом не уничтожается: окно меню, вкладки, скроллы,
// позиция меню и шрифт остаются как были — пользователь видит максимум один
// пустой кадр. Пересборка сама логируется в журнал здоровья.
static bool   g_rebuild_wanted = false;
static char   g_rebuild_reason[160] = {};
static int    g_swap_fail_streak = 0;
static double g_last_rebuild = 0.0;
static int    g_rebuilds = 0;
static int    g_surface_w = 0, g_surface_h = 0;
static int    g_resize_attempts = 0;
static int    g_resize_want = 0;

static double NowSeconds() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static constexpr float kOverlayPaceHz = 60.f;
static float g_peak_hz = 60.f;
static float g_overlay_fps = 60.f;
static double g_prev_swap = 0.0;
static double g_pace_sleep_ms = 0.0;  // сколько последний кадр проспал до своего темпа
static int g_refresh_tick = 0;

static float SnapHz(float h) {
    const float c[] = {60.f, 90.f, 120.f, 144.f, 165.f, 180.f, 240.f};
    float best = 60.f, bd = 1e9f;
    for (float x : c) {
        float d = fabsf(h - x);
        if (d < bd) { bd = d; best = x; }
    }
    if (bd <= 8.f) return best;
    if (h >= 50.f && h <= 240.f) return h;
    return 60.f;
}

static void ConsiderHz(float& best, float h) {
    if (h >= 50.f && h <= 240.f && h > best) best = h;
}

static void ScanModesFile(float& best, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        float hz = 0.f;
        const char* p = line;
        while (*p) {
            if ((*p == '-' || *p == 'p' || *p == 'P' || *p == 'R' || *p == '@' || *p == ' ') && p[1] >= '0' && p[1] <= '9') {
                hz = strtof(p + 1, nullptr);
                ConsiderHz(best, hz);
            }
            ++p;
        }
    }
    fclose(f);
}

static float DetectPeakRefresh() {
    float best = 0.f;
    const char* props[] = {
        "persist.sys.sf.peak_refresh_rate",
        "ro.surface_flinger.max_frame_rate",
        "ro.vendor.display.default_fps",
        "persist.vendor.display.refresh_rate",
        "ro.vendor.display.refresh_rate",
        "vendor.display.refresh_rate",
        "persist.sys.display.refresh_rate",
        "ro.sf.lcd_fps"
    };
    char buf[128];
    for (const char* k : props) {
        memset(buf, 0, sizeof(buf));
        if (__system_property_get(k, buf) > 0)
            ConsiderHz(best, strtof(buf, nullptr));
    }
    DIR* d = opendir("/sys/class/drm");
    if (d) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char path[320];
            snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", e->d_name);
            ScanModesFile(best, path);
        }
        closedir(d);
    }
    ScanModesFile(best, "/sys/class/graphics/fb0/modes");
    ScanModesFile(best, "/sys/class/lcd/panel/panel_refresh_rate");
    if (best < 50.f) best = 60.f;
    return SnapHz(best);
}

static void ApplyOverlayRefresh(ANativeWindow* w, float hz) {
    if (!w) return;
    using SetFR2 = int32_t(*)(ANativeWindow*, float, int8_t, int8_t);
    using SetFR  = int32_t(*)(ANativeWindow*, float, int8_t);
    using SetBC  = int32_t(*)(ANativeWindow*, size_t);
    static SetFR2 setfr2 = (SetFR2)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRateWithChangeStrategy");
    static SetFR  setfr  = (SetFR)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
    static SetBC  setbc  = (SetBC)dlsym(RTLD_DEFAULT, "ANativeWindow_setBufferCount");
    static bool buffers_set = false;
    if (setbc && !buffers_set) { setbc(w, 4); buffers_set = true; }
    // hz <= 61 — сознательно: оверлей больше не переводит панель в 120 Гц ради
    // своих кадров (игра всё равно рисует 60, а композитор и батарея платят за
    // удвоенную герцовку). Ветка оставлена на случай, если темп когда-нибудь
    // снова поднимут выше 61 Гц.
    if (hz > 61.f) {
        if (setfr2) setfr2(w, hz, 0, 1);
        else if (setfr) setfr(w, hz, 0);
    }
    if (display != EGL_NO_DISPLAY) eglSwapInterval(display, 0);
}

float overlay_fps() {
    return g_overlay_fps;
}

// Сколько миллисекунд последний кадр проспал, дожидаясь своего темпа (60 Гц).
// В логе это разделяет «кадр тормозит из-за работы» и «кадр просто ждёт темп».
double overlay_pace_sleep_ms() {
    return g_pace_sleep_ms;
}

// Пиковая герцовка панели (диагностика): в шапку лога пишется вместе с темпом
// оверлея, чтобы 60 у оверлея не путали с ФПС игры.
float overlay_peak_hz() {
    return g_peak_hz;
}

float overlay_pace_hz() {
    return kOverlayPaceHz;
}

void draw_request_surface_rebuild(const char* why) {
    if (g_rebuild_wanted) return;
    g_rebuild_wanted = true;
    snprintf(g_rebuild_reason, sizeof(g_rebuild_reason), "%s", why ? why : "?");
}

int draw_rebuild_count() { return g_rebuilds; }

// Собрать окно и EGL заново. Зовётся только МЕЖДУ кадрами (из drawBegin):
// предыдущий eglSwapBuffers уже вернулся, в GPU ничего не «в полёте», а
// ImGui-кадр закрыт.
static bool RebuildSurface(const char* why, int want) {
    const int old = g_surface_w;

    // 1. Отпускаем GL-объекты в ТЕКУЩЕМ (старом) контексте и только потом
    //    создаём новый: glDelete* с именами из старого контекста в новом могли
    //    бы задеть чужие объекты (имена начинаются с единицы в любом контексте).
    glFinish();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    Blur::Free();

    // 2. Старое откладываем в сторону, поднимаем новое. Порядок именно такой:
    //    если собрать новое не удалось (система в этот момент не готова отдать
    //    слой), возвращаемся на старое и продолжаем рисовать — а не остаёмся с
    //    разрушенным окном до конца работы.
    EGLDisplay old_display = display;
    EGLSurface old_surface = surface;
    EGLContext old_context = context;
    ANativeWindow* old_window = native_window;
    display = EGL_NO_DISPLAY; surface = EGL_NO_SURFACE;
    context = EGL_NO_CONTEXT; native_window = nullptr;

    // Как и на старте, поверхность — квадрат по большей стороне экрана: меню
    // разложено относительно него, и эта геометрия не должна меняться на ходу.
    native_window_screen_x = native_window_screen_y = want;
    if (!init_egl((uint32_t)want, (uint32_t)want, false)) {
        if (native_window) {   // новое окно могло создаться лишь частично
            ANativeWindow_release(native_window);
            anwc::ANativeWindowCreator::Destroy(native_window);
            native_window = nullptr;
        }
        display = old_display; surface = old_surface;
        context = old_context; native_window = old_window;
        native_window_screen_x = native_window_screen_y = old;
        if (display != EGL_NO_DISPLAY && surface != EGL_NO_SURFACE)
            eglMakeCurrent(display, surface, surface, context);
        // Бэкенд ImGui и блюр были сняты выше — ставим их обратно на старом окне.
        ImGui_ImplAndroid_Init(native_window);
        ImGui_ImplOpenGL3_Init("#version 300 es");
        Blur::Init();
        diag_log("draw", "окно не пересобралось (%s, размер %d→%d) — продолжаю на старом",
                 why, old, want);
        return false;
    }

    // 3. Новый контекст уже текущий: старую поверхность и её контекст отпускаем.
    //    eglTerminate здесь НЕ зовём: EGL_DEFAULT_DISPLAY — тот же самый display,
    //    и завершение работы с ним убило бы только что созданный контекст.
    if (old_display != EGL_NO_DISPLAY) {
        if (old_context != EGL_NO_CONTEXT) eglDestroyContext(old_display, old_context);
        if (old_surface != EGL_NO_SURFACE) eglDestroySurface(old_display, old_surface);
    }
    if (old_window) {
        ANativeWindow_release(old_window);
        anwc::ANativeWindowCreator::Destroy(old_window);
    }

    ImGui_ImplAndroid_Init(native_window);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    Blur::Init();
    LoadTabIcons();

    g_surface_w = g_surface_h = want;
    ++g_rebuilds;
    diag_log("draw", "окно пересобрано: %s (размер %d→%d, всего пересборок %d)",
             why, old, want, g_rebuilds);
    return true;
}

// Вызывается в начале кадра. Возвращает true, если окно было пересобрано
// (кадр продолжается как обычно — ImGui просто создаст GL-объекты заново).
bool draw_surface_service() {
    if (!native_window) {
        // Окно потеряно (пересборка не удалась, система забрала слой, старт
        // прошёл не полностью). Раньше здесь просто возвращались — и оверлей
        // пропадал до перезапуска чита: флаг «надо пересобрать» был взведён, но
        // использовать его было некому. Пересборка создаёт окно с нуля, старое
        // ей не нужно, поэтому зовём её и отсюда — не чаще раза в 2 секунды.
        const double now = NowSeconds();
        if (now - g_last_rebuild >= 2.0) {
            int want = g_surface_w > 0 ? g_surface_w : native_window_screen_x;
            if (want <= 0) {
                const int dw = displayInfo.width, dh = displayInfo.height;
                want = (dw >= 100 && dh >= 100) ? (dw > dh ? dw : dh) : 1920;
            }
            if (!RebuildSurface("окно потеряно — создаю заново", want)) {
                g_rebuild_wanted = true;
                snprintf(g_rebuild_reason, sizeof(g_rebuild_reason), "%s", "окно потеряно");
            }
            g_last_rebuild = NowSeconds();
        }
        return false;
    }

    if (g_surface_w <= 0 || g_surface_h <= 0) {   // первый кадр: узнаём размер
        g_surface_w = ANativeWindow_getWidth(native_window);
        g_surface_h = ANativeWindow_getHeight(native_window);
        g_resize_want  = g_surface_w;
    }

    const double now = NowSeconds();
    if (now - g_last_rebuild < 2.0) return false;   // не дёргаем чаще раза в 2 с

    char reason[160] = {};
    int want = g_surface_w;
    if (g_rebuild_wanted) {
        snprintf(reason, sizeof(reason), "%s", g_rebuild_reason);
    } else {
        int dw = displayInfo.width, dh = displayInfo.height;
        if (dw >= 100 && dh >= 100) {
            want = dw > dh ? dw : dh;
            if (want != g_surface_w && (g_resize_attempts < 3 || want != g_resize_want)) {
                // Размер экрана разошёлся с размером поверхности. Если привести
                // его не удаётся (композитор отдаёт своё), после третьей попытки
                // оставляем как есть и больше не дёргаем окно зря.
                if (want != g_resize_want) { g_resize_want = want; g_resize_attempts = 0; }
                ++g_resize_attempts;
                snprintf(reason, sizeof(reason), "размер экрана %dx%d, поверхность %dx%d",
                         dw, dh, g_surface_w, g_surface_h);
            }
        }
    }
    if (!reason[0]) return false;

    const bool ok = RebuildSurface(reason, want);
    g_rebuild_wanted = false;
    g_rebuild_reason[0] = 0;
    g_last_rebuild = NowSeconds();
    if (!ok) {
        // Не удалось — окно попробует собраться на следующем круге.
        g_rebuild_wanted = true;
        snprintf(g_rebuild_reason, sizeof(g_rebuild_reason), "%s", reason);
    }
    return ok;
}

bool initGUI_draw(uint32_t _screen_x, uint32_t _screen_y, bool log) {
    orientation = displayInfo.orientation;
    if (!init_egl(_screen_x, _screen_y, log)) return false;
    if (!ImGui_init()) return false;
    return true;
}

bool init_egl(uint32_t _screen_x, uint32_t _screen_y, bool log) {
    static const char wn[] = {0x53,0x75,0x72,0x66,0x61,0x63,0x65,0x00};
    ::native_window = anwc::ANativeWindowCreator::Create(wn, _screen_x, _screen_y, false);
    ANativeWindow_acquire(native_window);

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) return false;
    if (eglInitialize(display, 0, 0) != EGL_TRUE) return false;

    EGLint num_config = 0;
    const EGLint attribList[] = {
        EGL_SURFACE_TYPE,      EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,   EGL_OPENGL_ES2_BIT,
        EGL_BLUE_SIZE,  5,
        EGL_GREEN_SIZE, 6,
        EGL_RED_SIZE,   5,
        EGL_BUFFER_SIZE, 32,
        EGL_DEPTH_SIZE,  16,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    const EGLint attrib_list[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    if (eglChooseConfig(display, attribList, &config, 1, &num_config) != EGL_TRUE) return false;

    EGLint egl_format;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &egl_format);
    ANativeWindow_setBuffersGeometry(native_window, 0, 0, egl_format);

    context = eglCreateContext(display, config, EGL_NO_CONTEXT, attrib_list);
    if (context == EGL_NO_CONTEXT) return false;

    surface = eglCreateWindowSurface(display, config, native_window, nullptr);
    if (surface == EGL_NO_SURFACE) return false;

    if (!eglMakeCurrent(display, surface, surface, context)) return false;

    eglSwapInterval(display, 0);
    g_peak_hz = DetectPeakRefresh();          // только для шапки лога/диагностики
    ApplyOverlayRefresh(::native_window, kOverlayPaceHz);

    return true;
}

void screen_config() {
    displayInfo = anwc::ANativeWindowCreator::GetDisplayInfo();
}

bool ImGui_init() {
    if (g_Initialized) return true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplAndroid_Init(native_window);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.DisplaySize = ImVec2((float)native_window_screen_x, (float)native_window_screen_y);

    io.Fonts->AddFontFromMemoryTTF(
        (void*)RobotoFont,
        (int)RobotoFont_size,
        28.0f
    );

    ImGui::GetStyle().ScaleAllSizes(1.5f);
    ImGui::GetStyle().WindowRounding = 14.0f;

    g_Initialized = true;
    return true;
}

void drawBegin() {
    screen_config();

    // Окно могло умереть снаружи (см. шапку про живучесть окна) — до NewFrame
    // самое безопасное место, чтобы собрать его заново.
    draw_surface_service();

    // Геометрия меню/проекции считается от displayInfo (VisibleScreen), но
    // «бумажные» размеры экрана держим в согласии с дисплеем: по ним потом
    // сверяется следующая пересборка окна.
    if (displayInfo.width >= 100 && displayInfo.height >= 100) {
        float mx = (float)(displayInfo.width > displayInfo.height ? displayInfo.width : displayInfo.height);
        float mn = (float)(displayInfo.width < displayInfo.height ? displayInfo.width : displayInfo.height);
        if (g_sw != mx || g_sh != mn) { g_sw = mx; g_sh = mn; }
    }

    if ((++g_refresh_tick % 180) == 0)
        ApplyOverlayRefresh(::native_window, kOverlayPaceHz);

    if (::orientation != displayInfo.orientation) {
        ::orientation = displayInfo.orientation;
        UpdateScreenData(displayInfo.width, displayInfo.height, displayInfo.orientation);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    {
        float sx = (float)native_window_screen_x;
        float a = (float)displayInfo.width;
        float b = (float)displayInfo.height;
        if (a >= 100.f && b >= 100.f) sx = a > b ? a : b;
        if (sx < 100.f) sx = 1080.f;
        ImGui::GetIO().DisplaySize = ImVec2(sx, sx);
    }
    ImGui::NewFrame();
}

void drawEnd() {
    ImGui::Render();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (eglSwapBuffers(display, surface) != EGL_TRUE) {
        const EGLint err = eglGetError();
        // Кадр не дошёл до экрана. Один такой случай — обычное дело (панель
        // уснула, композитор замер); три подряд — поверхность мертва, и её надо
        // собрать заново, иначе чит «выключится» навсегда.
        if (++g_swap_fail_streak >= 3) {
            g_swap_fail_streak = 0;
            char why[160];
            snprintf(why, sizeof(why), "кадр не показывается 3 раза подряд (EGL 0x%x)", (unsigned)err);
            draw_request_surface_rebuild(why);
            diag_log("draw", "%s — назначаю пересборку окна", why);
        }
    } else {
        g_swap_fail_streak = 0;
    }

    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
    // Темп — фиксированные 60 Гц (см. kOverlayPaceHz): раньше здесь стоял пик
    // панели (120), и оверлей крутился вдвое чаще игры. Сон учитывается
    // отдельно и уходит в лог: по нему видно, кадр упирается в работу (сон ~0)
    // или в темп (сон ~8 мс при 60 Гц).
    const double pace = (double)kOverlayPaceHz;
    double slept = 0.0;
    if (g_prev_swap > 0.0) {
        double target = 1.0 / pace;
        double elapsed = now - g_prev_swap;
        if (elapsed > 0.0 && elapsed < target - 0.00035) {
            double sl = target - elapsed;
            struct timespec req{};
            req.tv_sec = (time_t)sl;
            req.tv_nsec = (long)((sl - (double)req.tv_sec) * 1000000000.0);
            clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const double after = (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
            slept = after - now;
            now = after;
        }
    }
    g_pace_sleep_ms = slept * 1000.0;
    if (g_prev_swap > 0.0) {
        float d = (float)(now - g_prev_swap);
        if (d > 0.0002f && d < 0.25f)
            g_overlay_fps += (1.f / d - g_overlay_fps) * 0.12f;
    }
    g_prev_swap = now;
}

void shutdown() {
    if (!g_Initialized) return;
    g_surface_w = g_surface_h = 0;
    g_swap_fail_streak = 0;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();

    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    display = EGL_NO_DISPLAY;
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;

    ANativeWindow_release(native_window);
    anwc::ANativeWindowCreator::Destroy(native_window);
    g_Initialized = false;
}