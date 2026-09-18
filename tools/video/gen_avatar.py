#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Готовит кадры аватарки (кружок слева сверху в меню) из mp4 в исходники.

Видео НЕ кладётся в бинарник целиком: кадры ужимаются до размера, в котором их
видно (в меню кружок ~88 px), и кодируются MJPEG. Один кадр 96x96 стоит ~1.2 КБ,
весь ролик (21 с, 12 кадров/с) — ~300 КБ, и он читается встроенным
декодером JPEG (jni/src/third_party/tjpgd) прямо из памяти, без кодеков
MediaCodec/ffmpeg и без распаковки кадров на диск.

Как считается кроп: ролик снимался с белым фоном, поэтому кадр обрезается по
не-белым пикселям (спрайт + его тень) и берётся квадрат по центру содержимого —
так спрайт занимает всю окружность, без полей.

    python3 tools/video/gen_avatar.py [видео] [--size 96] [--fps 12] [--q 9]
                                     [--out jni/src/media/avatar_video_data.inc]

ffmpeg ищется в $FFMPEG, в PATH и в пакете imageio-ffmpeg (pip install imageio-ffmpeg).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def find_ffmpeg():
    for cand in (os.environ.get("FFMPEG"), shutil.which("ffmpeg")):
        if cand and os.path.exists(cand):
            return cand
    try:
        import imageio_ffmpeg  # type: ignore
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        pass
    sys.exit("ffmpeg не найден: поставьте его в PATH, укажите $FFMPEG "
             "или поставьте пакет imageio-ffmpeg")


def probe(ffmpeg, src):
    p = subprocess.run([ffmpeg, "-hide_banner", "-i", src],
                       capture_output=True, text=True)
    out = p.stderr
    dur = None
    for line in out.splitlines():
        if "Duration:" in line:
            hh, mm, ss = line.split("Duration:")[1].split(",")[0].strip().split(":")
            dur = int(hh) * 3600 + int(mm) * 60 + float(ss)
    return dur


