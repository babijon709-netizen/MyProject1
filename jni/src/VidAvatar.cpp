// Проигрыватель аватарки: вшитые кадры JPEG -> ДЕКОД -> ASCII-АРТ -> кружок.
//
// Кадры лежат в jni/src/media/avatar_video_data.inc (их готовит
// tools/video/gen_avatar.py из mp4; 176x176, 12 кадров/с). Формат кадра —
// обычный baseline JPEG, его читает маленький декодер tjpgd (ChaN,
// third_party/tjpgd): никаких кодеков платформы и никакого ffmpeg.
//
// Раз декодированный кадр всё равно лежит в памяти как RGB-растр, кружок
// рисуется ИЗ СИМВОЛОВ: растр делится на сетку знакомест (моноширинный
// шрифт 14 px, тот же TermMono, что и во всём гуи), каждая ячейка
// превращается в символ по яркости. Вертикальное разрешение удвоено
// полу-блоками: верх и низ ячейки сравниваются отдельно, «▀▄█» закрывают
// комбинации «ярко/ярко», тёмные ячейки идут по лестнице « .:-=+*#%@».
// Ячейки вне круга — пробел: анимация сама складывается в круг, как
//.ascii-арт в терминале. Цвет — монохром «фосфор» (ring из темы).
//
// Декод идёт в потоке отрисовки меню, но не чаще кадровой частоты ролика:
// 12 раз в секунду по ~0.3 мс на кадр 176x176 плюс ~0.1 мс на сборку
// ASCII-сетки. Текстура OpenGL больше не нужна вовсе: рисуют AddText'ы
// (2 на строку сетки), поэтому путь GL в горячем цикле исчез.

#include "VidAvatar.h"
#include "ascii.h"   // SmallFont/Cell — малый кегль TermMono

#include <cstring>
#include <cstdint>

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

// Кегль ASCII-арта. Малый шрифт запечён в атласе именно в 14 px (draw.cpp),
// поэтому глифы выходят 1:1, без масштабирования — crisp, как в терминале.
constexpr float kArtFS = 14.f;

// Сетка: максимум под кружок ~радиуса 120 px (диаметр 240 -> 14 строк по
// 16.4 px); запас вдвое сверх того — на растянутые окна.
constexpr int kMaxCols = 56;
constexpr int kMaxRows = 30;

// Два слоя строки: яркие символы (блоки ▀▄█ и хвост лестницы) и тёмные
// (лестница « .:-=+*#%» в полупрозрачном цвете). Рисуется два AddText на
// строку — пустые позиции пробелы, квадов лишних не плодим.
char g_lo[kMaxRows][kMaxCols * 4 + 1];
char g_hi[kMaxRows][kMaxCols * 4 + 1];
int   g_cols = 0, g_rows = 0;
float g_gridR = -1.f;   // радиус, под который собрана сетка

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
double   g_clock = 0.0;       // время ролика, с
int      g_shown = -1;        // какой кадр сейчас в буфере
int      g_bad   = 0;         // подряд неудачных декодирований

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

