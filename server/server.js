#!/usr/bin/env node
// ============================================================================
//  Лицензионный сервер benzware — один файл, ноль зависимостей (Node 16+).
//
//  Запуск:
//    SECRET='<тот же, что в license.cpp>' ADMIN_SECRET='<пароль админа>' \
//      node server.js
//
//  Переменные окружения:
//    PORT         — порт (по умолчанию 8787; клиент ходит на этот порт)
//    SECRET       — ОБЯЗАТЕЛЬНО поменять: должен совпадать с S_SECRET()
//                   в jni/src/license.cpp — им подписываются ответы (HMAC)
//    ADMIN_SECRET — пароль для админ-команд (заголовок X-Admin); без него
//                   админка выключена
//    DB_PATH      — путь к базе ключей (по умолчанию ./db.json рядом)
//
//  Админ-примеры (как выключить бинарь и прочее) — в PROTECTION.md.
// ============================================================================
'use strict';

const http = require('http');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const PORT = parseInt(process.env.PORT || '8787', 10);
const SECRET = process.env.SECRET || 'CHANGE_ME_same_secret_as_on_server_7f3a9c21';
const ADMIN = process.env.ADMIN_SECRET || '';
const DB_PATH = process.env.DB_PATH || path.join(__dirname, 'db.json');

if (SECRET.startsWith('CHANGE_ME'))
    console.warn('[warn] SECRET не задан своим значением — задай переменную окружения SECRET (и такую же строку в license.cpp)!');

// ─────────────────────────────── база ───────────────────────────────

let db;
function loadDb() {
    try { db = JSON.parse(fs.readFileSync(DB_PATH, 'utf8')); }
    catch (e) { db = { global: { kill: false }, min_build: 0, keys: {} }; }
    if (!db.global) db.global = { kill: false };
    if (typeof db.min_build !== 'number') db.min_build = 0;
    if (!db.keys) db.keys = {};
}
function saveDb() {
    const tmp = DB_PATH + '.tmp';
    fs.writeFileSync(tmp, JSON.stringify(db, null, 2));
    fs.renameSync(tmp, DB_PATH);
}
loadDb();

// ───────────────────────── подпись и отправка ───────────────────────

function hmac(s) {
    return crypto.createHmac('sha256', SECRET).update(s).digest('hex');
}

// Каждый ответ клиенту подписан HMAC-SHA256 от тела — подделать/пропатчить
// ответ по сети без SECRET нельзя (клиент неподписанное игнорирует).
function sendSigned(res, code, obj) {
    const body = JSON.stringify(obj);
    res.writeHead(code, {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(body),
        'X-Sig': hmac(body),
        'Connection': 'close',
    });
    res.end(body);
}

function sendPlain(res, code, text) {
    const body = String(text);
    res.writeHead(code, {
        'Content-Type': 'text/plain; charset=utf-8',
        'Content-Length': Buffer.byteLength(body),
        'Connection': 'close',
    });
    res.end(body);
}

// ─────────────────────────── утилиты ────────────────────────────────

const ts = () => Math.floor(Date.now() / 1000);

function genKey() {
    const abc = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; // без похожих символов
    const g = n => Array.from(crypto.randomBytes(n), b => abc[b % abc.length]).join('');
    return `BZWR-${g(4)}-${g(4)}-${g(4)}`;
}

function readBody(req) {
    return new Promise((resolve, reject) => {
        let acc = '';
        req.on('data', c => {
            acc += c;
            if (acc.length > 16384) { reject(new Error('body too big')); req.destroy(); }
        });
        req.on('end', () => {
            if (!acc) return resolve({});
            try { resolve(JSON.parse(acc)); } catch (e) { reject(new Error('bad json')); }
        });
        req.on('error', reject);
    });
}

// примитивный rate-limit: 60 api-запросов/мин с IP, 30 админских
const rl = new Map();
function rateOk(ip, bucket, limit) {
    const now = Date.now(), k = bucket + '|' + ip;
    let arr = rl.get(k);
    if (!arr) { arr = []; rl.set(k, arr); }
    while (arr.length && now - arr[0] > 60000) arr.shift();
    if (arr.length >= limit) return false;
    arr.push(now);
    if (rl.size > 10000) rl.clear();
    return true;
}

// ─────────────────────── проверка лицензии ──────────────────────────

function deny(reason) {
    return { ok: false, reason, ts: ts() };
}

function checkLicense(body, allowBind) {
    if (db.global.kill) return deny('killed');
    const key = String(body.key || '');
    const hwid = String(body.hwid || '');
    const build = parseInt(body.build || 0, 10);
    const rec = db.keys[key];
    if (!rec) return deny('bad_key');
    if (rec.banned) return deny('banned');
    const now = ts();
    if (rec.expires && now > rec.expires) return deny('expired');
    if (db.min_build && build < db.min_build) return deny('update');
    if (!rec.hwid) {
        if (allowBind && hwid) { rec.hwid = hwid; rec.bound_at = now; saveDb(); }
    } else if (rec.hwid !== hwid) {
        return deny('hwid');
    }
    return {
        ok: true,
        ts: now,
        expires: rec.expires || 0,
        session: crypto.randomBytes(4).readUInt32BE(0),
    };
}

// ─────────────────────────── маршруты ───────────────────────────────

