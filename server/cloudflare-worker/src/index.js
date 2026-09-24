/**
 * Cheat Kill-Switch - Cloudflare Workers version
 * Бесплатный хост, глобальная сеть, <10ms latency
 * 
 * Деплой:
 *  npm install -g wrangler
 *  wrangler login
 *  wrangler secret put ADMIN_SECRET  (введи свой секрет)
 *  wrangler deploy
 * 
 * С KV (рекомендуется для persistence):
 *  wrangler kv:namespace create "CHEAT_KV"
 *  Вставь ID в wrangler.toml
 *  wrangler deploy
 * 
 * Без KV: состояние хранится в памяти (сбрасывается при рестарте воркера, но работает)
 */

// Default state
const DEFAULT_STATE = {
  enabled: true,
  message: "Cheat active",
  version: "1.0.0",
  min_version: "1.0.0",
  maintenance: false,
  updated_at: new Date().toISOString(),
  total_checks: 0,
  disabled_count: 0
};

// In-memory fallback (если нет KV)
let memoryState = { ...DEFAULT_STATE };

async function getState(env) {
  // Try KV first
  if (env.CHEAT_KV) {
    try {
      const stored = await env.CHEAT_KV.get("cheat_state", "json");
      if (stored) return stored;
    } catch (e) {
      console.error("KV get failed:", e);
    }
  }
  // Fallback to memory + env vars
  if (env.DEFAULT_ENABLED !== undefined) {
    memoryState.enabled = env.DEFAULT_ENABLED === "true" || env.DEFAULT_ENABLED === true;
  }
  if (env.CHEAT_VERSION) memoryState.version = env.CHEAT_VERSION;
  if (env.MIN_VERSION) memoryState.min_version = env.MIN_VERSION;
  return memoryState;
}

async function saveState(state, env) {
  state.updated_at = new Date().toISOString();
  memoryState = { ...state };
  
  if (env.CHEAT_KV) {
    try {
      await env.CHEAT_KV.put("cheat_state", JSON.stringify(state));
    } catch (e) {
      console.error("KV put failed:", e);
    }
  }
}

function jsonResponse(data, status = 200, extraHeaders = {}) {
  return new Response(JSON.stringify(data, null, 2), {
    status,
    headers: {
      "Content-Type": "application/json",
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type, X-Admin-Secret",
      ...extraHeaders
    }
  });
}

function htmlResponse(html, status = 200) {
  return new Response(html, {
    status,
    headers: {
      "Content-Type": "text/html; charset=utf-8",
      "Access-Control-Allow-Origin": "*"
    }
  });
}

