// Генератор иконок вкладок левой панели: Lucide (ISC) -> PNG 192x192 ->
// jni/include/media/tab_icons.h.
//
// Иконка: глубокий чёрный контур поверх белой обводки той же формы. Обводка
// рисуется той же геометрией с толщиной GLYPH + 2 * HALO, поэтому ровная по
// всему контуру. Текстура без тинта, цвет зашит в пиксели.
//
// Запуск (зависимость ставится в любую временную папку, в репозиторий не нужна):
//   npm i @resvg/resvg-js lucide-static
//   NODE_PATH=$PWD/node_modules node <repo>/tools/icons/gen_tab_icons.js \
//       $PWD/node_modules/lucide-static/icons <repo>/jni/include/media/tab_icons.h

const fs = require('fs');
const path = require('path');
const { Resvg } = require('@resvg/resvg-js');

const [iconDir, outHeader] = process.argv.slice(2);
if (!iconDir || !outHeader) {
    console.error('usage: gen_tab_icons.js <lucide icons dir> <out header>');
    process.exit(1);
}

const SIZE = 192;      // сторона текстуры, px (иконка в меню 60 px, запас на HiDPI)
const GLYPH = 2.2;     // толщина чёрного контура, в единицах viewBox 24
const HALO = 1.2;      // ширина белой обводки с каждой стороны, в единицах viewBox 24
const BLACK = '#000000';
const WHITE = '#ffffff';

// id -> имя иконки Lucide. Индексы вкладок в menu.cpp: 1 Аим, 2 ESP, 3 Разное, 4 Конфиги, 5 Опции.
const TABS = [
    { id: 'tab_aim',  icon: 'crosshair' },
    { id: 'tab_esp',  icon: 'scan-eye' },
    { id: 'tab_misc', icon: 'layout-grid' },
    { id: 'tab_cfg',  icon: 'folder-cog' },
    { id: 'tab_opt',  icon: 'settings' },
];

function buildSvg(src) {
    const m = src.replace(/<!--[\s\S]*?-->/g, '').match(/<svg[^>]*>([\s\S]*)<\/svg>/);
    if (!m) throw new Error('no <svg> body');
    const body = m[1];
    const layer = (color, width) =>
        `<g fill="none" stroke="${color}" stroke-width="${width}" ` +
        `stroke-linecap="round" stroke-linejoin="round">${body}</g>`;
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${SIZE}" height="${SIZE}" viewBox="0 0 24 24">` +
        layer(WHITE, GLYPH + 2 * HALO) + layer(BLACK, GLYPH) + `</svg>`;
}

function renderPng(svg) {
    return new Resvg(svg, { fitTo: { mode: 'width', value: SIZE } }).render().asPng();
}

const out = [
    '// Иконки вкладок левой панели меню: Lucide (https://lucide.dev, лицензия ISC).',
    '// Чёрный контур с белой обводкой, 192x192 PNG. Сгенерировано',
    '// tools/icons/gen_tab_icons.js — не править руками.',
    '#pragma once',
    '',
];

for (const t of TABS) {
    const src = fs.readFileSync(path.join(iconDir, `${t.icon}.svg`), 'utf8');
    const png = renderPng(buildSvg(src));
    const bytes = [];
    for (let i = 0; i < png.length; i++) {
        bytes.push('0x' + png[i].toString(16).padStart(2, '0'));
    }
    out.push(`// lucide: ${t.icon}`);
    out.push(`static const unsigned char ${t.id}_png[] = {`);
    for (let i = 0; i < bytes.length; i += 16) {
        out.push('    ' + bytes.slice(i, i + 16).join(', ') + ',');
    }
    out.push('};');
    out.push(`static const unsigned int ${t.id}_png_len = ${png.length};`);
    out.push('');
}

fs.writeFileSync(outHeader, out.join('\n'));
console.log(`wrote ${outHeader}`);
