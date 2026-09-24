/**
 * Cheat Kill-Switch Backend - Render.com version
 * Free tier compatible, no DB needed (uses JSON file + memory)
 * 
 * Endpoints:
 *  GET  /api/status              -> current status
 *  GET  /api/check?hwid=&ver=    -> status + logging
 *  POST /api/admin/status        -> update status (needs secret)
 *  GET  /admin?secret=xxx        -> admin panel
 *  GET  /                        -> info page
 */

const express = require('express');
const cors = require('cors');
const fs = require('fs');
const path = require('path');

const app = express();
const PORT = process.env.PORT || 10000;

// CONFIG - поменяй эти значения!
const ADMIN_SECRET = process.env.ADMIN_SECRET || 'CHANGE_ME_SUPER_SECRET_12345';
const DEFAULT_ENABLED = process.env.CHEAT_ENABLED !== 'false'; // true by default
const DATA_FILE = path.join(__dirname, 'data.json');

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// Load / Save state
function loadState() {
  try {
    if (fs.existsSync(DATA_FILE)) {
      const data = JSON.parse(fs.readFileSync(DATA_FILE, 'utf8'));
      return {
        enabled: typeof data.enabled === 'boolean' ? data.enabled : DEFAULT_ENABLED,
        message: data.message || (DEFAULT_ENABLED ? 'Cheat active' : 'Cheat disabled by admin'),
        version: data.version || '1.0.0',
        min_version: data.min_version || '1.0.0',
        maintenance: !!data.maintenance,
        updated_at: data.updated_at || new Date().toISOString(),
        total_checks: data.total_checks || 0,
        disabled_count: data.disabled_count || 0
      };
    }
  } catch (e) {
    console.error('Failed to load data.json:', e.message);
  }
  return {
    enabled: DEFAULT_ENABLED,
    message: DEFAULT_ENABLED ? 'Cheat active' : 'Cheat disabled by admin',
    version: '1.0.0',
    min_version: '1.0.0',
    maintenance: false,
    updated_at: new Date().toISOString(),
    total_checks: 0,
    disabled_count: 0
  };
}

function saveState(state) {
  try {
    fs.writeFileSync(DATA_FILE, JSON.stringify(state, null, 2));
  } catch (e) {
    console.error('Failed to save data.json:', e.message);
  }
}

let state = loadState();

// Middleware log
app.use((req, res, next) => {
  console.log(`[${new Date().toISOString()}] ${req.method} ${req.path} - IP: ${req.ip} - UA: ${req.get('User-Agent')?.substring(0, 80)}`);
  next();
});

// GET /api/status - главный эндпоинт для чита
app.get('/api/status', (req, res) => {
  state.total_checks++;
  if (!state.enabled) state.disabled_count++;
  // Save every 10 checks to avoid too much IO on free tier
  if (state.total_checks % 10 === 0) saveState(state);

  res.json({
    enabled: state.enabled,
    message: state.message,
    version: state.version,
    min_version: state.min_version,
    maintenance: state.maintenance,
    timestamp: Math.floor(Date.now() / 1000),
    server_time: new Date().toISOString()
  });
});

// GET /api/check - с логом HWID и версии
app.get('/api/check', (req, res) => {
  const { hwid, version, user } = req.query;
  state.total_checks++;
  if (!state.enabled) state.disabled_count++;
  if (state.total_checks % 10 === 0) saveState(state);

  if (hwid) {
    console.log(`Check from HWID=${hwid} version=${version || 'unknown'} user=${user || 'unknown'} -> enabled=${state.enabled}`);
  }

  // Можно добавить бан по HWID в будущем
  // if (banned_hwids.includes(hwid)) return res.json({enabled: false, message: "HWID banned"});

  // Проверка минимальной версии
  if (state.min_version && version && version !== state.min_version) {
    // Простая проверка строкой, можно улучшить до semver
    // Если версия клиента ниже минимальной - можно отключить
    // Сейчас просто отдаем статус, но логируем
    console.log(`Version mismatch: client=${version} min=${state.min_version}`);
  }

  res.json({
    enabled: state.enabled,
    message: state.message,
    version: state.version,
    min_version: state.min_version,
    maintenance: state.maintenance,
    timestamp: Math.floor(Date.now() / 1000),
    server_time: new Date().toISOString()
  });
});

