#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Генератор презентации «Права и общие обязанности военнослужащих в РФ».

Только фигуры PowerPoint (прямомоугольники, линии, текст): без растровых
изображений, без иконок и без AI-графики. Шрифт — Arial.

Запуск:
    python3 pptx_generator/build_pptx.py
    OUT=/path/file.pptx python3 pptx_generator/build_pptx.py   # свой путь
"""

import os
import re

from PIL import ImageFont
from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.util import Emu, Inches, Pt

# ----------------------------------------------------------------------------
# Палитра, шрифт
# ----------------------------------------------------------------------------
NAVY = RGBColor(0x0E, 0x25, 0x45)
NAVY_2 = RGBColor(0x18, 0x3A, 0x66)
NAVY_3 = RGBColor(0x24, 0x4E, 0x84)
GOLD = RGBColor(0xC2, 0x9A, 0x3E)
GOLD_SOFT = RGBColor(0xE4, 0xCE, 0x9A)
INK = RGBColor(0x1B, 0x2A, 0x3A)
GRAY_TXT = RGBColor(0x44, 0x53, 0x64)
GRAY_MID = RGBColor(0x6B, 0x7B, 0x8C)
GRAY_LT = RGBColor(0xB9, 0xC4, 0xCF)
TINT = RGBColor(0xF3, 0xF6, 0xF9)
TINT_2 = RGBColor(0xEA, 0xEF, 0xF5)
TINT_3 = RGBColor(0xF7, 0xF9, 0xFB)
LINE_LT = RGBColor(0xDD, 0xE4, 0xEC)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)
ON_DARK = RGBColor(0xC8, 0xD6, 0xE4)

FONT = "Arial"
_MEAS_R = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
_MEAS_B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
_PXPT = 4 / 3          # 96 dpi
_ARIAL_K = 0.905       # DejaVu -> Arial (узел: считаем по Arial-метрик)

# ----------------------------------------------------------------------------
# Геометрия слайда, дюймы
# ----------------------------------------------------------------------------
SW, SH = 13.3333, 7.5
STRIP_W = 0.115
ML, MR = 0.86, 0.62
CW = SW - ML - MR
MT = 1.62
BOTTOM_LIMIT = 6.92
FOOTER_RULE_Y = 7.03
GUTTER = 0.24
CARD_PAD = 0.21
CARD_HEAD_H = 0.14

# кегли
SZ_ITEM = 10.8
SZ_ITEM_LH = 0.176
SZ_CARD_H = 12.0
SZ_CARD_H_LH = 0.205
SZ_PANEL = 12.5
SZ_PANEL_LH = 0.225
SZ_SUB = 12.5
SZ_SUB_LH = 0.225
GAP_ITEM = 0.052

OUT = os.environ.get(
    "OUT", "/home/user/MyProject1/Prava_i_obazannosti_voennosluzhashchih.pptx")

_fc = {}


def _font(pt, bold):
    k = (round(pt, 2), bold)
    if k not in _fc:
        _fc[k] = ImageFont.truetype(_MEAS_B if bold else _MEAS_R,
                                    max(4, int(round(pt * _PXPT))))
    return _fc[k]


def text_w_pt(s, pt, bold=False):
    if not s:
        return 0.0
    return _font(pt, bold).getlength(s) / _PXPT * _ARIAL_K


BOLD_RE = re.compile(r"\*\*(.+?)\*\*")


def segments(text):
    """'**bold**' -> [(фрагмент, bold), ...]"""
    out, pos = [], 0
    for m in BOLD_RE.finditer(text):
        if m.start() > pos:
            out.append((text[pos:m.start()], False))
        out.append((m.group(1), True))
        pos = m.end()
    if pos < len(text):
        out.append((text[pos:], False))
    return out or [(text, False)]


def styled_wrap(text, pt, width_in):
    """Перенос по словам с сохранением **выделений**."""
    segs = []
    for chunk, b in segments(text):
        for w in re.findall(r"\S+", chunk):
            segs.append((w, b))
    width_pt = width_in * 72.0
    lines, cur = [], []

    def wline(ws):
        return sum(text_w_pt(t, pt, b) for t, b in ws) + text_w_pt(" ", pt) * (len(ws) - 1)

    for item in segs:
        trial = cur + [item]
        if wline(trial) <= width_pt or not cur:
            cur = trial
        else:
            lines.append(cur)
            cur = [item]
    if cur:
        lines.append(cur)
    return lines


def wrap_lines(text, pt, width_in):
    return [" ".join(t for t, _ in ln) for ln in styled_wrap(text, pt, width_in)]


def n_lines(text, pt, width_in):
    return len(styled_wrap(text, pt, width_in))


# ----------------------------------------------------------------------------
# Низкоуровневые помощники
# ----------------------------------------------------------------------------
AUDIT = []      # (номер слайда, имя, нижняя граница)
OVER = []       # переполнения текста внутри карточек


def rect(slide, x, y, w, h, fill=None, line=None, line_w=0.75,
         shape=MSO_SHAPE.RECTANGLE, name="shape"):
    sp = slide.shapes.add_shape(shape, Inches(x), Inches(y), Inches(w), Inches(h))
    sp.shadow.inherit = False
    sp.name = name
    if fill is None:
        sp.fill.background()
    else:
        sp.fill.solid()
        sp.fill.fore_color.rgb = fill
    if line is None:
        sp.line.fill.background()
    else:
        sp.line.color.rgb = line
        sp.line.width = Pt(line_w)
    tf = sp.text_frame
    tf.word_wrap = True
    tf.margin_left = tf.margin_right = tf.margin_top = tf.margin_bottom = 0
    sp.text_frame.paragraphs[0].alignment = PP_ALIGN.LEFT
    AUDIT.append((name, y + h))
    return sp


def textbox(slide, x, y, w, h, anchor=MSO_ANCHOR.TOP, align=PP_ALIGN.LEFT, name="txt"):
    tb = slide.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tb.name = name
    tf = tb.text_frame
    tf.word_wrap = True
    tf.margin_left = tf.margin_right = tf.margin_top = tf.margin_bottom = 0
    tf.vertical_anchor = anchor
    tf.paragraphs[0].alignment = align
    return tb, tf


def para(tf, first=False, line=1.16):
    p = tf.paragraphs[0] if first else tf.add_paragraph()
    p.alignment = PP_ALIGN.LEFT
    p.space_before = Pt(0)
    p.space_after = Pt(0)
    if line:
        p.line_spacing = line
    return p


def run(p, text, size, color, bold=False, italic=False, caps=False, spc=None):
    r = p.add_run()
    r.text = text.upper() if caps else text
    f = r.font
    f.size = Pt(size)
    f.bold = bold
    f.italic = italic
    f.name = FONT
    f.color.rgb = color
    if spc is not None:
        r._r.get_or_add_rPr().set("spc", str(int(spc * 100)))
    return r


def styled_para(tf, text, size, color, first=False, bold=False, italic=False,
                line=1.16, align=PP_ALIGN.LEFT):
    p = para(tf, first=first, line=line)
    p.alignment = align
    for chunk, b in segments(text):
        run(p, chunk, size, color, bold=(bold or b), italic=italic)
    return p


# ----------------------------------------------------------------------------
# Высокоуровневые блоки
# ----------------------------------------------------------------------------
def card_h(blk, inner_w):
    h = CARD_HEAD_H + 0.05 + n_lines(blk["h"], SZ_CARD_H, inner_w) * SZ_CARD_H_LH + 0.075
    for it in blk["items"]:
        h += n_lines(it, SZ_ITEM, inner_w - 0.20) * SZ_ITEM_LH + GAP_ITEM
    return h + 0.17


def draw_card(slide, x, y, w, h, blk, head_fill=NAVY, body_fill=TINT):
    rect(slide, x, y, w, h, fill=body_fill, line=LINE_LT, line_w=0.75, name="card")
    rect(slide, x, y, w, CARD_HEAD_H, fill=head_fill)
    tx, tw = x + CARD_PAD, w - 2 * CARD_PAD
    _, tf = textbox(slide, tx, y + CARD_HEAD_H + 0.05, tw,
                    0.30 * n_lines(blk["h"], SZ_CARD_H, tw) + 0.10)
    styled_para(tf, blk["h"], SZ_CARD_H, NAVY, first=True, bold=True, line=1.06)
    yy = y + CARD_HEAD_H + 0.05 + n_lines(blk["h"], SZ_CARD_H, tw) * SZ_CARD_H_LH + 0.075
    for it in blk["items"]:
        lines = styled_wrap(it, SZ_ITEM, tw - 0.20)
        _, tf = textbox(slide, tx + 0.20, yy, tw - 0.20, 0.30 * len(lines) + 0.10)
        for i, ln in enumerate(lines):
            p = para(tf, first=(i == 0), line=1.14)
            if i == 0:
                run(p, "—  ", SZ_ITEM, GOLD, bold=True)
            for j, (word, b) in enumerate(ln):
                run(p, word + (" " if j < len(ln) - 1 else ""), SZ_ITEM,
                    INK if b else GRAY_TXT, bold=b)
        yy += len(lines) * SZ_ITEM_LH + GAP_ITEM
    if yy - GAP_ITEM > y + h + 0.03:
        OVER.append((round(yy - y, 2), round(h, 2), blk["h"]))


def flow_cards(slide, blocks, y, cols=2, head_fill=NAVY, body_fill=TINT, reserve=0.0):
    """Карточки строками; автоцентрирование и автоподгонка по высоте слайда."""
    colw = (CW - GUTTER * (cols - 1)) / cols
    xs = [ML + i * (colw + GUTTER) for i in range(cols)]
    inner = colw - 2 * CARD_PAD
    hs = [card_h(b, inner) for b in blocks]
    rows = [hs[i:i + cols] for i in range(0, len(hs), cols)]
    used = sum(max(r) for r in rows) + 0.18 * len(rows)
    avail = BOTTOM_LIMIT - reserve - y
    if used > avail and used > 0:                      # не влезают — подрезаем
        k = max(0.82, avail / used)
        hs = [h * k for h in hs]
        rows = [hs[i:i + cols] for i in range(0, len(hs), cols)]
        used = sum(max(r) for r in rows) + 0.18 * len(rows)
    free = BOTTOM_LIMIT - reserve - y - used           # пустоту — в равномерные отступы
    if free > 0.28:
        y += free * 0.45
    yy = y
    bottom = y
    idx = 0
    for ri, row in enumerate(rows):
        hh = max(row)
        n = len(row)
        if ri == len(rows) - 1 and n < cols:            # неполный ряд — по центру
            total_w = colw * n + GUTTER * (n - 1)
            x0 = ML + (CW - total_w) / 2
        else:
            x0 = ML
        for j in range(n):
            draw_card(slide, x0 + j * (colw + GUTTER), yy, colw, hh,
                      blocks[idx + j], head_fill=head_fill, body_fill=body_fill)
        bottom = yy + hh
        yy = bottom + 0.18
        idx += n
    return bottom


def bullets_panel(slide, y, items, size=SZ_PANEL, reserve=0.0):
    """Один широкий список в плашке (для слайда с общими обязанностями)."""
    pad = CARD_PAD
    inner_w = CW - 2 * pad - 0.28
    hs = [n_lines(it, size, inner_w) * SZ_PANEL_LH for it in items]
    gap = 0.115
    total = pad + sum(hs) + gap * (len(items) - 1) + pad
    avail = BOTTOM_LIMIT - reserve - y
    if total > avail:
        k = max(0.86, (avail - 2 * pad - gap * (len(items) - 1)) / sum(hs))
        hs = [h * k for h in hs]
        total = pad + sum(hs) + gap * (len(items) - 1) + pad
    free = BOTTOM_LIMIT - reserve - y - total
    if free > 0.28:
        y += free * 0.45
    rect(slide, ML, y, CW, total, fill=TINT, line=LINE_LT, line_w=0.75, name="panel")
    yy = y + pad
    for it, hh in zip(items, hs):
        lines = wrap_lines(it, size, inner_w)
        _, tf = textbox(slide, ML + pad + 0.28, yy, inner_w, 0.30 * len(lines) + 0.10)
        for i, ln in enumerate(lines):
            p = para(tf, first=(i == 0), line=1.14)
            if i == 0:
                run(p, "—  ", size, GOLD, bold=True)
            for chunk, b in segments(ln):
                run(p, chunk, size, INK if b else GRAY_TXT, bold=b)
        yy += hh + gap
    return y + total


def note_strip(slide, y, text, label=None, size=10.5):
    lab = (label + "     ") if label else ""
    w = CW - 0.60
    lines = wrap_lines(lab + text, size, w)
    lh = size * 1.42 / 72.0
    hh = lh * len(lines) + 0.24
    rect(slide, ML, y, CW, hh, fill=TINT_2, name="note")
    rect(slide, ML, y, 0.055, hh, fill=GOLD)
    _, tf = textbox(slide, ML + 0.30, y + 0.12, w, hh, anchor=MSO_ANCHOR.MIDDLE)
    for i, ln in enumerate(lines):
        p = para(tf, first=(i == 0), line=1.18)
        if i == 0 and label:
            run(p, label, size, NAVY, bold=True, caps=True, spc=0.8)
            run(p, "     ", size, NAVY)
            ln = ln[len(label) + 5:] if ln.startswith(label) else ln
        for chunk, b in segments(ln):
            run(p, chunk, size, GRAY_TXT, bold=b)
    return hh


def note_strip_h(text, label=None, size=10.5):
    txt = ("" if label is None else label + "     ") + text
    return size * 1.42 / 72.0 * len(wrap_lines(txt, size, CW - 0.60)) + 0.24


def ref_strip_h(refs, size=10.5):
    return size * 1.35 / 72.0 * sum(n_lines(r, size, CW - 2 * CARD_PAD) for r in refs) + 0.42


def ref_strip(slide, y, refs, size=10.5, label="ПРАВОВАЯ БАЗА"):
    hh = ref_strip_h(refs, size)
    rect(slide, ML, y, CW, hh, fill=TINT_3, line=LINE_LT, line_w=0.75, name="note")
    _, tf = textbox(slide, ML + CARD_PAD, y + 0.14, 2.4, 0.24)
    p = para(tf, first=True)
    run(p, label, 9.5, NAVY, bold=True, spc=1.2)
    yy = y + 0.14 + 0.26
    for r in refs:
        lines = styled_wrap(r, size, CW - 2 * CARD_PAD)
        _, tf = textbox(slide, ML + CARD_PAD, yy, CW - 2 * CARD_PAD,
                        0.22 * len(lines) + 0.06)
        for i, ln in enumerate(lines):
            p = para(tf, first=(i == 0), line=1.16)
            if i == 0:
                run(p, "§  ", size, GOLD, bold=True)
            for j, (word, b) in enumerate(ln):
                run(p, word + (" " if j < len(ln) - 1 else ""), size, GRAY_TXT, bold=b)
        yy += size * 1.35 / 72.0 * len(lines) + 0.06
    return hh


def section_title(slide, num, title, kicker, subtitle=None):
    rect(slide, 0, 0, STRIP_W, SH, fill=NAVY)
    rect(slide, 0, 0, STRIP_W, 2.15, fill=GOLD)
    _, tf = textbox(slide, SW - 1.55, 0.40, 0.93, 0.62, align=PP_ALIGN.RIGHT)
    p = para(tf, first=True, line=1.0)
    p.alignment = PP_ALIGN.RIGHT
    run(p, num, 34, RGBColor(0xD7, 0xDF, 0xE8), bold=True)
    _, tf = textbox(slide, ML, 0.45, 8.6, 0.24)
    p = para(tf, first=True)
    run(p, kicker, 10, GRAY_MID, bold=True, caps=True, spc=1.4)
    _, tf = textbox(slide, ML, 0.72, 10.9, 0.50)
    p = para(tf, first=True, line=1.0)
    run(p, title, 25.5, NAVY, bold=True)
    rect(slide, ML, 1.33, 0.86, 0.036, fill=GOLD)
    rect(slide, ML + 0.92, 1.345, CW - 0.92, 0.014, fill=LINE_LT)
    y = MT
    if subtitle:
        k = n_lines(subtitle, SZ_SUB, CW)
        _, tf = textbox(slide, ML, y, CW, 0.28 * k + 0.10)
        for i, ln in enumerate(wrap_lines(subtitle, SZ_SUB, CW)):
            p = para(tf, first=(i == 0), line=1.2)
            for chunk, b in segments(ln):
                run(p, chunk, SZ_SUB, INK if b else GRAY_TXT, bold=b)
        y += k * SZ_SUB_LH + 0.14
    return y


def footer(slide, name, idx, total=10):
    rect(slide, ML, FOOTER_RULE_Y, CW, 0.012, fill=LINE_LT)
    _, tf = textbox(slide, ML, FOOTER_RULE_Y + 0.10, 10.2, 0.24)
    p = para(tf, first=True)
    run(p, "Права и общие обязанности военнослужащих в РФ", 9, GRAY_LT)
    run(p, "   ·   " + name, 9, GRAY_MID)
    _, tf = textbox(slide, SW - MR - 1.6, FOOTER_RULE_Y + 0.10, 1.6, 0.24,
                    align=PP_ALIGN.RIGHT)
    p = para(tf, first=True)
    p.alignment = PP_ALIGN.RIGHT
    run(p, "%02d" % idx, 9, GOLD, bold=True)
    run(p, " / %02d" % total, 9, GRAY_LT)


# ----------------------------------------------------------------------------
# Слайды
# ----------------------------------------------------------------------------
def slide_01(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, SW, SH, fill=NAVY)
    rect(s, 8.55, -1.2, 6.4, 10.0, fill=NAVY_2)
    rect(s, 10.62, -1.2, 4.0, 10.0, fill=NAVY_3)
    rect(s, 0, 0, STRIP_W, SH, fill=GOLD)
    cx, cy = 11.62, 2.05
    rect(s, cx - 0.62, cy - 0.62, 1.24, 1.24, line=GOLD_SOFT, line_w=1.5,
         shape=MSO_SHAPE.DIAMOND)
    rect(s, cx - 0.36, cy - 0.36, 0.72, 0.72, line=WHITE, line_w=1.0,
         shape=MSO_SHAPE.DIAMOND)
    _, tf = textbox(s, cx - 1.5, cy - 0.17, 3.0, 0.34, align=PP_ALIGN.CENTER)
    p = para(tf, first=True, line=1.0)
    p.alignment = PP_ALIGN.CENTER
    run(p, "76-ФЗ", 13.5, WHITE, bold=True)

    _, tf = textbox(s, 0.95, 0.98, 9.2, 0.26)
    p = para(tf, first=True)
    run(p, "Военная служба в Российской Федерации", 11.5, GOLD_SOFT, bold=True,
        caps=True, spc=2.0)
    rect(s, 0.95, 1.38, 1.05, 0.036, fill=GOLD)
    _, tf = textbox(s, 0.95, 1.72, 8.6, 2.0)
    p = para(tf, first=True, line=1.02)
    run(p, "Права и общие обязанности", 38, WHITE, bold=True)
    p = para(tf, line=1.02)
    run(p, "военнослужащих в РФ", 38, WHITE, bold=True)
    _, tf = textbox(s, 0.95, 3.46, 7.7, 0.85)
    p = para(tf, first=True, line=1.28)
    run(p, "Правовые основы статуса, воинский долг, социальные гарантии "
           "и ответственность участников военной службы", 15, ON_DARK)

    meta = [
        ("ПРАВОВАЯ ОСНОВА", ["Федеральный закон № 76-ФЗ", "«О статусе военнослужащих»,",
                              "Конституция РФ, общевоинские", "уставы ВС РФ"]),
        ("СТРУКТУРА", ["10 слайдов: обязанности, права,", "политические ограничения,",
                       "ответственность"]),
        ("ОФОРМЛЕНИЕ", ["Строгий деловой стиль,", "без изображений и иллюстраций"]),
    ]
    x0, y0, wcol = 0.95, 4.92, 3.0
    for i, (k, lines) in enumerate(meta):
        x = x0 + i * (wcol + 0.35)
        rect(s, x, y0, wcol, 0.014, fill=RGBColor(0x3A, 0x5C, 0x86))
        _, tf = textbox(s, x, y0 + 0.17, wcol, 0.24)
        p = para(tf, first=True)
        run(p, k, 9.5, GOLD_SOFT, bold=True, spc=1.4)
        _, tf = textbox(s, x, y0 + 0.47, wcol, 1.0)
        for j, ln in enumerate(lines):
            p = para(tf, first=(j == 0), line=1.22)
            run(p, ln, 10.5, ON_DARK)
    _, tf = textbox(s, 0.95, 6.88, 8.6, 0.26)
    p = para(tf, first=True)
    run(p, "Учебная презентация · формулировки — по действующей редакции закона",
        9.5, RGBColor(0x86, 0x9E, 0xBA))
    _, tf = textbox(s, SW - 2.3, 6.88, 1.65, 0.26, align=PP_ALIGN.RIGHT)
    p = para(tf, first=True)
    p.alignment = PP_ALIGN.RIGHT
    run(p, "01", 9.5, GOLD_SOFT, bold=True)
    run(p, " / 10", 9.5, RGBColor(0x86, 0x9E, 0xBA))


def slide_02(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, SW, SH, fill=WHITE)
    y = section_title(
        s, "02", "Правовая основа статуса военнослужащего", "Нормативные акты",
        "Статус военнослужащего — совокупность прав, свобод, обязанностей и ответственности, "
        "закреплённых Конституцией РФ, федеральными законами и общевоинскими уставами.")
    blocks = [
        {"h": "ФЗ № 76-ФЗ «О статусе военнослужащих»", "items": [
            "от 27.05.1998 — базовый закон, который определяет права, свободы, обязанности "
            "и социальную защиту военнослужащих.",
            "Ст. 1 — понятия: воинская обязанность, воинский долг, военная служба; "
            "ст. 2 — общие обязанности и правовая защита.",
            "Разделы II–IV — социально-экономические, личные права и гарантии, ответственность.",
        ]},
        {"h": "Конституция Российской Федерации", "items": [
            "Ст. 59 — защита Отечества долг и обязанность гражданина; военная служба — "
            "особый вид службы.",
            "Ст. 2, 17–19 — права и свободы имеют прямое действие и гарантируются государством.",
            "Ограничения прав допустимы только как следствие специфики службы и закреплены "
            "федеральным законом.",
        ]},
        {"h": "Общевоинские уставы ВС РФ", "items": [
            "Устав внутренней службы: общие обязанности военнослужащих (ст. 29) и обязанности "
            "по гарнизонной службе.",
            "Дисциплинарный устав: воинская слава, доблесть, честь; порядок поддержания "
            "дисциплины и взыскания.",
            "Устав гарнизонной и караульной служб, Строевой устав — повседневная деятельность "
            "подразделения.",
        ]},
        {"h": "Иные акты, определяющие статус", "items": [
            "ФЗ № 53-ФЗ «О воинской обязанности и военной службе» — условия и порядок "
            "прохождения службы.",
            "ФЗ «Об обороне», ФЗ № 117-ФЗ (военная ипотека), Закон РФ № 2159-1 "
            "(государственная тайна).",
            "Указы Президента и постановления Правительства РФ, приказы Минобороны России; "
            "УК и КоАП РФ.",
        ]},
    ]
    refs = [
        "Действующая редакция ФЗ № 76-ФЗ — первоисточник формулировок (КонсультантПлюс, Гарант).",
        "Общевоинские уставы ВС РФ утверждены Указом Президента РФ от 10.11.2007 № 1495.",
    ]
    rh = ref_strip_h(refs)
    bottom = flow_cards(s, blocks, y + 0.06, cols=2, reserve=rh + 0.26)
    ref_strip(s, bottom + 0.26, refs)
    footer(s, "Правовая основа", 2)


def slide_03(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, SW, SH, fill=WHITE)
    y = section_title(
        s, "03", "Сущность воинского долга и назначения службы", "Воинский долг",
        "Воинский долг — признанная и принятая военнослужащим необходимость достойного "
        "исполнения обязанностей военной службы во имя защиты Отечества.")
    blocks = [
        {"h": "Защита Отечества", "items": [
            "Конституция РФ (ст. 59): защита Отечества — долг и обязанность гражданина.",
            "Служба направлена на реализацию ст. 59 и 87 Конституции РФ — оборона страны.",
            "Верность Военной присяге (контракту) — личное обязательство, с которого "
            "начинается служба.",
        ]},
        {"h": "Безопасность государства", "items": [
            "Отражение вооружённого нападения, защита суверенитета, независимости и "
            "территориальной целостности.",
            "Обеспечение военной безопасности — функция ВС РФ и иных войск (ФЗ «Об обороне»).",
            "Поддержание боевой готовности, содействие безопасности при чрезвычайных ситуациях.",
        ]},
        {"h": "Международные обязательства", "items": [
            "Участие в операциях по поддержанию и восстановлению мира по международным "
            "договорам РФ.",
            "Решение о применении ВС за пределами страны принимает Совет Федерации по "
            "представлению Президента.",
            "Взаимодействие в рамках ОДКБ, совместные учения и мероприятия боевой подготовки.",
        ]},
    ]
    note = ("Права и обязанности военнослужащих производны от назначения военной службы: "
            "расширение социальных гарантий компенсирует ограничения, без которых невозможны "
            "единоначалие и готовность к выполнению задач.")
    nh = note_strip_h(note, label="Вывод")
    bottom = flow_cards(s, blocks, y + 0.06, cols=3, reserve=nh + 0.24)
    note_strip(s, bottom + 0.24, note, label="Вывод")
    footer(s, "Сущность воинского долга", 3)


def slide_04(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, SW, SH, fill=WHITE)
    y = section_title(
        s, "04", "Общие обязанности военнослужащих", "Устав внутренней службы ВС РФ",
        "Касаются каждого военнослужащего независимо от звания, должности и выслуги лет. "
        "Их нарушение — дисциплинарный проступок, а в ряде случаев — правонарушение.")
    items = [
        "Быть верным Военной присяге (контракту о прохождении военной службы), беззаветно "
        "служить народу Российской Федерации, мужественно и умело защищать Российскую Федерацию.",
        "Строго соблюдать Конституцию и законы Российской Федерации, требования общевоинских "
        "уставов, беспрекословно выполнять приказы командиров (начальников).",
        "Проявлять дисциплинированность, уважение к воинской чести и добрым именам сослуживцев, "
        "неукоснительно соблюдать воинские обычаи, поддерживать войсковое товарищество.",
        "Совершенствовать воинское мастерство, постоянно повышать военно-профессиональную "
        "квалификацию, знать и беречь вверенное вооружение и военную технику.",
        "Беречь военное и государственное имущество, грамотно эксплуатировать вооружение "
        "и военную технику, поддерживать её в готовности к применению.",
        "Хранить государственную и военную тайну; быть бдительным, честно и добросовестно "
        "исполнять обязанности военной службы.",
    ]
    refs = [
        "Источник: **ст. 29 Устава внутренней службы ВС РФ** — общие обязанности; "
        "**ст. 2 ФЗ № 76-ФЗ** — правовая защита, уважение чести и достоинства, ответственность.",
        "Кроме общих, военнослужащий исполняет **должностные** (по занимаемой должности) "
        "и **специальные** (караульная, гарнизонная службы, суточный наряд) обязанности.",
    ]
    rh = ref_strip_h(refs)
    bottom = bullets_panel(s, y + 0.06, items, reserve=rh + 0.26)
    ref_strip(s, bottom + 0.26, refs)
    footer(s, "Общие обязанности", 4)


def slide_05(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, SW, SH, fill=WHITE)
    y = section_title(
        s, "05", "Профессиональные и специальные обязанности", "Мастерство, готовность, тайна",
        "Объём этих обязанностей определяется должностью, воинским званием, уставными "
        "инструкциями и наставлениями по видам службы.")
    blocks = [
        {"h": "Боевая подготовка и мастерство", "items": [
            "Совершенное овладение вверенным вооружением и военной техникой, участие в учениях "
            "и тренировках.",
            "Поддержание постоянной боевой готовности; умение действовать в условиях применения "
            "современных средств поражения.",
            "Уровень военно-профессиональной подготовки влияет на присвоение званий и назначение "
            "на вышестоящие должности.",
        ]},
        {"h": "Сбережение вооружения и техники", "items": [
            "Соблюдение правил хранения, эксплуатации, технического обслуживания и ремонта.",
            "Утрата, порча или недобросовестное отношение к имуществу — основания для "
            "материальной ответственности.",
            "Закрепление имущества за должностными лицами фиксируется порядком материального "
            "учёта в воинской части.",
        ]},
        {"h": "Защита государственной тайны", "items": [
            "Допуск по Закону РФ № 2159-1 от 21.07.1993; ограничения на выезд, работу "
            "и разглашение сведений.",
            "Разглашение или утрата сведений, составляющих государственную тайну, — "
            "ст. 283, 284 УК РФ.",
            "Пропускной и внутриобъектовый режимы, запрет на публикацию служебной информации "
            "в соцсетях.",
        ]},
        {"h": "Специальные обязанности по видам служб", "items": [
            "Суточный наряд, дежурство по части, гарнизонная и караульная службы — по "
            "соответствующим уставам.",
            "Служба в боевом охранении, на боевом дежурстве, в режимах повышенной готовности.",
            "Для отдельных категорий — специфика полётных, подводных, испытательных и иных "
            "видов службы.",
        ]},
    ]
    flow_cards(s, blocks, y + 0.06, cols=2)
    footer(s, "Профессиональные обязанности", 5)


def slide_06(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, SW, SH, fill=WHITE)
    y = section_title(
        s, "06", "Социально-экономические права и гарантии", "Раздел II ФЗ № 76-ФЗ",
        "Государство компенсирует особенности службы денежным, жилищным, медицинским "
        "и социальным обеспечением военнослужащего и членов его семьи.")
    blocks = [
        {"h": "Денежное довольствие (ст. 2)", "items": [
            "Оклад по воинской должности и оклад по воинскому званию; ежемесячные и иные выплаты.",
            "Надбавки: за выслугу лет, классную квалификацию, особые условия службы, работу "
            "с гостайной.",
            "Индексация размеров, единовременные выплаты при увольнении, районные коэффициенты "
            "и процентные надбавки.",
        ]},
        {"h": "Жилищное обеспечение (ст. 15)", "items": [
            "Служебные жилые помещения на период службы, жилые помещения в закрытых "
            "городках — общежития.",
            "Единовременная денежная выплата (ЕДВ) на приобретение или строительство жилья — "
            "для признанных нуждающимися.",
            "Военная ипотека — накопительно-ипотечная система (НИС), ФЗ № 117-ФЗ от 20.08.2004.",
        ]},
        {"h": "Медицинское обеспечение (ст. 16)", "items": [
            "Бесплатная медицинская помощь в военно-медицинских организациях, включая "
            "специализированную и высокотехнологичную.",
            "Обеспечение лекарственными препаратами, изготовление и ремонт зубных протезов.",
            "Санаторно-курортное лечение и реабилитация — для военнослужащих и членов семей.",
        ]},
        {"h": "Отдых, страхование, быт (ст. 11, 12, 17)", "items": [
            "Основной отпуск исчисляется в сутках и зависит от выслуги лет; дополнительно "
            "учитывается время на проезд к месту отдыха и обратно.",
            "Обязательное государственное страхование жизни, здоровья и имущества; "
            "единовременные пособия при ранениях (увечьях).",
            "Вещевое и продовольственное обеспечение (или соответствующие выплаты), "
            "военно-транспортные перевозки.",
        ]},
    ]
    flow_cards(s, blocks, y + 0.06, cols=2)
    footer(s, "Социально-экономические права", 6)


def slide_07(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, SW, SH, fill=WHITE)
    y = section_title(
        s, "07", "Политические права и служебные ограничения", "Раздел III ФЗ № 76-ФЗ",
        "Военнослужащие сохраняют общероссийские политические права, но порядок их "
        "реализации ограничен в интересах аполитичности армии и боеготовности.")
    blocks = [
        {"h": "Что сохраняется", "items": [
            "Избирательное право: участие в референдуме и выборах, выдвижение кандидатом "
            "в порядке, установленном законодательством.",
            "Обращения в государственные органы, суд и прокуратуру; доступ к сведениям о себе.",
            "Свобода слова и информации — в части, не затрагивающей охраняемую законом тайну.",
        ]},
        {"h": "Что запрещено или ограничено", "items": [
            "Членство в политических партиях, участие в их деятельности, митингах, шествиях "
            "и пикетировании, использование служебного положения в интересах партий.",
            "Забастовки и отказ от исполнения обязанностей; предпринимательская деятельность "
            "и оплачиваемая работа по совместительству (кроме преподавательской, научной, "
            "творческой).",
            "Публичные оценки деятельности органов государственной власти и политических "
            "объединений.",
        ]},
        {"h": "Дополнительные ограничения", "items": [
            "Ограничения на выезд за пределы РФ на период службы и на срок, установленный "
            "условиями допуска к гостайне.",
            "Исполнение заведомо незаконного приказа не освобождает от ответственности; "
            "неисполнение законного приказа — правонарушение.",
            "Правила использования мобильных устройств, социальных сетей и мессенджеров "
            "в воинской части.",
        ]},
    ]
    note = ("Собрания, митинги и демонстрации допустимы только как участие гражданина — "
            "во внеслужебное время, без форменной одежды, знаков различия и оружия.")
    nh = note_strip_h(note, label="Практика")
    bottom = flow_cards(s, blocks, y + 0.06, cols=3, reserve=nh + 0.24)
    note_strip(s, bottom + 0.24, note, label="Практика")
    footer(s, "Политические права и ограничения", 7)


def slide_08(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, SW, SH, fill=WHITE)
    y = section_title(
        s, "08", "Личные права и свободы военнослужащего", "Достоинство, вера, юридическая защита",
        "Личные права защищают самого человека — его жизнь, здоровье, честь и мировоззрение "
        "— вне зависимости от занимаемой должности.")
    blocks = [
        {"h": "Защита жизни, чести и достоинства", "items": [
            "Честь и достоинство охраняются законом; оскорбление и унижение недопустимы ни со "
            "стороны сослуживцев, ни со стороны начальников.",
            "Государство защищает военнослужащего и членов его семьи от насилия, угроз и "
            "посягательств в связи с исполнением обязанностей.",
            "Ответственность за оскорбление военнослужащего при исполнении обязанностей — "
            "ст. 319 УК РФ.",
        ]},
        {"h": "Свобода совести и вероисповедания", "items": [
            "Право исповедовать любую религию или не исповедовать никакой, свободно выбирать "
            "и распространять убеждения.",
            "Религиозные обряды совершаются во внеслужебное время и не должны мешать "
            "повседневной деятельности подразделения.",
            "Духовные (военные) служители могут привлекаться к окормлению личного состава "
            "в установленном порядке.",
        ]},
        {"h": "Юридическая защита и обжалование", "items": [
            "Решения, действия (бездействие) командиров обжалуются вышестоящему командиру, "
            "в гарнизонский военный суд, военному прокурору.",
            "Право на юридическую помощь и на судебную защиту, включая обжалование в "
            "вышестоящие органы.",
            "Надзор за исполнением законов на военной службе осуществляют военная "
            "прокуратура и военные следственные органы СК РФ.",
        ]},
        {"h": "Информация, творчество, личная жизнь", "items": [
            "Свобода слова и информации с учётом запретов, связанных со спецификой службы "
            "и защитой тайны.",
            "Право на отдых, неприкосновенность частной жизни, жилище, охрану здоровья — "
            "в объёме, определённом ФЗ № 76-ФЗ.",
            "Творческая и писательская деятельность — при отсутствии в материалах сведений, "
            "отнесённых к охраняемой законом тайне.",
        ]},
    ]
    flow_cards(s, blocks, y + 0.06, cols=2)
    footer(s, "Личные права", 8)


def slide_09(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, SW, SH, fill=WHITE)
    y = section_title(
        s, "09", "Ответственность военнослужащих", "Виды юридической ответственности",
        "За неисполнение обязанностей военнослужащий отвечает в пределах, установленных "
        "уставами, дисциплинарным, административным, гражданским и уголовным законодательством.")
    blocks = [
        {"h": "Дисциплинарная", "items": [
            "Наступает за дисциплинарный проступок — невыполнение приказа, нарушение "
            "воинской дисциплины и уставного порядка.",
            "Взыскания: замечание, выговор, строгий выговор, предупреждение о несоответствии "
            "занимаемой должности, штраф, лишение нагрудного знака, понижение в должности "
            "или в воинском звании на одну ступень, дисциплинарный арест.",
            "Срок действия взыскания — один год; возможно снятие досрочно (Дисциплинарный "
            "устав ВС РФ).",
        ]},
        {"h": "Административная", "items": [
            "За административные правонарушения, совершённые **во внеслужебное время и не "
            "по службе** (например, гл. 19 КоАП РФ — воинский учёт, режимные правила).",
            "За воинские проступки административная ответственность не применяется — "
            "вместо неё наступает дисциплинарная.",
        ]},
        {"h": "Материальная и гражданско-правовая", "items": [
            "Материальная ответственность за ущерб, причинённый при исполнении обязанностей, — "
            "в пределах прямого действительного ущерба (ФЗ № 145-ФЗ от 12.07.1999).",
            "Полная материальная ответственность — в случаях, предусмотренных законом "
            "(умысел, состояние опьянения, хищение).",
            "Вред, причинённый третьим лицам, возмещает воинская часть, за ней — регресс "
            "к виновному.",
        ]},
        {"h": "Уголовная", "items": [
            "Преступления против военной службы — **глава 33 УК РФ** (ст. 331–352.1): "
            "неисполнение приказа, сопротивление начальнику, самовольное оставление части, "
            "дезертирство.",
            "За иные преступления — ответственность на общих основаниях, но с учётом статуса "
            "военнослужащего.",
            "Смягчение или освобождение от ответственности возможно при добровольном "
            "предотвращении (пресечении) правонарушения.",
        ]},
    ]
    flow_cards(s, blocks, y + 0.06, cols=2)
    footer(s, "Ответственность военнослужащих", 9)


def slide_10(prs):
    s = prs.slides.add_slide(prs.slide_layouts[6])
    rect(s, 0, 0, SW, SH, fill=WHITE)
    y = section_title(
        s, "10", "Заключение", "Итоги",
        "Статус военнослужащего строится на единстве обязанностей и прав: государство "
        "требует готовности защищать Отечество и взамен гарантирует защиту самому "
        "военнослужащему и его семье.")
    blocks = [
        {"h": "Баланс — основа боеспособности", "items": [
            "Права создают условия для службы (довольствие, жильё, медицина, отдых), "
            "обязанности формируют единую волю и готовность подразделения.",
            "Искажение любой из частей — ослабление Вооружённых Сил.",
        ]},
        {"h": "Обязанности действуют непрерывно", "items": [
            "Общие обязанности закреплены уставами и не ограничены рамками служебного времени: "
            "они охватывают и поведение во внеслужебное время.",
            "Должностные и специальные обязанности конкретизируют общие.",
        ]},
        {"h": "Ограничения законны, а не произвольны", "items": [
            "Политические и иные ограничения установлены федеральным законом и вытекают из "
            "специфики службы.",
            "Командиры не вправе расширять ограничения по своему усмотрению.",
        ]},
        {"h": "Ответственность персональна", "items": [
            "Дисциплинарная, административная, материальная и уголовная ответственность "
            "наступает лично.",
            "Незаконность приказа и превышение пределов его исполнения оцениваются судом.",
        ]},
    ]
    note = ("Военнослужащий не вправе требовать реализации своего права в ущерб исполнению "
            "обязанностей военной службы; равно как и командир не вправе требовать действий, "
            "выходящих за рамки закона и уставов.")
    nh = note_strip_h(note, label="Ключевое правило")
    bottom = flow_cards(s, blocks, y + 0.06, cols=2, reserve=nh + 0.80)
    note_strip(s, bottom + 0.22, note, label="Ключевое правило")
    ysrc = bottom + 0.22 + nh + 0.14
    _, tf = textbox(s, ML, ysrc, CW, 0.50)
    for i, ln in enumerate(wrap_lines(
            "Источники: Конституция РФ; Федеральный закон от 27.05.1998 № 76-ФЗ "
            "«О статусе военнослужащих»; Устав внутренней службы, Дисциплинарный устав, "
            "Устав гарнизонной и караульной служб ВС РФ (Указ Президента РФ от 10.11.2007 "
            "№ 1495); УК РФ; КоАП РФ.", 9.5, CW)):
        p = para(tf, first=(i == 0), line=1.2)
        run(p, ln, 9.5, GRAY_MID)
    footer(s, "Заключение", 10)


# ----------------------------------------------------------------------------
def main():
    prs = Presentation()
    prs.slide_width = Inches(SW)
    prs.slide_height = Inches(SH)
    for fn in (slide_01, slide_02, slide_03, slide_04, slide_05, slide_06,
               slide_07, slide_08, slide_09, slide_10):
        del AUDIT[:]
        fn(prs)
        bots = [b for name, b in AUDIT if name in ("card", "panel", "note") and b < 7.4]
        tag = fn.__name__
        print("  %-9s низ контента: %.2f" % (tag, max(bots) if bots else 0))
        if bots and max(bots) > BOTTOM_LIMIT + 0.02:
            print("!! переполнение: нижняя граница контента %.2f" % max(bots))
    if OVER:
        print("!! текст не влезает в карточку (нужно/доступно/заголовок):")
        for o in OVER:
            print("   ", o)
    else:
        print("вёрстка: переполнений нет")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    prs.save(OUT)
    n = len(prs.slides._sldIdLst)
    print("saved: %s (%d слайдов, %.1f КБ)" % (OUT, n, os.path.getsize(OUT) / 1024))


if __name__ == "__main__":
    main()
