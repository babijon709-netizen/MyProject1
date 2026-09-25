// Cloudflare Worker with optional D1 binding CONTROL_DB (preferred) or legacy KV
// binding CONTROL. No secret is present in this file. Failed reads deny access.
const headers = {
  'content-type': 'text/plain; charset=utf-8',
  'cache-control': 'no-store, max-age=0',
  'x-content-type-options': 'nosniff',
};
const PAGE_SIZE = 8;
const ONLINE_WINDOW_SECONDS = 210; // heartbeat ~60s, DB timestamp every ~120s
const PRESENCE_WRITE_SECONDS = 120;
const keyboard = {
  keyboard: [
    [{ text: '📊 Статус' }, { text: '🟢 Онлайн' }],
    [{ text: '📱 Устройства' }, { text: '📈 Статистика' }],
    [{ text: '✅ Включить' }, { text: '🛑 Выключить' }],
    [{ text: '❓ Помощь' }],
  ], resize_keyboard: true, is_persistent: true,
};
const help = '<b>BENZWARE • Управление</b>\n'
  + '📊 /status — общий доступ и сводка\n'
  + '🟢 /online — недавно активные устройства\n'
  + '📱 /devices [страница] — все устройства\n'
  + '📈 /stats — числа устройств\n'
  + '🔎 /device HWID — карточка устройства\n'
  + '⛔ /ban HWID — запретить запуск\n'
  + '✅ /unban HWID — снять бан\n'
  + '🔛 /on и /off — общий выключатель\n\n'
  + 'Онлайн = успешная проверка за последние ~3,5 минуты, а не присутствие в игре.';
function escapeHtml(value) {
  return String(value).replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
function ago(seconds, now) {
  if (!Number.isFinite(seconds)) return 'нет данных';
  const delta = Math.max(0, now - seconds);
  return delta < 60 ? 'только что' : `${Math.floor(delta / 60)} мин назад`;
}

// D1 uses primary reads without the Sessions API. Use it when bound for
// near-immediate revocation; KV is kept only for installations not migrated yet.
// Never fall back to KV if a configured D1 binding errors (fail closed).
async function readSwitch(env) {
  if (env.CONTROL_DB) {
    const row = await env.CONTROL_DB.prepare(
      'SELECT enabled FROM benzware_control WHERE id = 1').first();
    return row?.enabled === 1 ? 'enabled' : 'disabled';
  }
  const value = await env.CONTROL.get('enabled', { cacheTtl: 30 });
  return value === 'enabled' ? 'enabled' : 'disabled';
}

async function writeSwitch(env, value) {
  if (env.CONTROL_DB) {
    const result = await env.CONTROL_DB.prepare(
      'INSERT INTO benzware_control (id, enabled) VALUES (1, ?) '
      + 'ON CONFLICT(id) DO UPDATE SET enabled = excluded.enabled'
    ).bind(value === 'enabled' ? 1 : 0).run();
    if (result?.success === false) throw Error('D1 write failed');
    return;
  }
  await env.CONTROL.put('enabled', value);
}

function reply(text, status = 200) {
  return new Response(text + '\n', { status, headers });
}

// Compare fixed-length SHA-256 hashes, rather than short-circuiting on strings.
async function validToken(presented, expected) {
  if (typeof expected !== 'string' || expected.length < 32 ||
      typeof presented !== 'string' || presented.length > 512) return false;
  const encode = new TextEncoder();
  const [a, b] = await Promise.all([presented, expected].map(async (value) =>
    new Uint8Array(await crypto.subtle.digest('SHA-256', encode.encode(value)))));
  let mismatch = 0;
  for (let i = 0; i < a.length; i++) mismatch |= a[i] ^ b[i];
  return mismatch === 0;
}

async function sendTelegram(env, chatId, text) {
  // The bot token is a Cloudflare secret; never put it in a reply or a log.
  if (typeof env.TELEGRAM_BOT_TOKEN !== 'string' || !env.TELEGRAM_BOT_TOKEN) return;
  try {
    await fetch('https://api.telegram.org/bot' + env.TELEGRAM_BOT_TOKEN + '/sendMessage', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ chat_id: chatId, text, parse_mode: 'HTML',
        disable_web_page_preview: true, reply_markup: keyboard }),
    });
  } catch (_) {
    // The storage operation may have succeeded despite an unavailable Telegram API.
    // Return 200 so Telegram does not repeatedly replay a state-changing update.
  }
}

