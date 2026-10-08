// Иконки вкладок левой панели меню: Material Design Icons (filled, Apache-2.0) ->
// PNG 192x192 -> jni/include/media/tab_icons.h.
//
// Иконка: сплошной глубокий чёрный силуэт с белой обводкой. Обводка — тот же
// путь, обведённый белым штрихом шириной 2 * HALO (HALO с каждой стороны),
// под чёрной заливкой. Текстура без тинта, цвет зашит в пиксели.
//
// Запуск (зависимость ставится в любую временную папку, в репозиторий не нужна):
//   npm i @resvg/resvg-js @material-design-icons/svg
//   NODE_PATH=$PWD/node_modules node <repo>/tools/icons/gen_tab_icons.js \
//       $PWD/node_modules/@material-design-icons/svg/filled <repo>/jni/include/media/tab_icons.h

const fs = require('fs');
const path = require('path');
const { Resvg } = require('@resvg/resvg-js');

const [iconDir, outHeader] = process.argv.slice(2);
if (!iconDir || !outHeader) {
    console.error('usage: gen_tab_icons.js <material filled icons dir> <out header>');
    process.exit(1);
}

const SIZE = 192;      // сторона текстуры, px (иконка в меню 60 px, запас на HiDPI)
const HALO = 1.0;      // ширина белой обводки с каждой стороны, в единицах viewBox 24
const BLACK = '#000000';
const WHITE = '#ffffff';

// id -> имя иконки Material. Индексы вкладок в menu.cpp: 1 Аим, 2 ESP, 3 Разное, 4 Конфиги, 5 Опции.
const TABS = [
    { id: 'tab_aim',  icon: 'gps_fixed' },
    { id: 'tab_esp',  icon: 'visibility' },
    { id: 'tab_misc', icon: 'apps' },
    { id: 'tab_cfg',  icon: 'folder_special' },
    { id: 'tab_opt',  icon: 'settings' },
];

function buildSvg(src) {
    const m = src.replace(/<!--[\s\S]*?-->/g, '').match(/<svg[^>]*>([\s\S]*)<\/svg>/);
    if (!m) throw new Error('no <svg> body');
    const body = m[1];
    const outline = `<g fill="${WHITE}" stroke="${WHITE}" stroke-width="${2 * HALO}" stroke-linejoin="round">${body}</g>`;
    const fill = `<g fill="${BLACK}">${body}</g>`;
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${SIZE}" height="${SIZE}" viewBox="0 0 24 24">` +
        outline + fill + `</svg>`;
}

function renderPng(svg) {
    return new Resvg(svg, { fitTo: { mode: 'width', value: SIZE } }).render().asPng();
}

const out = [
    '// Иконки вкладок левой панели меню: Material Design Icons (Apache-2.0, https://github.com/google/material-design-icons).',
    '// Чёрный силуэт с белой обводкой, 192x192 PNG. Сгенерировано',
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
    out.push(`// material: ${t.icon}`);
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
