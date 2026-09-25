import assert from 'node:assert/strict';
import worker from '../../cloudflare/worker.mjs';
let state;
const store = {
  async get() { return state; },
  async put(_key, value) { state = value; },
};
const env = {
  CONTROL: store, ADMIN_TOKEN: 'a'.repeat(40),
  TELEGRAM_WEBHOOK_SECRET: 'b'.repeat(40), TELEGRAM_ADMIN_ID: '123456',
  TELEGRAM_BOT_TOKEN: 'test-token',
};
const call = (path, init) => worker.fetch(new Request('https://example.workers.dev' + path, init), env);
const post = (token, body) => call('/admin', {
  method: 'POST', headers: { Authorization: 'Bearer ' + token, 'Content-Type': 'application/json' },
  body,
});
assert.equal(await (await call('/status')).text(), 'disabled\n');
assert.equal((await post('bad', '{"enabled":true}')).status, 401);
assert.equal((await post(env.ADMIN_TOKEN, '{"enabled":1}')).status, 400);
assert.equal((await post(env.ADMIN_TOKEN, '{"enabled":true}')).status, 200);
assert.equal(await (await call('/status')).text(), 'enabled\n');
assert.equal((await post(env.ADMIN_TOKEN, '{"enabled":false}')).status, 200);
assert.equal(await (await call('/status')).text(), 'disabled\n');
assert.equal((await call('/admin', {method:'GET'})).status, 404);
// D1 device inventory and bans: reads are from D1, not stale KV.
let d1Value = 0;
const devices = new Map();
const presence = new Map();
let presenceWrites = 0;
const rowFor = (d) => ({ ...d, last_seen: presence.get(d.hwid) ?? null });
const d1 = {
  prepare(query) {
    if (query.startsWith('SELECT enabled FROM benzware_control')) return {
      async first() { return { enabled: d1Value }; },
    };
    if (query.startsWith('INSERT INTO benzware_control')) return {
      bind(enabled) { return {
        async run() { d1Value = enabled; return { success: true }; },
      }; },
    };
    if (query.startsWith('INSERT OR IGNORE INTO benzware_devices')) return {
      bind(hwid, name) { return {
        async run() {
          if (devices.has(hwid)) return { success: true, meta: { changes: 0 } };
          devices.set(hwid, { hwid, name, banned: 0 });
          return { success: true, meta: { changes: 1 } };
        },
      }; },
    };
    if (query.startsWith('SELECT banned FROM benzware_devices')) return {
      bind(hwid) { return { async first() { return devices.get(hwid) || null; } }; },
    };
    if (query.startsWith('SELECT hwid, name, banned FROM benzware_devices')) return {
      async all() { return { results: [...devices.values()] }; },
    };
    if (query.startsWith('SELECT last_seen FROM benzware_presence')) return {
      bind(hwid) { return { async first() {
        return presence.has(hwid) ? { last_seen: presence.get(hwid) } : null;
      } }; },
    };
    if (query.startsWith('INSERT INTO benzware_presence')) return {
      bind(hwid, lastSeen) { return { async run() {
        presence.set(hwid, lastSeen);
        presenceWrites++;
        return { success: true };
      } }; },
    };
    if (query.startsWith('SELECT (SELECT COUNT(*)')) return {
      bind(cutoff) { return { async first() { return {
        total: devices.size,
        banned: [...devices.values()].filter(d => d.banned).length,
        online: [...devices.values()].filter(d => !d.banned && (presence.get(d.hwid) ?? 0) >= cutoff).length,
      }; } }; },
    };
    if (query.startsWith('SELECT d.hwid') && query.includes('WHERE d.hwid = ?')) return {
      bind(hwid) { return { async first() {
        return devices.has(hwid) ? rowFor(devices.get(hwid)) : null;
      } }; },
    };
    if (query.startsWith('SELECT d.hwid') && query.includes('FROM benzware_presence')) return {
      bind(cutoff, limit, offset) { return { async all() { return { results: [...devices.values()]
        .filter(d => !d.banned && (presence.get(d.hwid) ?? 0) >= cutoff)
        .map(rowFor).sort((a,b) => b.last_seen - a.last_seen).slice(offset, offset + limit) }; } }; },
    };
    if (query.startsWith('SELECT d.hwid') && query.includes('LEFT JOIN benzware_presence')) return {
      bind(limit, offset) { return { async all() { return { results: [...devices.values()]
        .map(rowFor).reverse().slice(offset, offset + limit) }; } }; },
    };
    if (query.startsWith('UPDATE benzware_devices')) return {
      bind(banned, hwid) { return {
        async run() {
          const device = devices.get(hwid);
          if (device) device.banned = banned;
          return { success: true, meta: { changes: device ? 1 : 0 } };
        },
      }; },
    };
    throw Error('Unexpected SQL: ' + query);
  },
};
const d1Env = { ...env, CONTROL_DB: d1, CONTROL: {
  get() { throw Error('KV must not be read with D1 bound'); },
  put() { throw Error('KV must not be written with D1 bound'); },
} };
const d1Call = (path, init) => worker.fetch(new Request('https://example.workers.dev' + path, init), d1Env);
const nonce = '0123456789abcdef0123456789abcdef';
const auth = (hwid, name, register = true, req = d1Call) => req('/auth', {
  method: 'POST', headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ hwid, name, register, nonce }),
});
assert.equal(await (await d1Call('/status')).text(), 'disabled\n');
assert.equal((await d1Call('/admin', {
  method: 'POST', headers: { Authorization: 'Bearer ' + env.ADMIN_TOKEN, 'Content-Type': 'application/json' },
  body: '{"enabled":true}',
})).status, 200);
assert.equal(await (await d1Call('/status')).text(), 'enabled\n');
assert.equal((await worker.fetch(new Request('https://example.workers.dev/status'), {
  ...env, CONTROL_DB: { prepare() { throw Error('D1 unavailable'); } },
})).status, 503); // Never fall back to a potentially enabled/stale KV value.
assert.equal((await auth('a'.repeat(64), 'Pixel 8', true, call)).status, 503); // D1 required.
assert.equal((await d1Call('/auth', {
  method: 'POST', headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ hwid: 'a'.repeat(64), name: 'Pixel 8', register: true }),
})).status, 400); // Missing nonce never registers or enables.
assert.equal(devices.size, 0);