function getAdminSecret(env) {
  return env.ADMIN_SECRET || "CHANGE_ME_SUPER_SECRET_12345";
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const path = url.pathname;

    // CORS preflight
    if (request.method === "OPTIONS") {
      return new Response(null, {
        headers: {
          "Access-Control-Allow-Origin": "*",
          "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
          "Access-Control-Allow-Headers": "Content-Type, X-Admin-Secret",
          "Access-Control-Max-Age": "86400"
        }
      });
    }

    // GET /api/status
    if (path === "/api/status" && request.method === "GET") {
      let state = await getState(env);
      state.total_checks++;
      if (!state.enabled) state.disabled_count++;
      ctx.waitUntil(saveState(state, env));

      return jsonResponse({
        enabled: state.enabled,
        message: state.message,
        version: state.version,
        min_version: state.min_version,
        maintenance: state.maintenance,
        timestamp: Math.floor(Date.now() / 1000),
        server_time: new Date().toISOString()
      });
    }

    // GET /api/check
    if (path === "/api/check" && request.method === "GET") {
      const hwid = url.searchParams.get("hwid");
      const version = url.searchParams.get("version");
      const user = url.searchParams.get("user");

      let state = await getState(env);
      state.total_checks++;
      if (!state.enabled) state.disabled_count++;
      ctx.waitUntil(saveState(state, env));

      if (hwid) {
        console.log(`Check HWID=${hwid} ver=${version} -> ${state.enabled}`);
      }

      return jsonResponse({
        enabled: state.enabled,
        message: state.message,
        version: state.version,
        min_version: state.min_version,
        maintenance: state.maintenance,
        timestamp: Math.floor(Date.now() / 1000),
        server_time: new Date().toISOString()
      });
    }

    // POST /api/admin/status
    if (path === "/api/admin/status" && request.method === "POST") {
      let body;
      try {
        body = await request.json();
      } catch {
        return jsonResponse({ error: "Invalid JSON" }, 400);
      }

      const secret = body.secret || request.headers.get("X-Admin-Secret") || url.searchParams.get("secret");
      if (secret !== getAdminSecret(env)) {
        return jsonResponse({ error: "Invalid secret" }, 403);
      }

      let state = await getState(env);
      if (typeof body.enabled === "boolean") state.enabled = body.enabled;
      if (typeof body.message === "string") state.message = body.message;
      if (typeof body.version === "string") state.version = body.version;
      if (typeof body.min_version === "string") state.min_version = body.min_version;
      if (typeof body.maintenance === "boolean") state.maintenance = body.maintenance;

      await saveState(state, env);
      console.log(`ADMIN UPDATE: enabled=${state.enabled} by ${request.headers.get("CF-Connecting-IP") || "unknown"}`);

      return jsonResponse({ success: true, state });
    }

    // GET /api/admin/state
    if (path === "/api/admin/state" && request.method === "GET") {
      const secret = url.searchParams.get("secret") || request.headers.get("X-Admin-Secret");
      if (secret !== getAdminSecret(env)) {
        return jsonResponse({ error: "Invalid secret" }, 403);
      }
      const state = await getState(env);
      return jsonResponse(state);
    }

    // GET /admin - админ панель
    if (path === "/admin" || path === "/admin/") {
      const state = await getState(env);
      const secretFromUrl = url.searchParams.get("secret") || "";
      // Встроенная админка (копия из render)
      return htmlResponse(`<!DOCTYPE html>
<html lang="ru"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Cheat Admin - Cloudflare</title>
<style>
*{box-sizing:border-box}body{font-family:Arial,sans-serif;background:#0f0f0f;color:#eee;min-height:100vh;margin:0;padding:20px;display:flex;justify-content:center}
.container{max-width:600px;width:100%}h1{text-align:center;color:#fff}
.card{background:#1e1e1e;border-radius:12px;padding:20px;margin:20px 0;border:1px solid #333}
.status{font-size:20px;padding:15px;border-radius:8px;text-align:center;margin:15px 0}
.enabled{background:#143314;border:2px solid #2a6a2a;color:#8f8}
.disabled{background:#331414;border:2px solid #6a2a2a;color:#f88}
input,textarea{width:100%;padding:10px;margin:8px 0;background:#2a2a2a;border:1px solid #444;color:#fff;border-radius:6px}
button{padding:12px 20px;border:none;border-radius:8px;font-size:16px;cursor:pointer;margin:5px;transition:0.2s}
.btn-on{background:#2a7a2a;color:white}.btn-on:hover{background:#3a9a3a}
.btn-off{background:#7a2a2a;color:white}.btn-off:hover{background:#9a3a3a}
.btn-save{background:#2a4a7a;color:white}.btn-save:hover{background:#3a5a9a}
.row{display:flex;gap:10px;flex-wrap:wrap}.row button{flex:1}
label{color:#aaa;font-size:14px}
.log{background:#111;padding:10px;border-radius:6px;font-family:monospace;font-size:12px;max-height:200px;overflow-y:auto;white-space:pre-wrap}
.secret-box{display:flex;gap:10px}.secret-box input{flex:1}
small{color:#888}
</style></head><body><div class="container">
<h1>🛡️ Админ панель (Cloudflare)</h1>
<div class="card">
<label>Admin Secret:</label>
<div class="secret-box"><input type="password" id="secret" placeholder="ADMIN_SECRET" value="${secretFromUrl}"><button onclick="toggleSecret()" style="padding:10px">👁️</button></div>
<small>Секрет из wrangler secret put ADMIN_SECRET</small>
</div>
<div class="card">
<div id="status" class="status">Загрузка...</div>
<div id="details" style="font-size:14px;color:#aaa"></div>
<label>Сообщение:</label><textarea id="message" rows="2"></textarea>
<div class="row"><div style="flex:1"><label>Версия:</label><input type="text" id="version"></div><div style="flex:1"><label>Мин версия:</label><input type="text" id="min_version"></div></div>
<div class="row" style="margin-top:15px"><button class="btn-on" onclick="setEnabled(true)">✅ ВКЛЮЧИТЬ</button><button class="btn-off" onclick="setEnabled(false)">❌ ВЫКЛЮЧИТЬ</button></div>
<button class="btn-save" style="width:100%;margin-top:10px" onclick="saveAll()">💾 Сохранить</button>
</div>
<div class="card"><h3>Быстрые действия</h3><div class="row">
<button onclick="setMessage('Cheat disabled by admin - maintenance')" class="btn-off">Тех работы</button>
<button onclick="setMessage('Cheat disabled - new version required')" class="btn-off">Обнова нужна</button>
<button onclick="setMessage('Cheat active - have fun!')" class="btn-on">Активен</button>
</div></div>
<div class="card"><h3>Лог</h3><div id="log" class="log">Готов...</div></div>
<div class="card"><small>Cloudflare Workers - бесплатно 100k запросов/день, глобальная сеть, мгновенное выключение<br>API: <code>/api/status</code> | <code>/api/check?hwid=</code> | <code>POST /api/admin/status</code></small></div>
</div>
<script>
let currentState=null;
function getSecret(){return document.getElementById('secret').value.trim()}
function log(m){const e=document.getElementById('log');e.textContent='['+new Date().toLocaleTimeString()+'] '+m+'\\n'+e.textContent}
function toggleSecret(){const i=document.getElementById('secret');i.type=i.type==='password'?'text':'password'}
async function loadState(){const s=getSecret();if(!s){document.getElementById('status').textContent='Введи SECRET';document.getElementById('status').className='status disabled';return}
try{const r=await fetch('/api/admin/state?secret='+encodeURIComponent(s));const d=await r.json();if(!r.ok)throw new Error(d.error||'Failed');currentState=d;renderState(d);log('Загружено: enabled='+d.enabled)}catch(e){document.getElementById('status').textContent='Ошибка: '+e.message;document.getElementById('status').className='status disabled';log('Ошибка: '+e.message)}}
function renderState(s){const st=document.getElementById('status');st.textContent=s.enabled?'✅ ЧИТ ВКЛЮЧЕН':'❌ ЧИТ ВЫКЛЮЧЕН';st.className='status '+(s.enabled?'enabled':'disabled');document.getElementById('details').innerHTML='Версия: '+s.version+' | Мин: '+s.min_version+'<br>Обновлено: '+new Date(s.updated_at).toLocaleString()+'<br>Проверок: '+s.total_checks+' | Отклонено: '+s.disabled_count+'<br>Сообщение: '+s.message;document.getElementById('message').value=s.message;document.getElementById('version').value=s.version;document.getElementById('min_version').value=s.min_version}
async function apiUpdate(p){const sec=getSecret();if(!sec){alert('Введи SECRET!');return null}try{const r=await fetch('/api/admin/status',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({secret:sec,...p})});const d=await r.json();if(!r.ok)throw new Error(d.error||'Failed');currentState=d.state;renderState(d.state);log('Обновлено: '+JSON.stringify(p));return d.state}catch(e){alert('Ошибка: '+e.message);log('Ошибка: '+e.message);return null}}
function setEnabled(e){const m=document.getElementById('message').value||(e?'Cheat active':'Cheat disabled by admin');apiUpdate({enabled:e,message:m})}
function setMessage(m){document.getElementById('message').value=m;apiUpdate({message:m})}
function saveAll(){apiUpdate({message:document.getElementById('message').value,version:document.getElementById('version').value,min_version:document.getElementById('min_version').value})}
window.onload=()=>{if(document.getElementById('secret').value)loadState();setInterval(()=>{if(getSecret())loadState()},10000)};document.getElementById('secret').addEventListener('change',loadState);
</script></body></html>`);
    }

    // GET / - info
    if (path === "/" || path === "") {
      const state = await getState(env);
      return htmlResponse(`
        <html><head><title>Cheat Kill-Switch</title><style>
          body{font-family:Arial,sans-serif;max-width:800px;margin:40px auto;padding:20px;background:#111;color:#eee}
          .status{padding:20px;border-radius:10px;margin:20px 0;font-size:18px}
          .enabled{background:#1a3a1a;border:2px solid #2a5a2a}
          .disabled{background:#3a1a1a;border:2px solid #5a2a2a}
          a{color:#6af} code{background:#222;padding:2px 6px;border-radius:4px}
          .btn{display:inline-block;padding:10px 20px;background:#2a5a9a;color:white;text-decoration:none;border-radius:5px;margin:5px}
        </style></head><body>
          <h1>🛡️ Cheat Kill-Switch (Cloudflare Workers)</h1>
          <div class="status ${state.enabled ? 'enabled' : 'disabled'}">
            <b>Статус:</b> ${state.enabled ? '✅ ВКЛЮЧЕН' : '❌ ВЫКЛЮЧЕН'}<br>
            <b>Сообщение:</b> ${state.message}<br>
            <b>Версия:</b> ${state.version}<br>
            <b>Обновлено:</b> ${state.updated_at}<br>
            <b>Проверок:</b> ${state.total_checks}
          </div>
          <h3>API:</h3>
          <ul>
            <li><code>GET /api/status</code> - статус для чита</li>
            <li><code>GET /api/check?hwid=XXX&version=1.0</code> - с HWID</li>
            <li><code>POST /api/admin/status</code> - управление</li>
            <li><code>GET /admin?secret=xxx</code> - админ панель</li>
          </ul>
          <a class="btn" href="/admin?secret=">Админ панель</a>
          <p><small>Cloudflare Workers - бесплатно 100k req/day, глобально, <10ms</small></p>
        </body></html>
      `);
    }

    return jsonResponse({ error: "Not found", path }, 404);
  }
};
