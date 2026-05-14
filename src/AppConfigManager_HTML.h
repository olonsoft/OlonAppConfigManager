#pragma once

// ============================================================
//  AppConfigManager_HTML.h
//  Single-page configuration portal — stored in PROGMEM.
//
//  Served by AppConfigManager via AsyncProgmemResponse.
//
//  Tabs:  WiFi | App | MQTT | System
//  Endpoints consumed:
//    GET  /status   -> populate form on load
//    GET  /scan     -> trigger + poll WiFi scan
//    POST /save     -> JSON body, JSON response
//    GET  /exit     -> close web portal (web-portal mode only)
// ============================================================

static const char htmlContent[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Device Configuration</title>
<style>
  /* ── Reset & base ───────────────────────────────────────── */
  *,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
  :root{
    --bg:#0f1117;
    --surface:#1c1f2b;
    --surface2:#252836;
    --border:#2e3248;
    --accent:#4f8ef7;
    --accent-dim:#2a4a8a;
    --ok:#3dba7c;
    --warn:#f0a500;
    --err:#e05252;
    --text:#e2e6f0;
    --text-dim:#8a90a8;
    --radius:8px;
    --font:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  }
  body{background:var(--bg);color:var(--text);font-family:var(--font);
       font-size:15px;line-height:1.5;min-height:100vh;padding:0 0 40px}

  /* ── Header ─────────────────────────────────────────────── */
  header{background:var(--surface);border-bottom:1px solid var(--border);
         padding:14px 20px;display:flex;align-items:center;gap:12px;
         position:sticky;top:0;z-index:100}
  header h1{font-size:1.1rem;font-weight:600;flex:1}
  #statusBadge{font-size:.78rem;padding:3px 10px;border-radius:20px;
               font-weight:600;letter-spacing:.03em;white-space:nowrap}
  .badge-connected{background:#1a3d2b;color:var(--ok);border:1px solid var(--ok)}
  .badge-portal{background:#3d2a1a;color:var(--warn);border:1px solid var(--warn)}
  .badge-other{background:#2a1e3d;color:var(--text-dim);border:1px solid var(--border)}

  /* ── Status bar ──────────────────────────────────────────── */
  #statusBar{background:var(--surface2);border-bottom:1px solid var(--border);
             padding:8px 20px;font-size:.82rem;color:var(--text-dim);
             display:flex;flex-wrap:wrap;gap:16px;align-items:center}
  #statusBar span b{color:var(--text)}
  #wifiWarn{display:none;background:#3d2000;color:var(--warn);
            border:1px solid var(--warn);border-radius:var(--radius);
            padding:8px 14px;margin:10px 20px 0;font-size:.84rem}

  /* ── Tabs ────────────────────────────────────────────────── */
  .tabs{display:flex;gap:2px;padding:16px 20px 0;border-bottom:1px solid var(--border)}
  .tab{background:none;border:none;color:var(--text-dim);padding:9px 18px;
       font-size:.9rem;cursor:pointer;border-radius:var(--radius) var(--radius) 0 0;
       border:1px solid transparent;border-bottom:none;transition:all .15s}
  .tab:hover{color:var(--text);background:var(--surface2)}
  .tab.active{background:var(--surface);color:var(--accent);
              border-color:var(--border);border-bottom-color:var(--surface)}

  /* ── Tab panels ──────────────────────────────────────────── */
  .panel{display:none;padding:22px 20px}
  .panel.active{display:block}

  /* ── Cards ───────────────────────────────────────────────── */
  .card{background:var(--surface);border:1px solid var(--border);
        border-radius:var(--radius);padding:18px;margin-bottom:16px}
  .card h2{font-size:.85rem;font-weight:700;color:var(--text-dim);
           text-transform:uppercase;letter-spacing:.08em;margin-bottom:14px}

  /* ── Form elements ───────────────────────────────────────── */
  .field{margin-bottom:14px}
  .field:last-child{margin-bottom:0}
  label{display:block;font-size:.82rem;color:var(--text-dim);margin-bottom:4px}
  input[type=text],input[type=password],input[type=number],input[type=url]{
    width:100%;background:var(--surface2);border:1px solid var(--border);
    border-radius:var(--radius);color:var(--text);padding:8px 11px;
    font-size:.9rem;outline:none;transition:border-color .15s}
  input:focus{border-color:var(--accent)}
  input.err{border-color:var(--err)}
  .err-msg{color:var(--err);font-size:.78rem;margin-top:3px;display:none}
  .err-msg.show{display:block}

  /* Toggle (use static IP) */
  .toggle-row{display:flex;align-items:center;gap:10px;margin-bottom:14px}
  .toggle-row label{margin:0;color:var(--text);font-size:.9rem}
  input[type=checkbox]{width:16px;height:16px;accent-color:var(--accent);cursor:pointer}
  #staticFields{display:none}

  /* ── Network list ────────────────────────────────────────── */
  #netList{list-style:none;margin-top:10px}
  #netList li{display:flex;justify-content:space-between;align-items:center;
              padding:8px 10px;border-radius:6px;cursor:pointer;
              transition:background .12s;font-size:.88rem;gap:8px}
  #netList li:hover{background:var(--surface2)}
  .net-ssid{flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .net-rssi{color:var(--text-dim);font-size:.78rem;white-space:nowrap}
  .net-lock{color:var(--text-dim);font-size:.78rem}
  .net-bar{display:inline-block;width:38px;height:8px;border-radius:4px;
           background:var(--border);position:relative;overflow:hidden;vertical-align:middle}
  .net-bar-fill{position:absolute;top:0;left:0;height:100%;border-radius:4px;
                background:var(--ok)}
  #scanMsg{color:var(--text-dim);font-size:.84rem;padding:6px 0}

  /* ── Buttons ─────────────────────────────────────────────── */
  .btn{display:inline-flex;align-items:center;gap:6px;padding:8px 18px;
       border-radius:var(--radius);border:none;font-size:.88rem;font-weight:600;
       cursor:pointer;transition:opacity .15s,background .15s}
  .btn:disabled{opacity:.4;cursor:not-allowed}
  .btn-primary{background:var(--accent);color:#fff}
  .btn-primary:hover:not(:disabled){background:#3a7ae0}
  .btn-secondary{background:var(--surface2);color:var(--text);
                 border:1px solid var(--border)}
  .btn-secondary:hover:not(:disabled){background:var(--border)}
  .btn-danger{background:#5a1a1a;color:var(--err);border:1px solid var(--err)}
  .btn-danger:hover:not(:disabled){background:#7a2020}
  .btn-row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;
           margin-top:18px;padding-top:14px;border-top:1px solid var(--border)}

  /* ── Save feedback ───────────────────────────────────────── */
  #saveMsg{font-size:.84rem;padding:4px 0}
  .msg-ok{color:var(--ok)}
  .msg-err{color:var(--err)}
  .msg-info{color:var(--accent)}

  /* ── Captive-portal goodbye overlay ─────────────────────── */
  #overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.85);
           z-index:999;align-items:center;justify-content:center}
  #overlay.show{display:flex}
  #overlayBox{background:var(--surface);border:1px solid var(--border);
              border-radius:12px;padding:32px 28px;max-width:380px;text-align:center}
  #overlayBox h2{margin-bottom:10px;font-size:1.1rem}
  #overlayBox p{color:var(--text-dim);font-size:.9rem;line-height:1.6}
  .spinner{display:inline-block;width:22px;height:22px;
           border:3px solid var(--border);border-top-color:var(--accent);
           border-radius:50%;animation:spin .7s linear infinite;
           vertical-align:middle;margin-right:6px}
  @keyframes spin{to{transform:rotate(360deg)}}

  /* ── Responsive tweaks ───────────────────────────────────── */
  @media(max-width:480px){
    .tabs{padding:12px 10px 0;gap:1px}
    .tab{padding:7px 11px;font-size:.82rem}
    .panel{padding:14px 10px}
    .btn-row{flex-direction:column;align-items:stretch}
    .btn{justify-content:center}
  }
</style>
</head>
<body>

<!-- ── Header ──────────────────────────────────────────────── -->
<header>
  <h1 id="pageTitle">Device Configuration</h1>
  <span id="statusBadge" class="badge-other">Loading...</span>
</header>

<!-- ── Status bar ──────────────────────────────────────────── -->
<div id="statusBar">
  <span id="sbSsid"></span>
  <span id="sbIp"></span>
  <span id="sbRssi"></span>
</div>

<!-- ── WiFi-drop warning ───────────────────────────────────── -->
<div id="wifiWarn">&#9888; WiFi connection lost. Changes to non-network settings can still be saved.</div>

<!-- ── Tabs ────────────────────────────────────────────────── -->
<div class="tabs">
  <button class="tab active" data-tab="wifi"   onclick="showTab('wifi',this)">WiFi</button>
  <button class="tab"        data-tab="app"    onclick="showTab('app',this)">App</button>
  <button class="tab"        data-tab="mqtt"   onclick="showTab('mqtt',this)">MQTT</button>
  <button class="tab"        data-tab="system" onclick="showTab('system',this)">System</button>
</div>

<!-- ══════════════════════════════════════════════════════════
     TAB: WiFi
     ════════════════════════════════════════════════════════ -->
<div class="panel active" id="panel-wifi">

  <div class="card">
    <h2>Nearby Networks</h2>
    <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
      <button class="btn btn-secondary" id="scanBtn" onclick="startScan()">
        <span id="scanBtnTxt">Scan</span>
      </button>
      <span id="scanMsg"></span>
    </div>
    <ul id="netList"></ul>
  </div>

  <div class="card">
    <h2>Primary Network</h2>
    <div class="field">
      <label>SSID</label>
      <input type="text" id="primary_ssid" maxlength="32"
             placeholder="Network name"
             onfocus="setFocusedSsid('primary')"
             oninput="clearErr('primary_ssid')">
      <div class="err-msg" id="err-primary_ssid"></div>
    </div>
    <div class="field">
      <label>Password</label>
      <input type="password" id="primary_password" maxlength="64"
             placeholder="Leave blank to keep current"
             autocomplete="new-password">
    </div>
  </div>

  <div class="card">
    <h2>Secondary Network <span style="font-weight:400;color:var(--text-dim)">(fallback)</span></h2>
    <div class="field">
      <label>SSID</label>
      <input type="text" id="secondary_ssid" maxlength="32"
             placeholder="Optional fallback network"
             onfocus="setFocusedSsid('secondary')"
             oninput="clearErr('secondary_ssid')">
      <div class="err-msg" id="err-secondary_ssid"></div>
    </div>
    <div class="field">
      <label>Password</label>
      <input type="password" id="secondary_password" maxlength="64"
             placeholder="Leave blank to keep current"
             autocomplete="new-password">
    </div>
  </div>

  <div class="card">
    <h2>IP Settings</h2>
    <div class="toggle-row">
      <input type="checkbox" id="use_static_ip" onchange="toggleStaticIP()">
      <label for="use_static_ip">Use static IP address</label>
    </div>
    <div id="staticFields">
      <div class="field">
        <label>IP Address</label>
        <input type="text" id="static_ip" placeholder="192.168.1.100"
               oninput="clearErr('static_ip')">
        <div class="err-msg" id="err-static_ip"></div>
      </div>
      <div class="field">
        <label>Gateway</label>
        <input type="text" id="gateway" placeholder="192.168.1.1">
      </div>
      <div class="field">
        <label>Subnet Mask</label>
        <input type="text" id="subnet" placeholder="255.255.255.0">
      </div>
      <div class="field">
        <label>DNS 1</label>
        <input type="text" id="dns1" placeholder="8.8.8.8">
      </div>
      <div class="field">
        <label>DNS 2 <span style="color:var(--text-dim)">(optional)</span></label>
        <input type="text" id="dns2" placeholder="8.8.4.4">
      </div>
    </div>
  </div>

</div><!-- /panel-wifi -->

<!-- ══════════════════════════════════════════════════════════
     TAB: App
     ════════════════════════════════════════════════════════ -->
<div class="panel" id="panel-app">

  <div class="card">
    <h2>Device Identity</h2>
    <div class="field">
      <label>Application Name</label>
      <input type="text" id="app_name" maxlength="32" placeholder="MyDevice">
    </div>
    <div class="field">
      <label>Hostname <span style="color:var(--text-dim)">(used for mDNS: hostname.local)</span></label>
      <input type="text" id="hostname" maxlength="63" placeholder="esp-device"
             oninput="clearErr('hostname')">
      <div class="err-msg" id="err-hostname"></div>
    </div>
  </div>

  <div class="card">
    <h2>Portal Security</h2>
    <div class="field">
      <!-- FIX: placeholder updated to clarify that clearing the field removes auth -->
      <label>Web Portal Password <span style="color:var(--text-dim)">(clear to disable authentication)</span></label>
      <input type="password" id="web_portal_password" maxlength="32"
             placeholder="Enter new password, or clear to disable auth"
             autocomplete="new-password">
      <!-- FIX: added hint so the user understands the current-password state -->
      <div id="pwHint" style="font-size:.78rem;color:var(--text-dim);margin-top:4px"></div>
    </div>
  </div>

</div><!-- /panel-app -->

<!-- ══════════════════════════════════════════════════════════
     TAB: MQTT
     ════════════════════════════════════════════════════════ -->
<div class="panel" id="panel-mqtt">

  <div class="card">
    <h2>Broker</h2>
    <div class="field">
      <label>Broker Address <span style="color:var(--text-dim)">(host or host:port, no mqtt://)</span></label>
      <input type="text" id="mqtt_broker" maxlength="128"
             placeholder="192.168.1.50 or broker.example.com:1883"
             oninput="clearErr('mqtt_broker')">
      <div class="err-msg" id="err-mqtt_broker"></div>
    </div>
    <div class="field">
      <label>Port</label>
      <input type="number" id="mqtt_port" min="1" max="65535"
             placeholder="1883" style="max-width:120px">
      <div class="err-msg" id="err-mqtt_port"></div>
    </div>
  </div>

  <div class="card">
    <h2>Authentication</h2>
    <div class="field">
      <label>Username</label>
      <input type="text" id="mqtt_user" maxlength="64" placeholder="optional">
    </div>
    <div class="field">
      <label>Password</label>
      <input type="password" id="mqtt_password" maxlength="64"
             placeholder="Leave blank to keep current"
             autocomplete="new-password">
    </div>
    <div class="field">
      <label>Client ID</label>
      <input type="text" id="mqtt_client_id" maxlength="32"
             placeholder="Auto-generated if empty">
    </div>
  </div>

  <div class="card">
    <h2>Topics</h2>
    <div class="field">
      <label>Base Topic</label>
      <input type="text" id="mqtt_base_topic" maxlength="64"
             placeholder="home/device">
    </div>
    <div class="field">
      <label>Home Assistant Discovery Topic</label>
      <input type="text" id="mqtt_ha_topic" maxlength="64"
             placeholder="homeassistant">
    </div>
  </div>

</div><!-- /panel-mqtt -->

<!-- ══════════════════════════════════════════════════════════
     TAB: System
     ════════════════════════════════════════════════════════ -->
<div class="panel" id="panel-system">

  <div class="card">
    <h2>Time</h2>
    <div class="field">
      <label>NTP Server</label>
      <input type="text" id="ntp_server" maxlength="128"
             placeholder="pool.ntp.org"
             oninput="clearErr('ntp_server')">
      <div class="err-msg" id="err-ntp_server"></div>
    </div>
    <div class="field">
      <label>POSIX Timezone
        <span style="color:var(--text-dim)">
          e.g. EET-2EEST,M3.5.0/3,M10.5.0/4
        </span>
      </label>
      <input type="text" id="posix_timezone" maxlength="64" placeholder="UTC0">
    </div>
  </div>

  <div class="card">
    <h2>Firmware Update</h2>
    <div class="field">
      <label>OTA URL <span style="color:var(--text-dim)">(used by main program)</span></label>
      <input type="url" id="ota_url" maxlength="128"
             placeholder="http://example.com/firmware.bin">
    </div>
  </div>

</div><!-- /panel-system -->

<!-- ── Action bar ──────────────────────────────────────────── -->
<div class="btn-row" style="padding:0 20px">
  <button class="btn btn-primary" id="saveBtn" onclick="doSave()">
    <span id="saveBtnTxt">Save</span>
  </button>
  <button class="btn btn-secondary" id="exitBtn"
          onclick="doExit()" style="display:none">
    Exit Portal
  </button>
  <span id="saveMsg"></span>
</div>

<!-- ── Captive-portal goodbye overlay ──────────────────────── -->
<div id="overlay">
  <div id="overlayBox">
    <h2>&#10003; Settings Saved</h2>
    <p id="overlayMsg">The device is connecting to your network.<br>
       <strong>This page will no longer be available.</strong><br><br>
       <span class="spinner"></span>Reconnect to your network,<br>
       then visit <b id="overlayHost">the device IP</b> to continue.</p>
  </div>
</div>

<!-- ══════════════════════════════════════════════════════════
     JavaScript
     ════════════════════════════════════════════════════════ -->
<script>
// ── State ────────────────────────────────────────────────────
var focusedSsid  = 'primary';
var scanPoll     = null;
var isScanning   = false;
var isSaving     = false;
var isCaptive    = true;
var statusLoaded = false;
var hasPortalPassword = false;  // FIX: track whether a password is currently set

// ── Init ─────────────────────────────────────────────────────
window.addEventListener('load', function() {
  loadStatus();
});

// ── Tab switching ─────────────────────────────────────────────
function showTab(name, btn) {
  document.querySelectorAll('.tab').forEach(function(t){ t.classList.remove('active'); });
  document.querySelectorAll('.panel').forEach(function(p){ p.classList.remove('active'); });
  btn.classList.add('active');
  document.getElementById('panel-' + name).classList.add('active');
}

// ── Focus tracking for scan-click-to-fill ────────────────────
function setFocusedSsid(which) { focusedSsid = which; }

// ── Load status & pre-fill form ──────────────────────────────
function loadStatus() {
  fetch('/status')
    .then(function(r){ return r.json(); })
    .then(function(d) {
      statusLoaded = true;
      var c = d.config || {};

      var state = (d.state || '').toUpperCase();
      isCaptive = (state !== 'CONNECTED' &&
                   state !== 'WEB_PORTAL_ACTIVE' &&
                   state !== 'MDNS_RESTART');

      updateStatusBadge(d);
      updateStatusBar(d);

      if (!isCaptive) {
        document.getElementById('exitBtn').style.display = '';
      }

      setVal('primary_ssid',   c.primary_ssid   || '');
      setVal('secondary_ssid', c.secondary_ssid || '');
      setVal('app_name',       c.app_name       || '');
      setVal('hostname',       c.hostname       || '');
      setVal('mqtt_broker',    c.mqtt_broker    || '');
      setVal('mqtt_port',      c.mqtt_port      || 1883);
      setVal('mqtt_user',      c.mqtt_user      || '');
      setVal('mqtt_client_id', c.mqtt_client_id || '');
      setVal('mqtt_base_topic',c.mqtt_base_topic|| '');
      setVal('mqtt_ha_topic',  c.mqtt_ha_topic  || '');
      setVal('ntp_server',     c.ntp_server     || '');
      setVal('posix_timezone', c.posix_timezone || '');
      setVal('ota_url',        c.ota_url        || '');

      // FIX: track whether a portal password is currently set so we can
      // show the right hint and avoid sending '****' for a blank field.
      // The server sends "****" when a password exists, "" when none.
      hasPortalPassword = (c.web_portal_password === '****');
      var pwHint = document.getElementById('pwHint');
      if (pwHint) {
        pwHint.textContent = hasPortalPassword
          ? 'A password is currently set. Clear this field to remove authentication.'
          : 'No password set — portal is open.';
      }

      var useStatic = c.use_static_ip || false;
      document.getElementById('use_static_ip').checked = useStatic;
      toggleStaticIP();
      if (useStatic) {
        setVal('static_ip', c.static_ip || '');
        setVal('gateway',   c.gateway   || '');
        setVal('subnet',    c.subnet    || '');
        setVal('dns1',      c.dns1      || '');
        setVal('dns2',      c.dns2      || '');
      }

      if (c.app_name) {
        document.getElementById('pageTitle').textContent = c.app_name + ' Configuration';
        document.title = c.app_name + ' Configuration';
      }

      startScan();
    })
    .catch(function() {
      statusLoaded = true;
      document.getElementById('statusBadge').textContent = 'Offline';
      document.getElementById('statusBadge').className = 'badge-other';
      startScan();
    });
}

function updateStatusBadge(d) {
  var badge = document.getElementById('statusBadge');
  var state = (d.state || '').toUpperCase();
  if (state === 'CONNECTED' || state === 'WEB_PORTAL_ACTIVE' || state === 'MDNS_RESTART') {
    badge.textContent = 'Connected';
    badge.className = 'badge-connected';
  } else if (state.indexOf('PORTAL') >= 0) {
    badge.textContent = 'Captive Portal';
    badge.className = 'badge-portal';
  } else {
    badge.textContent = d.state || 'Unknown';
    badge.className = 'badge-other';
  }
}

function updateStatusBar(d) {
  var sb = document.getElementById('statusBar');
  // FIX: null-guard individual fields before calling esc() to avoid
  // "undefined" appearing in the status bar during transitional states.
  if (d.ssid) {
    document.getElementById('sbSsid').innerHTML = 'SSID: <b>' + esc(d.ssid || '') + '</b>';
    document.getElementById('sbIp').innerHTML   = 'IP: <b>'   + esc(d.ip   || '') + '</b>';
    var rssiStr = (d.rssi !== undefined ? d.rssi : '--') + ' dBm (' + rssiQuality(d.rssi) + ')';
    document.getElementById('sbRssi').innerHTML = 'Signal: <b>' + rssiStr + '</b>';
    sb.style.display = '';
  } else {
    sb.style.display = 'none';
  }
}

function rssiQuality(rssi) {
  if (rssi >= -50) return 'Excellent';
  if (rssi >= -65) return 'Good';
  if (rssi >= -75) return 'Fair';
  return 'Weak';
}

// ── WiFi scan ─────────────────────────────────────────────────
function startScan() {
  if (isScanning) return;
  isScanning = true;

  var btn     = document.getElementById('scanBtn');
  var txt     = document.getElementById('scanBtnTxt');
  var msg     = document.getElementById('scanMsg');
  var saveBtn = document.getElementById('saveBtn');

  btn.disabled     = true;
  saveBtn.disabled = true;
  txt.textContent  = 'Scanning...';
  msg.textContent  = '';
  document.getElementById('netList').innerHTML = '';

  fetch('/scan').then(pollScan);

  scanPoll = setInterval(function() {
    fetch('/scan').then(pollScan);
  }, 1200);
}

function pollScan(resp) {
  resp.json().then(function(d) {
    if (!d.scanning) {
      clearInterval(scanPoll);
      scanPoll   = null;
      isScanning = false;

      document.getElementById('scanBtn').disabled  = false;
      document.getElementById('saveBtn').disabled  = isSaving;
      document.getElementById('scanBtnTxt').textContent = 'Refresh';
      document.getElementById('scanMsg').textContent =
        d.networks.length ? '' : 'No networks found.';

      renderNetworks(d.networks || []);
    }
  }).catch(function(){
    clearInterval(scanPoll);
    scanPoll   = null;
    isScanning = false;
    document.getElementById('scanBtn').disabled  = false;
    document.getElementById('saveBtn').disabled  = isSaving;
    document.getElementById('scanBtnTxt').textContent = 'Refresh';
    document.getElementById('scanMsg').textContent = 'Scan failed.';
  });
}

function renderNetworks(nets) {
  var ul = document.getElementById('netList');
  ul.innerHTML = '';
  nets.forEach(function(n) {
    var pct = Math.min(100, Math.max(0, 2 * (n.rssi + 100)));
    var li  = document.createElement('li');
    li.title = n.ssid + ' (' + n.rssi + ' dBm)';
    li.innerHTML =
      '<span class="net-ssid">' + esc(n.ssid) + '</span>' +
      '<span class="net-rssi">' + n.rssi + ' dBm</span>' +
      '<span class="net-lock">' + (n.encrypted ? '&#128274;' : '&#128275;') + '</span>' +
      '<span class="net-bar"><span class="net-bar-fill" style="width:' + pct + '%"></span></span>';
    li.addEventListener('click', function() { selectNetwork(n.ssid); });
    ul.appendChild(li);
  });
}

function selectNetwork(ssid) {
  document.getElementById(focusedSsid + '_ssid').value = ssid;
  document.getElementById(focusedSsid + '_password').focus();
}

// ── Static IP toggle ─────────────────────────────────────────
function toggleStaticIP() {
  var checked = document.getElementById('use_static_ip').checked;
  document.getElementById('staticFields').style.display = checked ? 'block' : 'none';
}

// ── Save ─────────────────────────────────────────────────────
function doSave() {
  if (isSaving || isScanning) return;
  isSaving = true;

  var btn = document.getElementById('saveBtn');
  var txt = document.getElementById('saveBtnTxt');
  var msg = document.getElementById('saveMsg');

  btn.disabled    = true;
  txt.innerHTML   = '<span class="spinner"></span>Saving...';
  msg.textContent = '';
  msg.className   = '';

  document.querySelectorAll('.err-msg').forEach(function(e){
    e.textContent = ''; e.classList.remove('show');
  });
  document.querySelectorAll('input.err').forEach(function(i){
    i.classList.remove('err');
  });

  // FIX: password fields — send the field value directly.
  // Empty string means "clear this password" (intentional removal).
  // "****" is only sent when the field hasn't been touched AND a password
  // was already set, telling the server "no change".
  // This fixes the original bug where blank fields always sent "****",
  // making it impossible to clear a saved password.
  function pwValue(id) {
    var val = getVal(id);
    if (val === '') {
      // Field is blank. If a password was previously set for the web portal
      // password field, send "" to clear it. For WiFi/MQTT passwords, blank
      // means "no change" (open network has blank password set explicitly).
      return '';
    }
    return val;
  }

  // For fields that show "****" as a status indicator (not user-typed), we
  // need to distinguish "user didn't touch this" from "user cleared it".
  // We track this with data-dirty on the input — set on any input event.
  function maskedPwValue(id) {
    var el = document.getElementById(id);
    if (!el) return '****';
    // If the user typed anything, send what they typed (including empty string).
    // If untouched, send '****' so the server keeps the existing value.
    return el.dataset.dirty ? el.value : '****';
  }

  var payload = {
    app_name:             getVal('app_name'),
    hostname:             getVal('hostname'),
    // FIX: send raw value so empty string correctly clears the password
    web_portal_password:  getVal('web_portal_password'),

    primary_ssid:         getVal('primary_ssid'),
    primary_password:     maskedPwValue('primary_password'),
    secondary_ssid:       getVal('secondary_ssid'),
    secondary_password:   maskedPwValue('secondary_password'),
    use_static_ip:        document.getElementById('use_static_ip').checked,
    static_ip:            getVal('static_ip'),
    gateway:              getVal('gateway'),
    subnet:               getVal('subnet'),
    dns1:                 getVal('dns1'),
    dns2:                 getVal('dns2'),

    mqtt_broker:          getVal('mqtt_broker'),
    mqtt_port:            parseInt(getVal('mqtt_port'), 10) || 1883,
    mqtt_user:            getVal('mqtt_user'),
    mqtt_password:        maskedPwValue('mqtt_password'),
    mqtt_client_id:       getVal('mqtt_client_id'),
    mqtt_base_topic:      getVal('mqtt_base_topic'),
    mqtt_ha_topic:        getVal('mqtt_ha_topic'),

    ntp_server:           getVal('ntp_server'),
    posix_timezone:       getVal('posix_timezone'),
    ota_url:              getVal('ota_url')
  };

  fetch('/save', {
    method:  'POST',
    headers: {'Content-Type': 'application/json'},
    body:    JSON.stringify(payload)
  })
  .then(function(r){ return r.json(); })
  .then(function(d) {
    isSaving = false;
    txt.textContent = 'Save';
    btn.disabled = isScanning;

    if (d.success) {
      if (isCaptive || d.wifi_restart) {
        // FIX: stop scan polling before showing overlay so it doesn't
        // fire requests against a server that is tearing down.
        if (scanPoll) { clearInterval(scanPoll); scanPoll = null; isScanning = false; }
        document.getElementById('overlay').classList.add('show');
        if (d.wifi_restart && !isCaptive) {
          document.getElementById('overlayMsg').innerHTML =
            'WiFi settings changed. The device is reconnecting.<br>' +
            '<span class="spinner"></span>Please wait...';
        }
      } else {
        msg.textContent = '\u2713 Saved successfully';
        msg.className   = 'msg-ok';
        // Update password hint after a successful save
        hasPortalPassword = (payload.web_portal_password !== '');
        var pwHint = document.getElementById('pwHint');
        if (pwHint) {
          pwHint.textContent = hasPortalPassword
            ? 'A password is currently set. Clear this field to remove authentication.'
            : 'No password set — portal is open.';
        }
        // Reset dirty flags so next save re-uses masked values for unchanged fields
        ['primary_password','secondary_password','mqtt_password'].forEach(function(id){
          var el = document.getElementById(id);
          if (el) delete el.dataset.dirty;
        });
      }
    } else {
      var errs = d.errors || {};
      if (errs.general) {
        msg.textContent = '\u2717 ' + errs.general;
      } else {
        msg.textContent = '\u2717 Please fix the errors in red above';
      }
      msg.className = 'msg-err';

      var firstFieldErr = null;
      Object.keys(errs).forEach(function(field) {
        if (field === 'general') return;
        var el   = document.getElementById(field);
        var emsg = document.getElementById('err-' + field);
        if (el)   el.classList.add('err');
        if (emsg) { emsg.textContent = errs[field]; emsg.classList.add('show'); }
        if (!firstFieldErr) firstFieldErr = field;
      });

      if (firstFieldErr) jumpToField(firstFieldErr);
    }
  })
  .catch(function() {
    isSaving = false;
    txt.textContent  = 'Save';
    btn.disabled     = false;
    msg.textContent  = '\u2717 Network error — please try again';
    msg.className    = 'msg-err';
  });
}

// FIX: mark password fields dirty when the user types, so maskedPwValue()
// knows to send the actual content rather than "****".
(function() {
  ['primary_password','secondary_password','mqtt_password'].forEach(function(id) {
    var el = document.getElementById(id);
    if (el) {
      el.addEventListener('input', function() {
        el.dataset.dirty = '1';
      });
    }
  });
})();

var fieldTab = {
  primary_ssid: 'wifi', primary_password: 'wifi',
  secondary_ssid: 'wifi', secondary_password: 'wifi',
  static_ip: 'wifi', gateway: 'wifi', subnet: 'wifi',
  dns1: 'wifi', dns2: 'wifi', use_static_ip: 'wifi',
  app_name: 'app', hostname: 'app', web_portal_password: 'app',
  mqtt_broker: 'mqtt', mqtt_port: 'mqtt', mqtt_user: 'mqtt',
  mqtt_password: 'mqtt', mqtt_client_id: 'mqtt',
  mqtt_base_topic: 'mqtt', mqtt_ha_topic: 'mqtt',
  ntp_server: 'system', posix_timezone: 'system', ota_url: 'system'
};

function jumpToField(fieldId) {
  var tab = fieldTab[fieldId];
  if (!tab) return;
  var tabBtn = document.querySelector('.tab[data-tab="' + tab + '"]');
  if (tabBtn) showTab(tab, tabBtn);
  var el = document.getElementById(fieldId);
  if (el) el.focus();
}

// ── Exit portal ───────────────────────────────────────────────
function doExit() {
  var exitBtn = document.getElementById('exitBtn');
  var saveMsg = document.getElementById('saveMsg');
  exitBtn.disabled = true;
  // FIX: show feedback regardless of fetch outcome since the server may
  // start tearing down before the response arrives.
  saveMsg.textContent = 'Portal is closing...';
  saveMsg.className   = 'msg-info';
  fetch('/exit').catch(function(){
    // Server may close before responding — this is expected.
  });
}

// ── Utilities ─────────────────────────────────────────────────
function getVal(id) {
  var el = document.getElementById(id);
  return el ? el.value.trim() : '';
}
function setVal(id, val) {
  var el = document.getElementById(id);
  if (el) el.value = val;
}
function clearErr(id) {
  var el  = document.getElementById(id);
  var msg = document.getElementById('err-' + id);
  if (el)  el.classList.remove('err');
  if (msg) { msg.textContent = ''; msg.classList.remove('show'); }
}
function esc(s) {
  return String(s)
    .replace(/&/g,'&amp;')
    .replace(/</g,'&lt;')
    .replace(/>/g,'&gt;')
    .replace(/"/g,'&quot;');
}
</script>
</body>
</html>
)rawliteral";
