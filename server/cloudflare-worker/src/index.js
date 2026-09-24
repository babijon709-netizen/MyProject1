/**
 * Cheat Kill-Switch L3 - MINIMAL FIXED VERSION
 * Гарантированно работает, без сложного HTML
 */

let memoryState = {
  enabled: false,
  message: "Cheat disabled by admin L3",
  version: "1.0.0",
  min_version: "1.0.0",
  maintenance: false,
  updated_at: new Date().toISOString(),
  total_checks: 0
};

const ENCRYPTION_KEY = "BenzWare1337Key!";
const HMAC_SECRET = "BenzWare1337Key!";

function xorEncrypt(data, key) {
  let out = "";
  for (let i=0;i<data.length;i++) {
    out += String.fromCharCode(data.charCodeAt(i) ^ key.charCodeAt(i % key.length) ^ ((i*11) & 0xFF) ^ ((i>>3) & 0xFF));
  }
  return out;
}
function base64EncodeBinary(str) {
  try {
    const bytes = new Uint8Array(str.length);
    for (let i=0;i<str.length;i++) bytes[i] = str.charCodeAt(i) & 0xFF;
    let binary = "";
    for (let i=0;i<bytes.length;i++) binary += String.fromCharCode(bytes[i]);
    return btoa(binary);
  } catch(e) { return ""; }
}
function simpleHmac(data, key) {
  let hash = 0x811C9DC5;
  for (let i=0;i<data.length;i++) {
    hash ^= data.charCodeAt(i) ^ key.charCodeAt(i % key.length);
    hash = Math.imul(hash, 0x01000193);
    hash ^= hash>>16;
  }
  for (let i=0;i<key.length;i++) {
    hash ^= key.charCodeAt(i);
    hash = Math.imul(hash, 0x01000193);
  }
  return (hash>>>0).toString(16).toUpperCase().padStart(8,'0');
}
function encryptResponse(jsonStr) {
  try {
    const xored = xorEncrypt(jsonStr, ENCRYPTION_KEY);
    const b64 = base64EncodeBinary(xored);
    if (!b64) return null;
    const sig = simpleHmac(b64, HMAC_SECRET);
    return { enc: b64, sig: sig };
  } catch(e) { return null; }
}

