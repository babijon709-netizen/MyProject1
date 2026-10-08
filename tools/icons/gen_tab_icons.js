// Иконки вкладок левой панели меню: Phosphor Icons, вариант regular (MIT,
// https://github.com/phosphor-icons/core) -> белые линии на прозрачном фоне,
// PNG 192x192 -> jni/include/media/tab_icons.h. Без обводки и заливки.
//
// Запуск (зависимость ставится в любую временную папку, в репозиторий не нужна):
//   npm i @resvg/resvg-js @phosphor-icons/core
//   NODE_PATH=$PWD/node_modules node <repo>/tools/icons/gen_tab_icons.js \
//       $PWD/node_modules/@phosphor-icons/core/assets/regular <repo>/jni/include/media/tab_icons.h

const fs = require('fs');
const path = require('path');
const { Resvg } = require('@resvg/resvg-js');

const [iconDir, outHeader] = process.argv.slice(2);
if (!iconDir || !outHeader) {
    console.error('usage: gen_tab_icons.js <phosphor regular assets dir> <out header>');
    process.exit(1);
}

const SIZE = 192;      // сторона текстуры, px (иконка в меню 60 px, запас на HiDPI)
const WHITE = '#ffffff';

// id -> файл иконки Phosphor (regular). Индексы вкладок в menu.cpp: 1 Аим, 2 ESP, 3 Разное, 4 Конфиги, 5 Опции.
const TABS = [
    { id: 'tab_aim',  icon: 'crosshair' },
    { id: 'tab_esp',  icon: 'eye' },
    { id: 'tab_misc', icon: 'squares-four' },
    { id: 'tab_cfg',  icon: 'floppy-disk' },
    { id: 'tab_opt',  icon: 'gear' },
];

function buildSvg(src) {
    const clean = src.replace(/<!--[\s\S]*?-->/g, '');
    const vb = clean.match(/viewBox="([^"]+)"/);
    if (!vb) throw new Error('no viewBox');
    const body = clean.match(/<svg[^>]*>([\s\S]*)<\/svg>/)[1];
    return `<svg xmlns="http://www.w3.org/2000/svg" width="${SIZE}" height="${SIZE}" viewBox="${vb[1]}">` +
        `<g fill="${WHITE}">${body}</g></svg>`;
}

function renderPng(svg) {
    return new Resvg(svg, { fitTo: { mode: 'width', value: SIZE } }).render().asPng();
}

const out = [
    '// Иконки вкладок левой панели меню: Phosphor Icons, regular (MIT, https://github.com/phosphor-icons/core).',
    '// Белые линии на прозрачном фоне, 192x192 PNG. Сгенерировано',
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
    out.push(`// phosphor: ${t.icon}`);
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
