// Cloudflare Worker + KV binding CONTROL. No secret is present in this file.
// If KV is empty, missing, or unavailable, status is disabled (fail closed).
const headers = {
  'content-type': 'text/plain; charset=utf-8',
  'cache-control': 'no-store, max-age=0',
  'x-content-type-options': 'nosniff',
};
const help = 'Команды: /on — включить, /off — выключить, /status — проверить, /help — помощь.';

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

  const command = typeof message.text === 'string'
    ? message.text.trim().toLowerCase().split(/\s+/, 1)[0] : '';
  const bareCommand = command.split('@')[0];
  let answer = help;
  try {
    if (bareCommand === '/on' || bareCommand === '/off') {
      const value = bareCommand === '/on' ? 'enabled' : 'disabled';
      await env.CONTROL.put('enabled', value);
      answer = value === 'enabled' ? 'Включено. Изменение KV может примениться с задержкой.'
        : 'Выключено. Изменение KV может примениться с задержкой.';
    } else if (bareCommand === '/status') {
      const value = await env.CONTROL.get('enabled');
      answer = value === 'enabled' ? 'Статус: включено.' : 'Статус: выключено.';
    }
  } catch (_) {
    answer = 'Ошибка Cloudflare KV: действие не подтверждено. Проверьте /status позднее.';
  }
  await sendTelegram(env, chat.id, answer);
  return reply('ok');
}

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);
    if (pathname === '/status' && request.method === 'GET') {
      try {
        const value = await env.CONTROL.get('enabled');
        return reply(value === 'enabled' ? 'enabled' : 'disabled');
      } catch (_) {
        return reply('disabled', 503);
      }
    }
    if (pathname === '/telegram' && request.method === 'POST')
      return telegramWebhook(request, env);
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
        await env.CONTROL.put('enabled', data.enabled ? 'enabled' : 'disabled');
        return reply(data.enabled ? 'enabled' : 'disabled');
      } catch (_) {
        return reply('storage error', 503);
      }
    }
    return reply('not found', 404);
  },
};
