// Проигрыватель аватарки: вшитые кадры JPEG -> текстура OpenGL -> кружок в меню.
//
// Кадры лежат в jni/src/media/avatar_video_data.inc (их готовит
// tools/video/gen_avatar.py из mp4; сейчас 128x128, 12 кадров/с — кружок в
// меню 117 px, то есть кадр показывается в натуральную величину). Формат
// кадра — обычный baseline JPEG,
// поэтому его читает маленький декодер tjpgd (ChaN, third_party/tjpgd):
// никаких кодеков платформы, никакой распаковки на диск, никакого ffmpeg.
// Вес в бинарнике — сами кадры (~300 КБ) плюс ~15 КБ кода декодера.
//
// Декодирование идёт в том же потоке, что рисует меню (текстуре нужен текущий
// контекст GL), но не чаще кадровой частоты ролика: 12 кадров в секунду по
// ~0.15 мс на кадр 128x128 (замер на x86; на телефоне ожидаемо в разы больше,
// но всё равно доли миллисекунды на кадр экрана). Один кадр ролика декодируется
// ровно один раз — если ролик отстал (меню закрывали), пропущенные кадры не
// декодируются вообще: показываем сразу нужный.

#include "VidAvatar.h"

#include <cstring>
#include <cstdint>
#include <GLES3/gl3.h>

#if __has_include("media/avatar_video.h") && __has_include("media/avatar_video_data.inc")
#  include "media/avatar_video.h"
#  include "media/avatar_video_data.inc"   // сами кадры (~300 КБ), см. gen_avatar.py
#  define VID_AVATAR_AVAILABLE 1
#endif

#ifdef VID_AVATAR_AVAILABLE
extern "C" {
#  include "third_party/tjpgd/tjpgd.h"
}
#endif

namespace VidAvatar {

#ifdef VID_AVATAR_AVAILABLE

namespace {

constexpr int kBadFrameLimit = 3;   // столько неудач подряд — и выключаемся

// Чтение одного кадра: tjpgd тянет байты из памяти, за пределы кадра не
// выходим (кадры лежат подряд в одном массиве).
struct SrcCursor {
    const unsigned char* data = nullptr;
    size_t size = 0;
    size_t pos  = 0;
};

size_t inFunc(JDEC* jd, uint8_t* buf, size_t len) {
    SrcCursor* c = static_cast<SrcCursor*>(jd->device);
    if (!c) return 0;
    size_t n = c->size - c->pos;
    if (n > len) n = len;
    // buf == nullptr — контракт tjpgd: «пропусти len байт» (так пропускаются
    // сегменты, которые декодеру не нужны).
    if (buf && n) memcpy(buf, c->data + c->pos, n);
    c->pos += n;
    return n;
}

// Кадр в RGB888: tjpgd отдаёт прямоугольники по мере разбора MCU, кладём их в
// общий буфер с шагом строки в ширину картинки.
unsigned char g_px[vid_avatar_w * vid_avatar_h * 3];

int outFunc(JDEC*, void* bitmap, JRECT* rect) {
    const unsigned char* src = static_cast<const unsigned char*>(bitmap);
    const int w = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; ++y) {
        memcpy(g_px + ((size_t)y * vid_avatar_w + rect->left) * 3,
               src + (size_t)(y - rect->top) * (size_t)w * 3, (size_t)w * 3);
    }
    return 1;
}

uint32_t g_pool[2048];        // рабочая память декодера (~2 КБ нужно, 8 КБ с запасом)
GLuint   g_tex    = 0;
double   g_clock  = 0.0;      // время ролика, с
int      g_shown  = -1;       // какой кадр сейчас в текстуре
int      g_bad    = 0;        // подряд неудачных декодирований

void upload() {
    if (!g_tex) {
        glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Ширина кадра 128x3 = 384 байта — кратно 4, поэтому выравнивание строк
        // по умолчанию подходит и трогать состояние GL не надо.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, vid_avatar_w, vid_avatar_h, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, g_px);
    } else {
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vid_avatar_w, vid_avatar_h,
                        GL_RGB, GL_UNSIGNED_BYTE, g_px);
    }
}

bool decode(int idx) {
    if (idx < 0 || idx >= vid_avatar_count) return false;
    SrcCursor c;
    c.data = vid_avatar_data + vid_avatar_off[idx];
    c.size = (size_t)(vid_avatar_off[idx + 1] - vid_avatar_off[idx]);
    JDEC jd;
    if (jd_prepare(&jd, inFunc, g_pool, sizeof(g_pool), &c) != JDR_OK) return false;
    if (jd.width != (unsigned)vid_avatar_w || jd.height != (unsigned)vid_avatar_h) return false;
    return jd_decomp(&jd, outFunc, 0) == JDR_OK;
}

}  // namespace

bool Ready() { return true; }

void Draw(ImDrawList* dl, float cx, float cy, float radius, float dt,
          ImU32 ring, ImU32 back) {
    if (!dl || !vid_avatar_count || g_bad >= kBadFrameLimit) return;

    g_clock += (double)dt;
    if (g_clock < 0.0) g_clock = 0.0;   // время ролика только вперёд
    const int want = (int)(g_clock * vid_avatar_fps) % vid_avatar_count;
    if (want != g_shown) {
        if (decode(want)) {
            upload();
            g_shown = want;
            g_bad = 0;
        } else if (++g_bad >= kBadFrameLimit) {
            return;                     // кадры не читаются — молча выключаемся
        }
    }
    if (!g_tex) return;

    // Круг: подложка чуть больше, чтобы многоугольник скругления (ImGui рисует
    // его дугами) не выглядывал за обводку; сама картинка — по размеру кружка.
    const ImVec2 c(cx, cy);
    dl->AddCircleFilled(c, radius + 1.5f, back, 64);
    dl->AddImageRounded((ImTextureID)(intptr_t)g_tex,
                        ImVec2(cx - radius, cy - radius),
                        ImVec2(cx + radius, cy + radius),
                        ImVec2(0.f, 0.f), ImVec2(1.f, 1.f),
                        IM_COL32(255, 255, 255, 255), radius);
    dl->AddCircle(c, radius + 0.75f, ring, 64, 2.f);
}

#else   // кадры не вшиты

bool Ready() { return false; }

void Draw(ImDrawList*, float, float, float, float, ImU32, ImU32) {}

#endif

}  // namespace VidAvatar