def content_box(ffmpeg, src, probe_fps=6, white=245, step=2, work=192):
    """Границы не-белого содержимого по кадрам (белый фон ролика)."""
    p = subprocess.run(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", src,
         "-vf", f"fps={probe_fps},scale={work}:-1", "-pix_fmt", "rgb24",
         "-f", "rawvideo", "-"], capture_output=True)
    data = p.stdout
    n = 0
    x0 = y0 = 10 ** 9
    x1 = y1 = -1
    # ширина/высота уменьшенного кадра — как у видео, но не больше work
    fw = work
    fh = int(round(work * 688 / 720))     # пропорция исходника; уточняется ниже
    if n == 0:
        # уточняем по фактическому размеру кадра
        p2 = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", src,
                             "-vf", "scale=192:-1", "-frames:v", "1", "-pix_fmt", "rgb24",
                             "-f", "rawvideo", "-"], capture_output=True)
        fw = 192
        fh = len(p2.stdout) // (fw * 3)
    fs = fw * fh * 3
    for i in range(len(data) // fs):
        f = data[i * fs:(i + 1) * fs]
        n += 1
        for y in range(0, fh, step):
            row = f[y * fw * 3:(y + 1) * fw * 3]
            for x in range(0, fw, step):
                o = x * 3
                if row[o] < white or row[o + 1] < white or row[o + 2] < white:
                    if x < x0:
                        x0 = x
                    if x > x1:
                        x1 = x
                    if y < y0:
                        y0 = y
                    if y > y1:
                        y1 = y
    if x1 < 0:
        return None
    print(f"  не-белое содержимое: {n} кадров-проб, "
          f"x {x0}..{x1}, y {y0}..{y1} (из {fw}x{fh})")
    # переводим в координаты исходника
    sx = 720.0 / fw
    sy = (fh and (688.0 / fh)) or sx
    return (x0 * sx, y0 * sy, (x1 + step) * sx, (y1 + step) * sy)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video", nargs="?")
    ap.add_argument("--size", type=int, default=96, help="сторона кадра, px")
    ap.add_argument("--fps", type=int, default=12, help="кадров в секунду")
    ap.add_argument("--q", type=int, default=9, help="качество MJPEG (1..31)")
    ap.add_argument("--out", default=os.path.join(ROOT, "jni/src/media/avatar_video_data.inc"))
    ap.add_argument("--hdr", default=os.path.join(ROOT, "jni/include/media/avatar_video.h"))
    args = ap.parse_args()

    src = args.video
    if not src:
        cands = [f for f in os.listdir(ROOT)
                 if f.lower().endswith((".mp4", ".mov", ".mkv", ".webm"))]
        if not cands:
            sys.exit("в корне репозитория нет видеофайла — укажите путь")
        src = os.path.join(ROOT, sorted(cands)[0])
    if not os.path.exists(src):
        sys.exit(f"нет файла {src}")

    ffmpeg = find_ffmpeg()
    dur = probe(ffmpeg, src)
    print(f"источник: {os.path.basename(src)} ({dur:.1f} с)")

    box = content_box(ffmpeg, src)
    if box:
        bx0, by0, bx1, by1 = box
        cx, cy = (bx0 + bx1) / 2.0, (by0 + by1) / 2.0
        side = max(bx1 - bx0, by1 - by0)
        side = min(side * 1.04, 688.0)          # чуть воздуха по краям
        left = max(0.0, min(cx - side / 2.0, 720.0 - side))
        top = max(0.0, min(cy - side / 2.0, 688.0 - side))
        crop = f"crop={int(round(side))}:{int(round(side))}:{int(round(left))}:{int(round(top))}"
    else:
        crop = "crop=688:688:16:0"
    print(f"  кроп: {crop}")

    tmp = tempfile.mkdtemp(prefix="avatar_")
    try:
        vf = f"{crop},scale={args.size}:{args.size}:flags=lanczos,fps={args.fps}"
        cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", src,
               "-vf", vf, "-q:v", str(args.q), "-f", "image2",
               os.path.join(tmp, "%04d.jpg")]
        subprocess.run(cmd, check=True)
        frames = sorted(os.listdir(tmp))
        if not frames:
            sys.exit("ffmpeg не выдал ни одного кадра")
        blobs = []
        for f in frames:
            b = open(os.path.join(tmp, f), "rb").read()
            if not b.startswith(b"\xff\xd8"):
                sys.exit(f"кадр {f} не JPEG")
            blobs.append(b)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    total = sum(len(b) for b in blobs)
    print(f"кадров: {len(blobs)}, {args.size}x{args.size}, {args.fps} кадр/с, "
          f"q{args.q} → {total/1024:.0f} КБ ({total//len(blobs)} Б/кадр)")

    offs = [0]
    for b in blobs:
        offs.append(offs[-1] + len(b))

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("// Сгенерировано tools/video/gen_avatar.py — не править руками.\n")
        f.write(f"// Источник: {os.path.basename(src)}, {crop}, {args.size}x{args.size}, "
                f"{args.fps} кадр/с, q{args.q}.\n")
        f.write("// Каждый кадр — обычный JPEG (baseline); читается tjpgd прямо из памяти.\n")
        f.write("// extern: у const-объявления в C++ иначе внутренняя компоновка\n")
        f.write("extern const unsigned int vid_avatar_frames = %d;\n" % len(blobs))
        f.write("extern const unsigned int vid_avatar_off[%d] = {\n" % (len(blobs) + 1))
        for i in range(0, len(offs), 16):
            f.write("    " + ", ".join(str(v) for v in offs[i:i + 16]) + ",\n")
        f.write("};\n")
        f.write("extern const unsigned char vid_avatar_data[%d] = {\n" % total)
        line = []
        for b in blobs:
            for v in b:
                line.append("0x%02x" % v)
                if len(line) == 24:
                    f.write("    " + ",".join(line) + ",\n")
                    line = []
        if line:
            f.write("    " + ",".join(line) + ",\n")
        f.write("};\n")

    with open(args.hdr, "w", encoding="utf-8") as f:
        f.write("// Сгенерировано tools/video/gen_avatar.py — не править руками.\n")
        f.write("#ifndef MEDIA_AVATAR_VIDEO_H\n#define MEDIA_AVATAR_VIDEO_H\n\n")
        f.write("// Кадры аватарки: %d JPEG %dx%d, %d кадр/с, %u Б на всё видео "
                "(ролик НЕ вшит целиком, см. tools/video/gen_avatar.py).\n"
                % (len(blobs), args.size, args.size, args.fps, total))
        f.write("static const int vid_avatar_w = %d;\n" % args.size)
        f.write("static const int vid_avatar_h = %d;\n" % args.size)
        f.write("static const int vid_avatar_fps = %d;\n" % args.fps)
        f.write("static const int vid_avatar_count = %d;\n" % len(blobs))
        f.write("static const unsigned int vid_avatar_bytes = %u;\n" % total)
        f.write("extern const unsigned int vid_avatar_frames;\n")
        f.write("extern const unsigned int vid_avatar_off[%d];\n" % (len(blobs) + 1))
        f.write("extern const unsigned char vid_avatar_data[%d];\n" % total)
        f.write("\n#endif  // MEDIA_AVATAR_VIDEO_H\n")

    inc = os.path.getsize(args.out)
    print(f"{args.out}: {inc/1024:.0f} КБ текста")
    print(f"{args.hdr}: объявления")
    print(f"итого в бинарнике: +{total/1024:.0f} КБ данных, +~15 КБ кода "
          f"(декодер tjpgd)")


if __name__ == "__main__":
    main()