export default {
  async fetch(request, env, ctx) {
    try {
      const url = new URL(request.url);
      const path = url.pathname;

      if (request.method === "OPTIONS") {
        return new Response(null, { headers: { "Access-Control-Allow-Origin": "*" } });
      }

      // Load state from KV if available
      let state = memoryState;
      try {
        if (env.CHEAT_KV) {
          const stored = await env.CHEAT_KV.get("cheat_state", "json");
          if (stored) state = stored;
        }
      } catch(e) {}

      // Admin secret
      const adminSecret = env.ADMIN_SECRET || "CHANGE_ME_SUPER_SECRET_12345";

      // /api/status
      if (path === "/api/status") {
        state.total_checks++;
        const plain = {
          enabled: state.enabled,
          message: state.message,
          version: state.version,
          min_version: state.min_version,
          timestamp: Math.floor(Date.now()/1000),
          level: 3
        };
        if (url.searchParams.get("enc") === "1") {
          const enc = encryptResponse(JSON.stringify(plain));
          if (enc) return new Response(JSON.stringify(enc), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
        }
        return new Response(JSON.stringify(plain), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
      }

      // /api/check
      if (path === "/api/check") {
        state.total_checks++;
        const plain = {
          enabled: state.enabled,
          message: state.message,
          version: state.version,
          min_version: state.min_version,
          timestamp: Math.floor(Date.now()/1000),
          hwid_allowed: true,
          level: 3
        };
        const enc = encryptResponse(JSON.stringify(plain));
        if (enc) return new Response(JSON.stringify(enc), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
        return new Response(JSON.stringify(plain), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
      }

      // Admin toggle
      if (path === "/api/admin/status" && request.method === "POST") {
        let body;
        try { body = await request.json(); } catch { return new Response(JSON.stringify({error:"Invalid JSON"}), { status:400, headers: { "Content-Type": "application/json" } }); }
        const secret = body.secret || request.headers.get("X-Admin-Secret") || url.searchParams.get("secret");
        if (secret !== adminSecret) return new Response(JSON.stringify({error:"Invalid secret"}), { status:403, headers: { "Content-Type": "application/json" } });
        if (typeof body.enabled === "boolean") state.enabled = body.enabled;
        if (typeof body.message === "string") state.message = body.message;
        state.updated_at = new Date().toISOString();
        memoryState = state;
        try { if (env.CHEAT_KV) await env.CHEAT_KV.put("cheat_state", JSON.stringify(state)); } catch(e) {}
        return new Response(JSON.stringify({success:true, state}), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
      }

      if (path === "/api/admin/state") {
        const secret = url.searchParams.get("secret") || request.headers.get("X-Admin-Secret");
        if (secret !== adminSecret) return new Response(JSON.stringify({error:"Invalid secret"}), { status:403, headers: { "Content-Type": "application/json" } });
        return new Response(JSON.stringify(state), { headers: { "Content-Type": "application/json" } });
      }

      // Admin panel - simple
      if (path === "/admin") {
        const secretFromUrl = url.searchParams.get("secret") || "";
        const html = `<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Admin L3</title><style>body{font-family:Arial;background:#111;color:#eee;padding:20px;max-width:600px;margin:0 auto}.card{background:#222;padding:20px;border-radius:10px;margin:20px 0}.enabled{color:#8f8}.disabled{color:#f88}button{padding:12px 20px;margin:5px;border:none;border-radius:8px;cursor:pointer}.on{background:#2a7a2a;color:#fff}.off{background:#7a2a2a;color:#fff}input,textarea{width:100%;padding:10px;background:#333;color:#fff;border:1px solid #555;border-radius:6px}</style></head><body>
<h1>Admin L3</h1>
<div class="card"><label>Secret:</label><input type="password" id="sec" value="${secretFromUrl}"><button onclick="document.getElementById('sec').type=document.getElementById('sec').type==='password'?'text':'password'">👁️</button></div>
<div class="card"><div id="st">Loading...</div><div id="det"></div><label>Message:</label><textarea id="msg" rows="2"></textarea><br><button class="on" onclick="setE(true)">✅ ВКЛЮЧИТЬ</button><button class="off" onclick="setE(false)">❌ ВЫКЛЮЧИТЬ</button></div>
<script>
function getS(){return document.getElementById('sec').value}
async function load(){const s=getS();if(!s){document.getElementById('st').innerText='Enter secret';return}try{const r=await fetch('/api/admin/state?secret='+encodeURIComponent(s));const d=await r.json();if(!r.ok)throw new Error(d.error);document.getElementById('st').innerText=d.enabled?'✅ ВКЛЮЧЕН':'❌ ВЫКЛЮЧЕН';document.getElementById('st').className=d.enabled?'enabled':'disabled';document.getElementById('det').innerText='Checks: '+d.total_checks+' | Msg: '+d.message;document.getElementById('msg').value=d.message}catch(e){document.getElementById('st').innerText='Error: '+e.message}}
async function setE(e){const s=getS();const m=document.getElementById('msg').value||(e?'active L3':'disabled L3');try{const r=await fetch('/api/admin/status',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({secret:s,enabled:e,message:m})});const d=await r.json();if(!r.ok)throw new Error(d.error);load()}catch(e){alert(e.message)}}
window.onload=load;
</script></body></html>`;
        return new Response(html, { headers: { "Content-Type": "text/html; charset=utf-8" } });
      }

      // Root
      if (path === "/" || path === "") {
        const html = `<html><body style="font-family:Arial;background:#111;color:#eee;padding:20px"><h1>Kill-Switch L3 ${state.enabled?'✅ ВКЛ':'❌ ВЫКЛ'}</h1><p>Enabled: ${state.enabled}</p><p>Message: ${state.message}</p><p>Checks: ${state.total_checks}</p><a href="/admin?secret=">Admin</a> | <a href="/api/status">API plain</a> | <a href="/api/status?enc=1">API enc</a></body></html>`;
        return new Response(html, { headers: { "Content-Type": "text/html" } });
      }

      return new Response(JSON.stringify({error:"Not found", path}), { status:404, headers: { "Content-Type": "application/json" } });
    } catch(err) {
      return new Response(JSON.stringify({error: err.message, enabled: false, message: "error fallback - disabled", level:3}), { status:200, headers: { "Content-Type": "application/json" } });
    }
  }
};