// New binaries send only the digest, not the raw Android ID. Registration is
// one-time per process; heartbeat requests never write device records.
async function deviceAuth(request, env, ctx) {
  if (!env.CONTROL_DB) return reply('disabled', 503); // bans require D1
  if (!request.headers.get('content-type')?.toLowerCase().startsWith('application/json'))
    return reply('disabled', 415);
  if (Number(request.headers.get('content-length') || 0) > 512) return reply('disabled', 413);
  let data;
  try {
    const body = await request.text();
    if (body.length > 512) return reply('disabled', 413);
    data = JSON.parse(body);
  } catch (_) { return reply('disabled', 400); }
  if (typeof data?.hwid !== 'string' || !/^[a-f0-9]{64}$/.test(data.hwid) ||
      typeof data.name !== 'string' || data.name.length > 80 ||
      typeof data.register !== 'boolean' ||
      typeof data.nonce !== 'string' || !/^[a-f0-9]{32}$/.test(data.nonce))
    return reply('disabled', 400);
  const name = data.name.trim().replace(/[\x00-\x1f\x7f]/g, ' ').slice(0, 80) || 'Android device';
  try {
    // Do not consume D1 writes on every restart of an already registered
    // device. A write-limit error on an unnecessary INSERT OR IGNORE should
    // not keep an existing, unbanned device from starting.
    let device = await env.CONTROL_DB.prepare(
      'SELECT banned FROM benzware_devices WHERE hwid = ?'
    ).bind(data.hwid).first();
    if (data.register && !device) {
      const result = await env.CONTROL_DB.prepare(
        'INSERT OR IGNORE INTO benzware_devices (hwid, name) VALUES (?, ?)'
      ).bind(data.hwid, name).run();
      if (result?.success === false) throw Error('D1 write failed');
      if (result?.meta?.changes === 1 && /^\d+$/.test(String(env.TELEGRAM_ADMIN_ID ?? '')))
        // Telegram API latency must never hold up the 8-second device timeout.
        ctx?.waitUntil(sendTelegram(env, env.TELEGRAM_ADMIN_ID,
          `📱 <b>Новое устройство</b>\n${escapeHtml(name)}\nHWID: <code>${data.hwid}</code>\n/ban ${data.hwid}`));
      device = await env.CONTROL_DB.prepare(
        'SELECT banned FROM benzware_devices WHERE hwid = ?'
      ).bind(data.hwid).first();
    }
    const status = await readSwitch(env);
    const allowed = status === 'enabled' && device?.banned === 0;
    if (allowed) {
      // Presence is best-effort. A missing table or a D1 write limit must
      // never turn a valid authorization into a client shutdown.
      try {
        const presence = await env.CONTROL_DB.prepare(
          'SELECT last_seen FROM benzware_presence WHERE hwid = ?'
        ).bind(data.hwid).first();
        const now = Math.floor(Date.now() / 1000);
        if (!presence || now - presence.last_seen >= PRESENCE_WRITE_SECONDS) {
          await env.CONTROL_DB.prepare(
            'INSERT INTO benzware_presence (hwid, last_seen) VALUES (?, ?) '
            + 'ON CONFLICT(hwid) DO UPDATE SET last_seen = excluded.last_seen'
          ).bind(data.hwid, now).run();
        }
      } catch (_) { /* Online list may be stale, authorization remains valid. */ }
    }
    return reply(allowed ? 'enabled:' + data.nonce : 'disabled');
  } catch (_) { return reply('disabled', 503); }
}

function pageNumber(value) {
  const page = value === undefined ? 1 : Number(value);
  return Number.isInteger(page) && page >= 1 && page <= 1000 ? page : null;
}

function deviceLine(row, now) {
  const online = !row.banned && Number.isFinite(row.last_seen)
    && row.last_seen >= now - ONLINE_WINDOW_SECONDS;
  return `${online ? '🟢' : row.banned ? '⛔' : '⚪'} <b>${escapeHtml(row.name)}</b>`
    + `\n<code>${row.hwid}</code>`
    + `\n${online ? 'Активность: ' + ago(row.last_seen, now) : row.banned ? 'Заблокировано' : 'Не в сети'}`;
}

async function counts(env, now) {
  return env.CONTROL_DB.prepare(
    'SELECT (SELECT COUNT(*) FROM benzware_devices) AS total, '
    + '(SELECT COUNT(*) FROM benzware_devices WHERE banned = 1) AS banned, '
    + '(SELECT COUNT(*) FROM benzware_presence p JOIN benzware_devices d ON d.hwid = p.hwid '
    + 'WHERE d.banned = 0 AND p.last_seen >= ?) AS online'
  ).bind(now - ONLINE_WINDOW_SECONDS).first();
}

