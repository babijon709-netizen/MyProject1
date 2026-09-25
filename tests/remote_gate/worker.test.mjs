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
assert.equal((await worker.fetch(new Request('https://example.workers.dev/status'), {
  CONTROL: { get() { throw Error('KV unavailable'); } }, ADMIN_TOKEN: env.ADMIN_TOKEN,
})).status, 503);

const sent = [];
const realFetch = globalThis.fetch;
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
  assert.match(sent.at(-1).body.text, /включено/);
  assert.equal((await telegram('/off')).status, 200);
  assert.equal(state, 'disabled');
  assert.equal((await telegram('/help')).status, 200);
  assert.match(sent.at(-1).body.text, /\/on/);
  assert.equal((await call('/telegram', { method: 'GET' })).status, 404);
} finally {
  globalThis.fetch = realFetch;
}
console.log('Worker + Telegram switch tests: OK');