const server = http.createServer(async (req, res) => {
    const ip = req.socket.remoteAddress || '?';
    const url = new URL(req.url, 'http://x');
    const route = req.method + ' ' + url.pathname;

    try {
        // --- клиентские ---
        if (route === 'GET /api/ping' || route === 'GET /') {
            return sendSigned(res, 200, { ok: true, ts: ts(), service: 'benzware-license' });
        }
        if (route === 'POST /api/verify' || route === 'POST /api/heartbeat') {
            // 429 без подписи: клиент воспримет это как «сеть лежит» и уйдёт
            // в grace — правильно, rate-limit это временная ситуация, а не отказ.
            if (!rateOk(ip, 'api', 60)) return sendPlain(res, 429, 'rate limited');
            const body = await readBody(req);
            const r = checkLicense(body, route === 'POST /api/verify');
            console.log(new Date().toISOString(), ip, url.pathname,
                String(body.key || '').slice(0, 9) + '…', r.ok ? 'OK' : 'DENY:' + r.reason);
            return sendSigned(res, 200, r);
        }

        // --- админские (нужен заголовок X-Admin) ---
        if (url.pathname.startsWith('/admin/')) {
            if (!ADMIN) return sendPlain(res, 403, 'admin disabled: set ADMIN_SECRET');
            if (!rateOk(ip, 'adm', 30)) return sendPlain(res, 429, 'rate limited');
            const given = String(req.headers['x-admin'] || '');
            const okAuth = given.length === ADMIN.length &&
                crypto.timingSafeEqual(Buffer.from(given), Buffer.from(ADMIN));
            if (!okAuth) return sendPlain(res, 401, 'bad admin secret');

            if (route === 'GET /admin/state') {
                return sendPlain(res, 200, JSON.stringify({
                    global: db.global, min_build: db.min_build, keys: Object.keys(db.keys).length,
                }, null, 2));
            }
            if (route === 'GET /admin/keys') {
                const out = {};
                for (const [k, v] of Object.entries(db.keys))
                    out[k] = { ...v, hwid: v.hwid ? v.hwid.slice(0, 8) + '…' : null };
                return sendPlain(res, 200, JSON.stringify(out, null, 2));
            }
            const body = await readBody(req);

            // ГЛОБАЛЬНЫЙ ВЫКЛЮЧАТЕЛЬ: все бинари останавливаются при следующем
            // heartbeat (в пределах пары минут).
            if (route === 'POST /admin/kill') {
                db.global.kill = !!body.on; saveDb();
                return sendPlain(res, 200, 'global kill = ' + db.global.kill);
            }
            // Новый ключ: {"days":30,"note":"покупатель"} или {"days":0} — бессрочный
            if (route === 'POST /admin/addkey') {
                const key = body.key ? String(body.key) : genKey();
                const days = parseInt(body.days || 0, 10);
                db.keys[key] = {
                    hwid: null,
                    expires: days > 0 ? ts() + days * 86400 : 0,
                    banned: false,
                    note: String(body.note || ''),
                    created: ts(),
                };
                saveDb();
                return sendPlain(res, 200, key);
            }
            if (route === 'POST /admin/ban') {
                const rec = db.keys[String(body.key || '')];
                if (!rec) return sendPlain(res, 404, 'no such key');
                rec.banned = body.banned !== false; saveDb();
                return sendPlain(res, 200, 'banned = ' + rec.banned);
            }
            if (route === 'POST /admin/delkey') {
                delete db.keys[String(body.key || '')]; saveDb();
                return sendPlain(res, 200, 'deleted');
            }
            if (route === 'POST /admin/unbind') { // сброс привязки к устройству
                const rec = db.keys[String(body.key || '')];
                if (!rec) return sendPlain(res, 404, 'no such key');
                rec.hwid = null; saveDb();
                return sendPlain(res, 200, 'hwid reset');
            }
            if (route === 'POST /admin/extend') { // продлить: {"key":"...","days":30}
                const rec = db.keys[String(body.key || '')];
                if (!rec) return sendPlain(res, 404, 'no such key');
                const days = parseInt(body.days || 0, 10);
                const base = rec.expires && rec.expires > ts() ? rec.expires : ts();
                rec.expires = base + days * 86400; saveDb();
                return sendPlain(res, 200, 'expires = ' + new Date(rec.expires * 1000).toISOString());
            }
            // Требовать минимальный номер сборки: старые бинари перестанут
            // проходить verify — ещё один рубеж «выключить старое».
            if (route === 'POST /admin/minbuild') {
                db.min_build = parseInt(body.n || 0, 10); saveDb();
                return sendPlain(res, 200, 'min_build = ' + db.min_build);
            }
            return sendPlain(res, 404, 'unknown admin route');
        }

        sendPlain(res, 404, 'not found');
    } catch (e) {
        console.error('request error:', e.message);
        try { sendPlain(res, 500, 'error'); } catch (_) { /* сокет уже закрыт */ }
    }
});

server.listen(PORT, '0.0.0.0', () => {
    console.log(`[benzware-license] listening on 0.0.0.0:${PORT}`);
    console.log(`[benzware-license] db: ${DB_PATH}, admin: ${ADMIN ? 'on' : 'OFF (set ADMIN_SECRET)'}`);
});
