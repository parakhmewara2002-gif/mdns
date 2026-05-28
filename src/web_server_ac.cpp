// ============================================================
//  web_server_ac.cpp  —  AC Detector Routes + /ac Page
//
//  Routes:
//    GET  /ac                   — Full AC Detector UI page
//    GET  /api/ac/status        — Live RMS + detection state
//    GET  /api/ac/config        — Current config
//    POST /api/ac/config        — Save config
//    GET  /api/ac/pinmap        — GPIO pin status map
//    POST /api/ac/enable        — { "enabled": true/false }
// ============================================================
#include "web_server.h"
#include "ac_detector.h"
#include "auth_manager.h"
#include <ArduinoJson.h>

static void _acSend(AsyncWebServerRequest* req, int code, const String& json) {
    AsyncWebServerResponse* r = req->beginResponse(code, "application/json", json);
    r->addHeader("Access-Control-Allow-Origin", "*");
    r->addHeader("Access-Control-Allow-Headers", "Content-Type");
    req->send(r);
}

// ─────────────────────────────────────────────────────────────
//  /ac  — Standalone UI page (inline HTML, no LittleFS needed)
// ─────────────────────────────────────────────────────────────
static const char AC_PAGE[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AC Detector — IR Remote</title>
<style>
  :root {
    --bg:#0f1117; --card:#1a1d27; --border:#2a2d3e;
    --accent:#00d4ff; --green:#00e676; --red:#ff5252;
    --yellow:#ffd740; --text:#e0e0e0; --muted:#888;
  }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { background:var(--bg); color:var(--text); font-family:'Segoe UI',system-ui,sans-serif;
         min-height:100vh; padding:16px; }
  header { display:flex; align-items:center; gap:12px; margin-bottom:20px; }
  header a { color:var(--accent); text-decoration:none; font-size:14px; }
  header h1 { font-size:20px; font-weight:600; }
  .grid { display:grid; grid-template-columns:1fr 1fr; gap:16px; }
  @media(max-width:600px){ .grid{ grid-template-columns:1fr; } }
  .card { background:var(--card); border:1px solid var(--border);
          border-radius:12px; padding:20px; }
  .card h2 { font-size:13px; text-transform:uppercase; letter-spacing:.08em;
             color:var(--muted); margin-bottom:16px; }

  /* Detection badge */
  .detect-badge { display:flex; align-items:center; justify-content:center;
    gap:10px; padding:18px; border-radius:10px; font-size:18px; font-weight:700;
    margin-bottom:16px; transition:all .3s; }
  .detect-badge.live { background:rgba(0,230,118,.12); border:2px solid var(--green); color:var(--green); }
  .detect-badge.idle { background:rgba(136,136,136,.08); border:2px solid var(--border); color:var(--muted); }
  .dot { width:12px; height:12px; border-radius:50%; }
  .dot.live { background:var(--green); animation:pulse 1s infinite; }
  .dot.idle { background:var(--muted); }
  @keyframes pulse { 0%,100%{opacity:1} 50%{opacity:.3} }

  /* RMS bar */
  .rms-label { display:flex; justify-content:space-between; font-size:13px; margin-bottom:6px; }
  .bar-track { background:#111; border-radius:8px; height:22px; overflow:hidden; position:relative; }
  .bar-fill  { height:100%; border-radius:8px; transition:width .2s, background .3s;
               background:linear-gradient(90deg,var(--accent),var(--green)); }
  .bar-fill.hot { background:linear-gradient(90deg,var(--yellow),var(--red)); }
  .threshold-mark { position:absolute; top:0; bottom:0; width:2px; background:var(--yellow);
                    opacity:.7; }

  /* Signal bars */
  .bars { display:flex; gap:4px; align-items:flex-end; height:36px; margin-top:12px; }
  .bars span { flex:1; border-radius:3px 3px 0 0; transition:height .2s, background .3s;
               background:var(--border); min-height:4px; }
  .bars span.active { background:var(--accent); }
  .bars span.hot    { background:var(--green); }

  /* Pin map */
  .pin-grid { display:grid; grid-template-columns:1fr 1fr; gap:10px; }
  .pin-card { border-radius:8px; padding:12px 14px; border:2px solid transparent;
              position:relative; cursor:default; }
  .pin-card.active  { border-color:var(--accent); background:rgba(0,212,255,.08); }
  .pin-card.free    { border-color:var(--border);  background:rgba(255,255,255,.02); }
  .pin-card.conflict{ border-color:var(--red);     background:rgba(255,82,82,.05);
                      opacity:.6; }
  .pin-card .gpio   { font-size:22px; font-weight:700; letter-spacing:-.5px; }
  .pin-card .pname  { font-size:11px; color:var(--muted); margin-top:2px; }
  .pin-card .plabel { font-size:11px; margin-top:6px; }
  .pin-card.active  .plabel { color:var(--accent); }
  .pin-card.free    .plabel { color:var(--green);  }
  .pin-card.conflict .plabel{ color:var(--red);   }
  .badge-default { position:absolute; top:8px; right:8px; font-size:9px;
    background:var(--accent); color:#000; padding:2px 6px; border-radius:4px;
    font-weight:700; letter-spacing:.05em; }

  /* Wiring diagram */
  .wire-box { background:#111; border-radius:10px; padding:16px;
              font-family:'Courier New',monospace; font-size:12px;
              line-height:1.8; color:#ccc; }
  .wire-box .hl  { color:var(--accent); font-weight:700; }
  .wire-box .hl2 { color:var(--green);  font-weight:700; }
  .wire-box .hl3 { color:var(--yellow); }

  /* Controls */
  .row    { display:flex; gap:10px; align-items:center; margin-bottom:12px; flex-wrap:wrap; }
  label   { font-size:13px; color:var(--muted); min-width:90px; }
  select, input[type=number] {
    background:#111; border:1px solid var(--border); color:var(--text);
    border-radius:6px; padding:7px 10px; font-size:13px; flex:1; min-width:80px; }
  .toggle { display:flex; align-items:center; gap:10px; }
  .sw { position:relative; width:46px; height:24px; cursor:pointer; }
  .sw input { opacity:0; width:0; height:0; }
  .sw-track { position:absolute; inset:0; border-radius:12px; background:var(--border);
              transition:.3s; }
  .sw input:checked + .sw-track { background:var(--accent); }
  .sw-track:before { content:''; position:absolute; width:18px; height:18px;
    left:3px; top:3px; border-radius:50%; background:#fff; transition:.3s; }
  .sw input:checked + .sw-track:before { transform:translateX(22px); }
  .btn { padding:9px 18px; border-radius:8px; border:none; cursor:pointer;
         font-size:13px; font-weight:600; transition:.2s; }
  .btn-primary { background:var(--accent); color:#000; }
  .btn-primary:hover { opacity:.85; }
  .btn-danger  { background:var(--red); color:#fff; }
  .btn-outline { background:transparent; border:1px solid var(--border); color:var(--text); }
  .btn-outline:hover { border-color:var(--accent); color:var(--accent); }

  /* Stats */
  .stat-row { display:flex; justify-content:space-between; padding:8px 0;
              border-bottom:1px solid var(--border); font-size:13px; }
  .stat-row:last-child { border:none; }
  .stat-val  { font-weight:600; color:var(--text); }
  .stat-lbl  { color:var(--muted); }

  /* Threshold slider */
  input[type=range] { flex:1; accent-color:var(--accent); }
  .range-val { min-width:42px; text-align:right; font-size:13px; color:var(--accent); font-weight:600; }
</style>
</head>
<body>

<header>
  <a href="/">← IR Remote</a>
  <h1>⚡ AC Power Detector</h1>
  <div style="margin-left:auto; display:flex; gap:8px; align-items:center;">
    <span id="connDot" style="width:8px;height:8px;border-radius:50%;background:var(--border);display:inline-block;"></span>
    <span id="connLbl" style="font-size:12px;color:var(--muted)">Connecting...</span>
  </div>
</header>

<div class="grid">

  <!-- ── LIVE STATUS ── -->
  <div class="card" style="grid-column:1/-1">
    <div id="detectBadge" class="detect-badge idle">
      <div class="dot idle" id="dot"></div>
      <span id="detectText">No AC Detected</span>
    </div>

    <div class="rms-label">
      <span>Signal Strength (RMS)</span>
      <span id="rmsVal">0.0</span>
    </div>
    <div class="bar-track">
      <div class="bar-fill" id="barFill" style="width:0%"></div>
      <div class="threshold-mark" id="threshMark" style="left:2%"></div>
    </div>
    <div style="display:flex;justify-content:flex-end;margin-top:4px;font-size:11px;color:var(--muted)">
      <span>▲ threshold</span>
    </div>

    <div class="bars" id="signalBars">
      <span></span><span></span><span></span><span></span><span></span>
    </div>
  </div>

  <!-- ── GPIO PIN MAP ── -->
  <div class="card">
    <h2>GPIO Pin Map — Jumper Wire Lagao</h2>
    <div class="pin-grid" id="pinGrid">
      <!-- filled by JS -->
    </div>
    <p style="font-size:12px;color:var(--muted);margin-top:12px;">
      ⚡ Active pin pe jumper wire lagao (antenna). GND wire uske around twist karo shielding ke liye.
    </p>
  </div>

  <!-- ── WIRING DIAGRAM ── -->
  <div class="card">
    <h2>Wiring Diagram</h2>
    <div class="wire-box" id="wiringBox">
      <div>┌─────────────────────────┐</div>
      <div>│   ESP32-WROOM-32        │</div>
      <div>│                         │</div>
      <div>│  <span class="hl">GPIO36 (VP) ●─────────── Jumper Wire (Antenna)</span></div>
      <div>│                         │      ↑ ~10cm length</div>
      <div>│  <span class="hl2">GND         ●─────────── GND Wire (Shield)</span></div>
      <div>│                         │      ↑ twist around antenna</div>
      <div>│                         │        tip only khuli rakho</div>
      <div>└─────────────────────────┘</div>
      <div style="margin-top:12px;color:var(--muted)">
        <span class="hl3">Tip:</span> GND wire = antenna se 2cm lamba<br>
        Sirf antenna ki aage wali tip khuli rakho<br>
        Baaki sab GND mein → false positives kam
      </div>
    </div>
  </div>

  <!-- ── CONFIG ── -->
  <div class="card">
    <h2>Configuration</h2>

    <div class="row">
      <label>Module</label>
      <div class="toggle">
        <label class="sw">
          <input type="checkbox" id="swEnable" onchange="toggleEnable()">
          <div class="sw-track"></div>
        </label>
        <span id="enableLbl" style="font-size:13px">Disabled</span>
      </div>
    </div>

    <div class="row">
      <label>GPIO Pin</label>
      <select id="selPin" onchange="updateWiring()">
        <option value="36">GPIO 36 (VP) — Default</option>
        <option value="39">GPIO 39 (VN)</option>
        <option value="34">GPIO 34</option>
        <option value="35">GPIO 35</option>
      </select>
    </div>

    <div class="row">
      <label>AC Freq</label>
      <select id="selFreq">
        <option value="50">50 Hz (India/Europe)</option>
        <option value="60">60 Hz (USA/Japan)</option>
      </select>
    </div>

    <div class="row" style="align-items:flex-start;flex-direction:column;gap:6px">
      <div style="display:flex;justify-content:space-between;width:100%">
        <label style="min-width:unset">Detection Threshold</label>
        <span class="range-val" id="threshDisplay">80</span>
      </div>
      <input type="range" id="slThreshold" min="10" max="500" value="80"
             oninput="document.getElementById('threshDisplay').textContent=this.value">
      <p style="font-size:11px;color:var(--muted)">Kam karo = zyada sensitive. Zyada karo = false positives kam.</p>
    </div>

    <div class="row" style="align-items:flex-start;flex-direction:column;gap:6px">
      <div style="display:flex;justify-content:space-between;width:100%">
        <label style="min-width:unset">Hysteresis</label>
        <span class="range-val" id="hystDisplay">15</span>
      </div>
      <input type="range" id="slHysteresis" min="5" max="100" value="15"
             oninput="document.getElementById('hystDisplay').textContent=this.value">
    </div>

    <div class="row">
      <label>Buzzer Pin</label>
      <input type="number" id="buzzerPin" value="0" min="0" max="39" placeholder="0 = disabled">
    </div>

    <div class="row">
      <label>Buzzer</label>
      <div class="toggle">
        <label class="sw">
          <input type="checkbox" id="swBuzzer">
          <div class="sw-track"></div>
        </label>
        <span style="font-size:13px">Beep on detection</span>
      </div>
    </div>

    <div style="display:flex;gap:10px;margin-top:8px">
      <button class="btn btn-primary" onclick="saveConfig()">Save Config</button>
      <button class="btn btn-outline" onclick="loadConfig()">Reset</button>
    </div>
  </div>

  <!-- ── STATS ── -->
  <div class="card">
    <h2>Detection Stats</h2>
    <div class="stat-row">
      <span class="stat-lbl">Total Detections</span>
      <span class="stat-val" id="statCount">0</span>
    </div>
    <div class="stat-row">
      <span class="stat-lbl">Last Detected</span>
      <span class="stat-val" id="statDetected">—</span>
    </div>
    <div class="stat-row">
      <span class="stat-lbl">Last Lost</span>
      <span class="stat-val" id="statLost">—</span>
    </div>
    <div class="stat-row">
      <span class="stat-lbl">Current RMS</span>
      <span class="stat-val" id="statRms">0.0</span>
    </div>
    <div class="stat-row">
      <span class="stat-lbl">Signal Bars</span>
      <span class="stat-val" id="statBars">0 / 5</span>
    </div>
    <div class="stat-row">
      <span class="stat-lbl">Active Pin</span>
      <span class="stat-val" id="statPin">GPIO 36</span>
    </div>

    <h2 style="margin-top:20px">AC Events</h2>
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px">
      AC events WebSocket broadcast karta hai aur audit log mein record hote hain.
    </p>
    <div style="display:flex;gap:8px;flex-wrap:wrap">
      <span style="font-size:12px;padding:4px 10px;border-radius:6px;
                   background:rgba(0,230,118,.1);border:1px solid var(--green);color:var(--green)">
        AC_DETECTED
      </span>
      <span style="font-size:12px;padding:4px 10px;border-radius:6px;
                   background:rgba(255,82,82,.1);border:1px solid var(--red);color:var(--red)">
        AC_LOST
      </span>
    </div>
  </div>

</div><!-- /grid -->

<script>
let pollTimer = null;
let threshold = 80;

// ── Init ──────────────────────────────────────────────────────
window.onload = () => {
  loadConfig();
  loadPinMap();
  startPolling();
};

// ── Polling ───────────────────────────────────────────────────
function startPolling() {
  poll();
  pollTimer = setInterval(poll, 500);
}

async function poll() {
  try {
    const r = await fetch('/api/ac/status');
    const d = await r.json();
    updateStatus(d);
    setConn(true);
  } catch(e) { setConn(false); }
}

function setConn(ok) {
  document.getElementById('connDot').style.background = ok ? 'var(--green)' : 'var(--red)';
  document.getElementById('connLbl').textContent      = ok ? 'Live' : 'Disconnected';
}

// ── Status update ─────────────────────────────────────────────
function updateStatus(d) {
  const detected = d.acDetected;
  const rms      = d.rms || 0;
  const pct      = d.rmsPercent || 0;
  threshold      = d.threshold || threshold;

  // Badge
  const badge = document.getElementById('detectBadge');
  const dot   = document.getElementById('dot');
  const dtxt  = document.getElementById('detectText');
  if (detected) {
    badge.className = 'detect-badge live';
    dot.className   = 'dot live';
    dtxt.textContent = '⚡ AC Detected!';
  } else {
    badge.className = 'detect-badge idle';
    dot.className   = 'dot idle';
    dtxt.textContent = d.enabled ? 'No AC Detected' : 'Module Disabled';
  }

  // Bar
  const bar = document.getElementById('barFill');
  bar.style.width = Math.min(100, pct).toFixed(1) + '%';
  bar.className = 'bar-fill' + (pct > 70 ? ' hot' : '');
  document.getElementById('rmsVal').textContent = rms.toFixed(1);

  // Threshold marker
  const threshPct = (threshold / 4095) * 100;
  document.getElementById('threshMark').style.left = threshPct.toFixed(1) + '%';

  // Signal bars
  const bars = document.querySelectorAll('#signalBars span');
  const active = d.signalBars || 0;
  bars.forEach((b, i) => {
    const filled = i < active;
    b.style.height = (20 + i * 6) + 'px';
    b.className = filled ? (active >= 4 ? 'hot' : 'active') : '';
  });

  // Stats
  document.getElementById('statCount').textContent    = d.detectionCount || 0;
  document.getElementById('statRms').textContent      = rms.toFixed(1);
  document.getElementById('statBars').textContent     = (d.signalBars || 0) + ' / 5';
  document.getElementById('statPin').textContent      = 'GPIO ' + (d.pin || 36);
  document.getElementById('statDetected').textContent =
    d.detectedAt ? msToTime(d.detectedAt) : '—';
  document.getElementById('statLost').textContent =
    d.lostAt ? msToTime(d.lostAt) : '—';
}

function msToTime(ms) {
  if (!ms) return '—';
  const ago = Math.floor((Date.now() - ms) / 1000);
  if (ago < 5)  return 'Just now';
  if (ago < 60) return ago + 's ago';
  return Math.floor(ago/60) + 'm ago';
}

// ── Pin Map ───────────────────────────────────────────────────
async function loadPinMap() {
  try {
    const r = await fetch('/api/ac/pinmap');
    const d = await r.json();
    renderPinMap(d.pins || []);
  } catch(e) {}
}

function renderPinMap(pins) {
  const grid = document.getElementById('pinGrid');
  grid.innerHTML = '';
  pins.forEach(p => {
    const div = document.createElement('div');
    div.className = 'pin-card ' + p.status;
    div.innerHTML =
      (p.isDefault ? '<div class="badge-default">DEFAULT</div>' : '') +
      '<div class="gpio">GPIO ' + p.gpio + '</div>' +
      '<div class="pname">' + p.name + '</div>' +
      '<div class="plabel">' +
        (p.status === 'active'   ? '✅ ' :
         p.status === 'conflict' ? '❌ ' : '🟢 ') +
        p.label + '</div>';
    grid.appendChild(div);
  });
}

// ── Wiring diagram update ─────────────────────────────────────
function updateWiring() {
  const pin = document.getElementById('selPin').value;
  const pinName = pin == 36 ? 'GPIO36 (VP)' :
                  pin == 39 ? 'GPIO39 (VN)' :
                  'GPIO' + pin;
  document.getElementById('wiringBox').innerHTML = `
<div>┌─────────────────────────┐</div>
<div>│   ESP32-WROOM-32        │</div>
<div>│                         │</div>
<div>│  <span class="hl">${pinName.padEnd(11)} ●─────────── Jumper Wire (Antenna)</span></div>
<div>│                         │      ↑ ~10cm length</div>
<div>│  <span class="hl2">GND         ●─────────── GND Wire (Shield)</span></div>
<div>│                         │      ↑ twist around antenna</div>
<div>│                         │        tip only khuli rakho</div>
<div>└─────────────────────────┘</div>
<div style="margin-top:12px;color:var(--muted)">
  <span class="hl3">Tip:</span> GND wire = antenna se 2cm lamba<br>
  Sirf antenna ki aage wali tip khuli rakho<br>
  Baaki sab GND mein → false positives kam
</div>`;
}

// ── Config ────────────────────────────────────────────────────
async function loadConfig() {
  try {
    const r = await fetch('/api/ac/config');
    const d = await r.json();
    document.getElementById('selPin').value       = d.pin || 36;
    document.getElementById('selFreq').value      = d.acFreq || 50;
    document.getElementById('slThreshold').value  = d.threshold || 80;
    document.getElementById('threshDisplay').textContent = d.threshold || 80;
    document.getElementById('slHysteresis').value = d.hysteresis || 15;
    document.getElementById('hystDisplay').textContent   = d.hysteresis || 15;
    document.getElementById('swEnable').checked   = d.enabled || false;
    document.getElementById('enableLbl').textContent = d.enabled ? 'Enabled' : 'Disabled';
    document.getElementById('swBuzzer').checked   = d.buzzerEnabled || false;
    document.getElementById('buzzerPin').value    = d.buzzerPin || 0;
    updateWiring();
  } catch(e) { console.error('loadConfig:', e); }
}

async function saveConfig() {
  const body = {
    pin:           parseInt(document.getElementById('selPin').value),
    acFreq:        parseInt(document.getElementById('selFreq').value),
    threshold:     parseInt(document.getElementById('slThreshold').value),
    hysteresis:    parseInt(document.getElementById('slHysteresis').value),
    enabled:       document.getElementById('swEnable').checked,
    buzzerEnabled: document.getElementById('swBuzzer').checked,
    buzzerPin:     parseInt(document.getElementById('buzzerPin').value),
  };
  try {
    const r = await fetch('/api/ac/config', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify(body)
    });
    const d = await r.json();
    if (d.ok) {
      showToast('Config saved ✓');
      loadPinMap();
    } else showToast('Save failed: ' + (d.error||'unknown'), true);
  } catch(e) { showToast('Error: ' + e.message, true); }
}

async function toggleEnable() {
  const en = document.getElementById('swEnable').checked;
  document.getElementById('enableLbl').textContent = en ? 'Enabled' : 'Disabled';
  try {
    await fetch('/api/ac/enable', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({enabled: en})
    });
    loadPinMap();
  } catch(e) {}
}

// ── Toast ─────────────────────────────────────────────────────
function showToast(msg, err=false) {
  const t = document.createElement('div');
  t.textContent = msg;
  t.style.cssText = `position:fixed;bottom:24px;right:24px;padding:12px 20px;
    border-radius:8px;font-size:14px;z-index:999;transition:.3s;
    background:${err?'var(--red)':'var(--green)'};color:#000;font-weight:600;`;
  document.body.appendChild(t);
  setTimeout(()=>{ t.style.opacity='0'; setTimeout(()=>t.remove(),300); }, 2500);
}
</script>
</body>
</html>)rawhtml";

// ─────────────────────────────────────────────────────────────
//  POST body buffer helper (same pattern as other batch files)
// ─────────────────────────────────────────────────────────────
static String* _getAcBuf(AsyncWebServerRequest* req) {
    if (!req->_tempObject) {
        req->_tempObject = new String();
        req->onDisconnect([req]() {
            if (req->_tempObject) {
                delete reinterpret_cast<String*>(req->_tempObject);
                req->_tempObject = nullptr;
            }
        });
    }
    return reinterpret_cast<String*>(req->_tempObject);
}
static void _freeAcBuf(AsyncWebServerRequest* req) {
    if (req->_tempObject) {
        delete reinterpret_cast<String*>(req->_tempObject);
        req->_tempObject = nullptr;
    }
}

#define AC_POST(path, handler) \
    _server.on(path, HTTP_POST, \
        [](AsyncWebServerRequest* req){}, \
        nullptr, \
        [this](AsyncWebServerRequest* req, uint8_t* d, size_t l, size_t i, size_t t) { \
            if (_getAcBuf(req)->length() + l > HTTP_MAX_BODY) { \
                _freeAcBuf(req); \
                _acSend(req, 413, "{\"error\":\"Request too large\"}"); return; \
            } \
            _getAcBuf(req)->concat((char*)d, l); \
            bool last = (t > 0) ? (i + l >= t) : (i == 0); \
            if (last) { \
                String* buf = _getAcBuf(req); \
                handler(req, (uint8_t*)buf->c_str(), buf->length()); \
                _freeAcBuf(req); \
            } \
        })

// ─────────────────────────────────────────────────────────────
void WebUI::setupAcRoutes() {

    // ── GET /ac  — full UI page ───────────────────────────────
    _server.on("/ac", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* r = req->beginResponse_P(
            200, "text/html",
            reinterpret_cast<const uint8_t*>(AC_PAGE),
            sizeof(AC_PAGE) - 1);
        r->addHeader("Cache-Control", "no-store");
        req->send(r);
    });

    // ── GET /api/ac/status ────────────────────────────────────
    _server.on("/api/ac/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            _acSend(req, 200, acDetector.statusJson());
        });

    // ── GET /api/ac/config ────────────────────────────────────
    _server.on("/api/ac/config", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            _acSend(req, 200, acDetector.configJson());
        });

    // ── GET /api/ac/pinmap ────────────────────────────────────
    _server.on("/api/ac/pinmap", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            _acSend(req, 200, acDetector.pinMapJson());
        });

    // ── POST /api/ac/enable ───────────────────────────────────
    AC_POST("/api/ac/enable",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            JsonDocument body;
            if (deserializeJson(body, d, l) != DeserializationError::Ok) {
                _acSend(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
            }
            bool en = body["enabled"] | false;
            acDetector.enable(en);
            _acSend(req, 200, "{\"ok\":true}");
        });

    // ── POST /api/ac/config ───────────────────────────────────
    AC_POST("/api/ac/config",
        [](AsyncWebServerRequest* req, uint8_t* d, size_t l) {
            JsonDocument body;
            if (deserializeJson(body, d, l) != DeserializationError::Ok) {
                _acSend(req, 400, "{\"error\":\"Invalid JSON\"}"); return;
            }
            AcDetectorConfig cfg = acDetector.getConfig();
            if (body["pin"].is<uint8_t>()) {
                uint8_t pin = body["pin"].as<uint8_t>();
                if (!acDetector.isPinValid(pin)) {
                    _acSend(req, 400, "{\"error\":\"Invalid pin — use 34/35/36/39\"}");
                    return;
                }
                cfg.pin = pin;
            }
            if (body["threshold"].is<uint16_t>())
                cfg.threshold = constrain((int)body["threshold"].as<uint16_t>(), 10, 2000);
            if (body["hysteresis"].is<uint16_t>())
                cfg.hysteresis = constrain((int)body["hysteresis"].as<uint16_t>(), 5, 500);
            if (body["enabled"].is<bool>())
                cfg.enabled = body["enabled"].as<bool>();
            if (body["buzzerEnabled"].is<bool>())
                cfg.buzzerEnabled = body["buzzerEnabled"].as<bool>();
            if (body["buzzerPin"].is<uint8_t>())
                cfg.buzzerPin = body["buzzerPin"].as<uint8_t>();
            if (body["acFreq"].is<uint8_t>()) {
                uint8_t f = body["acFreq"].as<uint8_t>();
                cfg.acFreq = (f == 60) ? 60 : 50;
            }
            acDetector.setConfig(cfg);
            _acSend(req, 200, "{\"ok\":true}");
        });

    Serial.println("[WEB] AC Detector routes: /ac  /api/ac/*");
}
