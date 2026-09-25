// Cloudflare Worker with optional D1 binding CONTROL_DB (preferred) or legacy KV
// binding CONTROL. No secret is present in this file. Failed reads deny access.
const headers = {
  'content-type': 'text/plain; charset=utf-8',
  'cache-control': 'no-store, max-age=0',
  'x-content-type-options': 'nosniff',
};
const help = 'Команды: /on, /off, /status, /devices, /ban HWID, /unban HWID, /help.';

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
      body: JSON.stringify({ chat_id: chatId, text }),
    });
  } catch (_) {
    // The KV operation may have succeeded despite an unavailable Telegram API.
    // Return 200 so Telegram does not repeatedly replay a state-changing update.
  }
}

// New binaries send only the digest, not the raw Android ID. Registration is
// one-time per process; heartbeat requests never write device records.
async function deviceAuth(request, env) {
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
      typeof data.register !== 'boolean')
    return reply('disabled', 400);
  const name = data.name.trim().replace(/[\x00-\x1f\x7f]/g, ' ').slice(0, 80) || 'Android device';
  try {
    if (data.register) {
      const result = await env.CONTROL_DB.prepare(
        'INSERT OR IGNORE INTO benzware_devices (hwid, name) VALUES (?, ?)'
      ).bind(data.hwid, name).run();
      if (result?.success === false) throw Error('D1 write failed');
      if (result?.meta?.changes === 1 && /^\d+$/.test(String(env.TELEGRAM_ADMIN_ID ?? '')))
        await sendTelegram(env, env.TELEGRAM_ADMIN_ID,
          `Новое устройство: ${name}\nHWID: ${data.hwid}\n/ban ${data.hwid}`);
    }
    const status = await readSwitch(env);
    const device = await env.CONTROL_DB.prepare(
      'SELECT banned FROM benzware_devices WHERE hwid = ?').bind(data.hwid).first();
    return reply(status === 'enabled' && device?.banned === 0 ? 'enabled' : 'disabled');
  } catch (_) { return reply('disabled', 503); }
}

async function manageDevice(env, command, hwid) {
  if (!env.CONTROL_DB) return 'Для управления устройствами привяжите D1 как CONTROL_DB.';
  if (command === '/devices') {
    const result = await env.CONTROL_DB.prepare(
      'SELECT hwid, name, banned FROM benzware_devices ORDER BY first_seen DESC LIMIT 20').all();
    if (!result?.results?.length) return 'Устройств пока нет. Запустите новую сборку на телефоне.';
    return 'Устройства (последние 20):\n' + result.results.map((row) =>
      `${row.banned ? '⛔' : '✅'} ${row.name}\nHWID: ${row.hwid}`
    ).join('\n');
  }
  if (!/^[a-f0-9]{64}$/.test(hwid || ''))
    return 'Укажите полный HWID из /devices: /ban HWID или /unban HWID.';
  const banned = command === '/ban' ? 1 : 0;
  const result = await env.CONTROL_DB.prepare(
    'UPDATE benzware_devices SET banned = ? WHERE hwid = ?'
  ).bind(banned, hwid).run();
  if (result?.success === false) throw Error('D1 write failed');
  if (result?.meta?.changes !== 1) return 'HWID не найден. Сначала запустите новую сборку.';
  return banned ? 'Устройство заблокировано: ' + hwid : 'Блокировка снята: ' + hwid;
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

  const words = typeof message.text === 'string' ? message.text.trim().split(/\s+/) : [];
  const bareCommand = (words[0] || '').toLowerCase().split('@')[0];
  let answer = help;
  try {
    if (bareCommand === '/on' || bareCommand === '/off') {
      const value = bareCommand === '/on' ? 'enabled' : 'disabled';
      await writeSwitch(env, value);
      answer = value === 'enabled' ? 'Включено. Проверьте /status.'
        : 'Выключено. Проверьте /status.';
    } else if (bareCommand === '/status') {
      const value = await readSwitch(env);
      answer = value === 'enabled' ? 'Статус: включено.' : 'Статус: выключено.';
    } else if (bareCommand === '/devices' || bareCommand === '/ban' || bareCommand === '/unban') {
      answer = await manageDevice(env, bareCommand, words[1]?.toLowerCase());
    }
  } catch (_) {
    answer = 'Ошибка хранилища Cloudflare: действие не подтверждено. Проверьте /status позднее.';
  }
  await sendTelegram(env, chat.id, answer);
  return reply('ok');
}

export default {
  async fetch(request, env) {
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
      return deviceAuth(request, env);
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