// POST /api/admin/status - управление
app.post('/api/admin/status', (req, res) => {
  const { secret, enabled, message, version, min_version, maintenance } = req.body;

  if (secret !== ADMIN_SECRET) {
    return res.status(403).json({ error: 'Invalid secret' });
  }

  if (typeof enabled === 'boolean') state.enabled = enabled;
  if (typeof message === 'string') state.message = message;
  if (typeof version === 'string') state.version = version;
  if (typeof min_version === 'string') state.min_version = min_version;
  if (typeof maintenance === 'boolean') state.maintenance = maintenance;

  state.updated_at = new Date().toISOString();
  saveState(state);

  console.log(`ADMIN UPDATE: enabled=${state.enabled} message=${state.message} by IP ${req.ip}`);

  res.json({
    success: true,
    state: state
  });
});

// GET /api/admin/state - посмотреть состояние (с секретом)
app.get('/api/admin/state', (req, res) => {
  const secret = req.query.secret || req.headers['x-admin-secret'];
  if (secret !== ADMIN_SECRET) {
    return res.status(403).json({ error: 'Invalid secret' });
  }
  res.json(state);
});

// GET / - info
app.get('/', (req, res) => {
  res.send(`
    <html><head><title>Cheat Kill-Switch</title><style>
      body{font-family:Arial,sans-serif;max-width:800px;margin:40px auto;padding:20px;background:#111;color:#eee}
      .status{padding:20px;border-radius:10px;margin:20px 0;font-size:18px}
      .enabled{background:#1a3a1a;border:2px solid #2a5a2a}
      .disabled{background:#3a1a1a;border:2px solid #5a2a2a}
      a{color:#6af} code{background:#222;padding:2px 6px;border-radius:4px}
      .btn{display:inline-block;padding:10px 20px;background:#2a5a9a;color:white;text-decoration:none;border-radius:5px;margin:5px}
    </style></head><body>
      <h1>🛡️ Cheat Kill-Switch Server</h1>
      <div class="status ${state.enabled ? 'enabled' : 'disabled'}">
        <b>Статус:</b> ${state.enabled ? '✅ ВКЛЮЧЕН (чит работает)' : '❌ ВЫКЛЮЧЕН (чит не работает)'}<br>
        <b>Сообщение:</b> ${state.message}<br>
        <b>Версия:</b> ${state.version}<br>
        <b>Обновлено:</b> ${state.updated_at}<br>
        <b>Проверок всего:</b> ${state.total_checks} | <b>Отклонено:</b> ${state.disabled_count}
      </div>
      <h3>API Endpoints:</h3>
      <ul>
        <li><code>GET /api/status</code> - статус для чита</li>
        <li><code>GET /api/check?hwid=XXX&version=1.0</code> - статус с логом</li>
        <li><code>POST /api/admin/status</code> - управление (нужен secret)</li>
        <li><code>GET /admin?secret=YOUR_SECRET</code> - админ панель</li>
      </ul>
      <a class="btn" href="/admin?secret=${ADMIN_SECRET === 'CHANGE_ME_SUPER_SECRET_12345' ? '' : ADMIN_SECRET}">Админ панель</a>
      <p><small>Render.com free tier - сервер засыпает через 15 мин неактивности, первый запрос после сна ~30 сек</small></p>
    </body></html>
  `);
});

app.listen(PORT, '0.0.0.0', () => {
  console.log(`Server running on port ${PORT}`);
  console.log(`Admin secret: ${ADMIN_SECRET === 'CHANGE_ME_SUPER_SECRET_12345' ? '!!! DEFAULT - CHANGE IT !!!' : '*** set ***'}`);
  console.log(`Initial state: enabled=${state.enabled}`);
});