// Средняя яркость прямоугольника кадра (Rec.601), 0..9.
inline int LumLvl(int sx0, int sy0, int sx1, int sy1) {
    long sum = 0;
    int  n = 0;
    for (int y = sy0; y < sy1; y += 2) {
        const unsigned char* p = g_px + ((size_t)y * vid_avatar_w + sx0) * 3;
        for (int x = sx0; x < sx1; x += 2, p += 6) {
            sum += (p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8;
            ++n;
        }
    }
    if (!n) return 0;
    return (int)((float)sum / (float)n / 255.f * 9.99f);
}

// Собрать ASCII-сетку из g_px под кружок радиуса R. Ячейки вне круга —
// пробелы: видео само «вырезается» в кружок, без клип-масок.
void BuildGrid(float R) {
    ImFont* fn = ascii::SmallFont();
    if (!fn) { g_cols = 0; g_rows = 0; g_gridR = -1.f; return; }

    const ImVec2 cell = ascii::Cell(fn, kArtFS);
    const float d = R * 2.f;
    int rows = (int)(d / cell.y);
    int cols = (int)(d / cell.x);
    if (rows < 3 || cols < 3) { g_cols = 0; g_rows = 0; g_gridR = -1.f; return; }
    if (rows > kMaxRows) rows = kMaxRows;
    if (cols > kMaxCols) cols = kMaxCols;

    // Яркость 4/10 — порог «блока»: верх/низ ячейки ярче порога закрываем
    // блоками, темнее — лестницей оттенков.
    constexpr int kThresh = 4;
    const char* kRamp = " .:-=+*#%@";

    for (int r = 0; r < rows; ++r) {
        size_t pl = 0, ph = 0;
        const int sy0 = (int)((float)r * vid_avatar_h / rows);
        const int sy1 = (int)((float)(r + 1) * vid_avatar_h / rows);
        const int syM = (sy0 + sy1) / 2;
        for (int c = 0; c < cols; ++c) {
            // Центр ячейки в долях радиуса: круг чуть меньше, чтобы арт не
            // упирался в обводку.
            const float fx = ((float)c + 0.5f) / (float)cols * 2.f - 1.f;
            const float fy = ((float)r + 0.5f) / (float)rows * 2.f - 1.f;
            const bool inCircle = (fx * fx + fy * fy) < 0.86f;

            if (!inCircle) {
                g_lo[r][pl++] = ' ';
                g_hi[r][ph++] = ' ';
                continue;
            }

            const int sx0 = (int)((float)c * vid_avatar_w / cols);
            const int sx1 = (int)((float)(c + 1) * vid_avatar_w / cols);
            const int lt = LumLvl(sx0, sy0, sx1, syM);
            const int lb = LumLvl(sx0, syM, sx1, sy1);

            const char* glyph = nullptr;
            bool bright = false;
            if (lt >= kThresh && lb >= kThresh) { glyph = "\xE2\x96\x88"; bright = true; }  // █
            else if (lt >= kThresh)             { glyph = "\xE2\x96\x80"; bright = true; }  // ▀
            else if (lb >= kThresh)             { glyph = "\xE2\x96\x84"; bright = true; }  // ▄
            else {
                const int m = lt > lb ? lt : lb;
                if (m >= 1) glyph = &kRamp[m];   // тёмная лестница
                else        glyph = " ";
            }

            if (bright) {
                g_lo[r][pl++] = ' ';
                memcpy(g_hi[r] + ph, glyph, strlen(glyph)); ph += strlen(glyph);
            } else {
                memcpy(g_lo[r] + pl, glyph, strlen(glyph)); pl += strlen(glyph);
                g_hi[r][ph++] = ' ';
            }
        }
        g_lo[r][pl] = '\0';
        g_hi[r][ph] = '\0';
    }

    g_cols = cols;
    g_rows = rows;
    g_gridR = R;
}

}  // namespace

bool Ready() { return true; }

void Draw(ImDrawList* dl, float cx, float cy, float radius, float dt,
          ImU32 ring, ImU32 back) {
    if (!dl || !vid_avatar_count || g_bad >= kBadFrameLimit) return;

    g_clock += (double)dt;
    if (g_clock < 0.0) g_clock = 0.0;   // время ролика только вперёд
    const int want = (int)(g_clock * vid_avatar_fps) % vid_avatar_count;
    bool have = g_shown >= 0;
    if (want != g_shown) {
        if (decode(want)) {
            g_shown = want;
            g_bad   = 0;
            have    = true;
        } else if (++g_bad >= kBadFrameLimit) {
            return;                     // кадры не читаются — молча выключаемся
        }
    }

    // Кружок: подложка чуть больше обводки, чтобы кольцо не просвечивало.
    const ImVec2 c(cx, cy);
    dl->AddCircleFilled(c, radius + 1.5f, back, 48);

    // Окно растянули — пересобрать сетку под новый радиус (кадр уже в буфере).
    if (have && (g_gridR != radius || g_cols == 0)) BuildGrid(radius);

    ImFont* fn = ascii::SmallFont();
    if (fn && g_cols > 0 && g_rows > 0) {
        const ImVec2 cell = ascii::Cell(fn, kArtFS);
        const float x0 = cx - g_cols * cell.x * 0.5f;
        const float y0 = cy - g_rows * cell.y * 0.5f;
        // Два слоя: тёмная лестница — тот же «фосфор», но вдвое прозрачнее.
        const unsigned ra = (unsigned)(((ring >> IM_COL32_A_SHIFT) & 0xFF) * 0.45f);
        const ImU32 dim = (ring & ~IM_COL32_A_MASK) | (ra << IM_COL32_A_SHIFT);
        for (int r = 0; r < g_rows; ++r) {
            const ImVec2 p{x0, y0 + cell.y * (float)r};
            dl->AddText(fn, kArtFS, p, dim, g_lo[r]);
            dl->AddText(fn, kArtFS, p, ring, g_hi[r]);
        }
    }

    dl->AddCircle(c, radius + 0.75f, ring, 48, 2.f);
}

#else   // кадры не вшиты

bool Ready() { return false; }

void Draw(ImDrawList*, float, float, float, float, ImU32, ImU32) {}

#endif

}  // namespace VidAvatar