async function listDevices(env, command, page, now) {
  const online = command === '/online';
  const rows = await env.CONTROL_DB.prepare(online
    ? 'SELECT d.hwid, d.name, d.banned, p.last_seen FROM benzware_presence p '
      + 'JOIN benzware_devices d ON d.hwid = p.hwid '
      + 'WHERE d.banned = 0 AND p.last_seen >= ? '
      + 'ORDER BY p.last_seen DESC LIMIT ? OFFSET ?'
    : 'SELECT d.hwid, d.name, d.banned, p.last_seen FROM benzware_devices d '
      + 'LEFT JOIN benzware_presence p ON p.hwid = d.hwid '
      + 'ORDER BY d.first_seen DESC LIMIT ? OFFSET ?'
  ).bind(...(online ? [now - ONLINE_WINDOW_SECONDS] : []), PAGE_SIZE, (page - 1) * PAGE_SIZE).all();
  const title = online ? '🟢 <b>Недавно активные устройства</b>' : '📱 <b>Устройства</b>';
  if (!rows?.results?.length) return `${title}\nНа странице ${page} устройств нет.`
    + (online ? '\nОнлайн = успешная проверка за последние ~3,5 минуты.' : '');
  const commandForNext = online ? '/online' : '/devices';
  return `${title} · страница ${page}\n\n`
    + rows.results.map((row) => deviceLine(row, now)).join('\n\n')
    + (rows.results.length === PAGE_SIZE ? `\n\nДалее: ${commandForNext} ${page + 1}` : '')
    + (online ? '\n\nОнлайн = успешная проверка за последние ~3,5 минуты.' : '');
}

async function manageDevice(env, command, arg, now) {
  if (!env.CONTROL_DB) return '⚠️ Для управления устройствами привяжите D1 как CONTROL_DB.';
  if (command === '/devices' || command === '/online') {
    const page = pageNumber(arg);
    if (!page) return 'Укажите номер страницы: /devices 1 или /online 1.';
    return listDevices(env, command, page, now);
  }
  if (command === '/stats') {
    const stats = await counts(env, now);
    return '<b>📈 BENZWARE • Статистика</b>\n'
      + `📱 Зарегистрировано: <b>${stats.total}</b>\n`
      + `🟢 Недавно активны: <b>${stats.online}</b>\n`
      + `⛔ Заблокировано: <b>${stats.banned}</b>\n\n`
      + 'Онлайн — успешная проверка за последние ~3,5 минуты, не присутствие в игре.';
  }
  const hwid = arg?.toLowerCase();
  if (!/^[a-f0-9]{64}$/.test(hwid || ''))
    return 'Укажите полный HWID из /devices: /device HWID, /ban HWID или /unban HWID.';
  if (command === '/device') {
    const row = await env.CONTROL_DB.prepare(
      'SELECT d.hwid, d.name, d.banned, p.last_seen FROM benzware_devices d '
      + 'LEFT JOIN benzware_presence p ON p.hwid = d.hwid WHERE d.hwid = ?'
    ).bind(hwid).first();
    return row ? '<b>🔎 Карточка устройства</b>\n' + deviceLine(row, now)
      + '\n\n' + (row.banned ? '/unban ' : '/ban ') + hwid : 'HWID не найден.';
  }
  const banned = command === '/ban' ? 1 : 0;
  const result = await env.CONTROL_DB.prepare(
    'UPDATE benzware_devices SET banned = ? WHERE hwid = ?'
  ).bind(banned, hwid).run();
  if (result?.success === false) throw Error('D1 write failed');
  if (result?.meta?.changes !== 1) return 'HWID не найден. Сначала запустите новую сборку.';
  return banned ? '⛔ <b>Устройство заблокировано</b>\n<code>' + hwid + '</code>'
    : '✅ <b>Блокировка снята</b>\n<code>' + hwid + '</code>';
}