assert.equal((await worker.fetch(new Request('https://example.workers.dev/status'), {
  CONTROL: { get() { throw Error('KV unavailable'); } }, ADMIN_TOKEN: env.ADMIN_TOKEN,
})).status, 503);

const sent = [];
const realFetch = globalThis.fetch;
const realNow = Date.now;
let fakeNow = 1800000000000;
Date.now = () => fakeNow;
globalThis.fetch = async (url, init) => {
  sent.push({ url, body: JSON.parse(init.body) });
  return new Response('{"ok":true}', { status: 200 });
};
const telegram = (text, secret = env.TELEGRAM_WEBHOOK_SECRET, userId = 123456, type = 'private') =>
  call('/telegram', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-Telegram-Bot-Api-Secret-Token': secret },
    body: JSON.stringify({ update_id: 1, message: {
      from: { id: userId }, chat: { id: userId, type }, text,
    } }),
  });
try {
  assert.equal((await telegram('/on', 'wrong-secret')).status, 403);
  assert.equal((await worker.fetch(new Request('https://example.workers.dev/telegram', {
    method: 'POST', headers: { 'Content-Type': 'application/json',
      'X-Telegram-Bot-Api-Secret-Token': env.TELEGRAM_WEBHOOK_SECRET },
    body: JSON.stringify({message: {from: {id:123456}, chat: {id:123456,type:'private'}, text:'/on'}}),
  }), { ...env, TELEGRAM_WEBHOOK_SECRET: undefined })).status, 403);
  assert.equal((await telegram('/on', env.TELEGRAM_WEBHOOK_SECRET, 987654)).status, 200);
  assert.equal((await telegram('/on', env.TELEGRAM_WEBHOOK_SECRET, 123456, 'group')).status, 200);
  assert.equal(state, 'disabled');
  assert.equal(sent.length, 0);
  assert.equal((await telegram('/on')).status, 200);
  assert.equal(state, 'enabled');
  assert.equal(sent.at(-1).body.chat_id, 123456);
  assert.equal(sent.at(-1).url, 'https://api.telegram.org/bottest-token/sendMessage');
  assert.equal((await telegram('/status')).status, 200);
  assert.match(sent.at(-1).body.text, /ВКЛ/);
  assert.equal(sent.at(-1).body.parse_mode, 'HTML');
  assert.equal(sent.at(-1).body.reply_markup.keyboard[0][1].text, '🟢 Онлайн');
  assert.equal((await telegram('/off')).status, 200);
  assert.equal(state, 'disabled');
  assert.equal((await telegram('/help')).status, 200);
  assert.match(sent.at(-1).body.text, /\/on/);
  const d1Telegram = (text) => d1Call('/telegram', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json',
      'X-Telegram-Bot-Api-Secret-Token': env.TELEGRAM_WEBHOOK_SECRET },
    body: JSON.stringify({ message: {
      from: { id: 123456 }, chat: { id: 123456, type: 'private' }, text,
    } }),
  });
  assert.equal((await d1Telegram('/off')).status, 200);
  assert.equal(await (await d1Call('/status')).text(), 'disabled\n');
  assert.equal((await d1Telegram('/on')).status, 200);
  assert.equal(await (await d1Call('/status')).text(), 'enabled\n');

  assert.equal(await (await d1Call('/status')).text(), 'enabled\n');
  const hwid = 'a'.repeat(64);
  assert.equal(await (await auth(hwid, 'Pixel 8')).text(), 'enabled:' + nonce + '\n');
  assert.equal(sent.at(-1).body.text.includes('Pixel 8'), true);
  assert.equal(sent.at(-1).body.text.includes(hwid), true);
  const notifications = sent.length;
  assert.equal(await (await auth(hwid, 'Pixel 8')).text(), 'enabled:' + nonce + '\n');
  assert.equal(sent.length, notifications); // Only first registration notifies.
  assert.equal(await (await auth('b'.repeat(64), 'Unknown', false)).text(), 'disabled\n');
  assert.equal((await d1Telegram('/devices')).status, 200);
  assert.match(sent.at(-1).body.text, /Pixel 8/);
  assert.match(sent.at(-1).body.text, /a{64}/);
  assert.equal(presenceWrites, 1); // Repeated heartbeat never writes D1.
  assert.equal((await d1Telegram('🟢 Онлайн')).status, 200);
  assert.match(sent.at(-1).body.text, /Pixel 8/);
  assert.match(sent.at(-1).body.text, /3,5 минуты/);
  assert.equal((await d1Telegram('📈 Статистика')).status, 200);
  assert.match(sent.at(-1).body.text, /активны: <b>1<\/b>/);
  assert.equal((await d1Telegram('/device ' + hwid)).status, 200);
  assert.match(sent.at(-1).body.text, /Карточка устройства/);
  assert.equal((await d1Telegram('🛑 Выключить')).status, 200);
  assert.equal(await (await d1Call('/status')).text(), 'disabled\n');
  assert.equal(await (await auth(hwid, 'Pixel 8', false)).text(), 'disabled\n');
  assert.equal(presenceWrites, 1); // Denied requests do not refresh activity.
  assert.equal((await d1Telegram('✅ Включить')).status, 200);
  fakeNow += 119000;
  assert.equal(await (await auth(hwid, 'Pixel 8', false)).text(), 'enabled:' + nonce + '\n');
  assert.equal(presenceWrites, 1);
  fakeNow += 1000;
  assert.equal(await (await auth(hwid, 'Pixel 8', false)).text(), 'enabled:' + nonce + '\n');
  assert.equal(presenceWrites, 2); // At most once per 120 seconds.
  fakeNow += 211000;
  assert.equal((await d1Telegram('/online')).status, 200);
  assert.doesNotMatch(sent.at(-1).body.text, /Pixel 8/);
  assert.equal((await d1Telegram('/stats')).status, 200);
  assert.match(sent.at(-1).body.text, /активны: <b>0<\/b>/);
  assert.equal(await (await auth(hwid, 'Pixel 8', false)).text(), 'enabled:' + nonce + '\n');
  assert.equal(presenceWrites, 3);
  // Names are never parsed as Telegram HTML supplied by the device.
  const maliciousName = '<b>Bad & Name</b>';
  const badHwid = 'c'.repeat(64);
  assert.equal(await (await auth(badHwid, maliciousName)).text(), 'enabled:' + nonce + '\n');
  assert.match(sent.at(-1).body.text, /&lt;b&gt;Bad &amp; Name&lt;\/b&gt;/);
  assert.doesNotMatch(sent.at(-1).body.text, /<b>Bad & Name<\/b>/);
  assert.equal((await d1Telegram('/devices')).status, 200);
  assert.match(sent.at(-1).body.text, /&lt;b&gt;Bad &amp; Name&lt;\/b&gt;/);
  assert.equal((await d1Telegram('/device ' + badHwid)).status, 200);
  assert.match(sent.at(-1).body.text, /&lt;b&gt;Bad &amp; Name&lt;\/b&gt;/);
  assert.equal((await d1Telegram('/devices 0')).status, 200);
  assert.match(sent.at(-1).body.text, /Укажите номер страницы/);
  for (let i = 0; i < 7; ++i) {
    const id = i.toString(16).repeat(64);
    assert.equal(await (await auth(id, 'Page ' + i)).text(), 'enabled:' + nonce + '\n');
  }
  assert.equal((await d1Telegram('📱 Устройства')).status, 200);
  assert.match(sent.at(-1).body.text, /Далее: \/devices 2/);
  assert.equal((await d1Telegram('/devices 2')).status, 200);
  assert.match(sent.at(-1).body.text, /страница 2/);
  assert.equal((await d1Telegram('/online 2')).status, 200);
  assert.match(sent.at(-1).body.text, /страница 2/);
  assert.equal((await d1Telegram('/ban ' + hwid)).status, 200);
  assert.equal(await (await auth(hwid, 'Pixel 8', false)).text(), 'disabled\n');
  assert.equal((await d1Telegram('/online')).status, 200);
  assert.doesNotMatch(sent.at(-1).body.text, /Pixel 8/);
  assert.equal((await d1Telegram('/unban ' + hwid)).status, 200);
  assert.equal(await (await auth(hwid, 'Pixel 8', false)).text(), 'enabled:' + nonce + '\n');
  assert.equal((await d1Telegram('/ban short')).status, 200);
  assert.equal(await (await auth(hwid, 'Pixel 8', false)).text(), 'enabled:' + nonce + '\n');

  // Deploying Worker before the presence migration (or losing optional writes)
  // cannot deny an otherwise authorized device.
  fakeNow += 121000; // Force a presence write attempt after the throttling window.
  for (const failure of ['missing table', 'write failure']) {
    const failingD1 = {
      prepare(query) {
        if (query.includes('benzware_presence') &&
            (failure === 'missing table' || query.startsWith('INSERT INTO benzware_presence'))) {
          throw Error(failure);
        }
        return d1.prepare(query);
      },
    };
    const failingEnv = { ...d1Env, CONTROL_DB: failingD1 };
    const request = (path, init) => worker.fetch(new Request('https://example.workers.dev' + path, init), failingEnv);
    assert.equal(await (await auth(hwid, 'Pixel 8', false, request)).text(), 'enabled:' + nonce + '\n');
    assert.equal((await request('/status')).status, 200);
    const failingBot = (text) => request('/telegram', {
      method: 'POST', headers: { 'Content-Type': 'application/json',
        'X-Telegram-Bot-Api-Secret-Token': env.TELEGRAM_WEBHOOK_SECRET },
      body: JSON.stringify({ message: { from: { id: 123456 },
        chat: { id: 123456, type: 'private' }, text } }),
    });
    assert.equal((await failingBot('/status')).status, 200);
    assert.match(sent.at(-1).body.text, /ВКЛ/);
    if (failure === 'missing table') {
      assert.match(sent.at(-1).body.text, /Статистика пока недоступна/);
    }
  }
  assert.equal((await call('/telegram', { method: 'GET' })).status, 404);
} finally {
  globalThis.fetch = realFetch;
  Date.now = realNow;
}
console.log('Worker + Telegram switch tests: OK');