async function telegramWebhook(request, env) {
  const secret = request.headers.get('x-telegram-bot-api-secret-token');
  if (!(await validToken(secret, env.TELEGRAM_WEBHOOK_SECRET))) return reply('forbidden', 403);
  if (!request.headers.get('content-type')?.toLowerCase().startsWith('application/json'))
    return reply('expected JSON', 415);
  if (Number(request.headers.get('content-length') || 0) > 8192) return reply('too large', 413);
  let update;
  try {
    const body = await request.text();
    if (body.length > 8192) return reply('too large', 413);
    update = JSON.parse(body);
  } catch (_) {
    return reply('bad JSON', 400);
  }
  const message = update?.message;
  const userId = String(message?.from?.id ?? '');
  const chat = message?.chat;
  // No group chats, forwarded messages, or IDs supplied in command text.
  // The ID comes only from Telegram's authenticated webhook payload.
  if (!/^\d+$/.test(String(env.TELEGRAM_ADMIN_ID ?? '')) ||
      userId !== String(env.TELEGRAM_ADMIN_ID) ||
      chat?.type !== 'private' || String(chat.id) !== userId) return reply('ok');

  const text = typeof message.text === 'string' ? message.text.trim() : '';
  const buttonCommands = {
    '📊 Статус': '/status', '🟢 Онлайн': '/online',
    '📱 Устройства': '/devices', '📈 Статистика': '/stats',
    '✅ Включить': '/on', '🛑 Выключить': '/off', '❓ Помощь': '/help',
  };
  const words = (buttonCommands[text] || text).split(/\s+/);
  const bareCommand = (words[0] || '').toLowerCase().split('@')[0];
  let answer = help;
  try {
    if (bareCommand === '/on' || bareCommand === '/off') {
      const value = bareCommand === '/on' ? 'enabled' : 'disabled';
      await writeSwitch(env, value);
      answer = value === 'enabled'
        ? '✅ <b>Общий доступ включён</b>\nЗаблокированные устройства остаются заблокированными.'
        : '🛑 <b>Общий доступ выключен</b>\nУстройства остановятся при следующей проверке.';
    } else if (bareCommand === '/status') {
      const value = await readSwitch(env);
      answer = '<b>📊 BENZWARE • Состояние</b>\n'
        + (value === 'enabled' ? '🟢 Общий доступ: <b>ВКЛ</b>' : '🔴 Общий доступ: <b>ВЫКЛ</b>');
      if (env.CONTROL_DB) {
        try {
          const stats = await counts(env, Math.floor(Date.now() / 1000));
          answer += `\n📱 Устройств: ${stats.total}  ·  🟢 Активны: ${stats.online}`
            + `  ·  ⛔ Бан: ${stats.banned}`;
        } catch (_) { answer += '\n⚠️ Статистика пока недоступна: проверьте таблицу presence.'; }
      }
    } else if (['/online', '/devices', '/stats', '/device', '/ban', '/unban'].includes(bareCommand)) {
      answer = await manageDevice(env, bareCommand, words[1], Math.floor(Date.now() / 1000));
    }
  } catch (_) {
    answer = '⚠️ <b>Ошибка хранилища Cloudflare</b>\nДействие не подтверждено. Проверьте /status позднее.';
  }
  await sendTelegram(env, chat.id, answer);
  return reply('ok');
}

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);
    if (pathname === '/status' && request.method === 'GET') {
      try {
        return reply(await readSwitch(env));
      } catch (_) {
        return reply('disabled', 503);
      }
    }
    if (pathname === '/telegram' && request.method === 'POST')
      return telegramWebhook(request, env);
    if (pathname === '/auth' && request.method === 'POST')
      return deviceAuth(request, env, ctx);
    if (pathname === '/admin' && request.method === 'POST') {
      const auth = request.headers.get('authorization') || '';
      if (!auth.startsWith('Bearer ') ||
          !(await validToken(auth.slice(7), env.ADMIN_TOKEN))) return reply('unauthorized', 401);
      if (!request.headers.get('content-type')?.toLowerCase().startsWith('application/json'))
        return reply('expected JSON', 415);
      let data;
      try {
        if (Number(request.headers.get('content-length') || 0) > 128) return reply('too large', 413);
        const body = await request.text();
        if (body.length > 128) return reply('too large', 413);
        data = JSON.parse(body);
      } catch (_) {
        return reply('bad JSON', 400);
      }
      if (typeof data?.enabled !== 'boolean' || Object.keys(data).length !== 1)
        return reply('expected {"enabled":true|false}', 400);
      try {
        await writeSwitch(env, data.enabled ? 'enabled' : 'disabled');
        return reply(data.enabled ? 'enabled' : 'disabled');
      } catch (_) {
        return reply('storage error', 503);
      }
    }
    return reply('not found', 404);
  },
};
