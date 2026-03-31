// ── Config ──────────────────────────────────────────
var WS_URL   = 'ws://192.168.4.1/ws';
var PRESETS  = ['#ff6600','#ff0000','#ff3399','#9933ff','#0066ff','#00ccff','#00ff88','#ffffff'];

// ── State ────────────────────────────────────────────
var ws             = null;
var connected      = false;
var currentRole    = '';
var currentUid     = -1;
var currentUser    = '';
var adminPass      = '';
var pendingLogin   = null;
var currentEffect  = { u: 'solid', a: 'solid' };
var liveTimer      = null;
var allLogs        = [];
var logFilter      = 'all';
var storedProfiles = [];
var previewTimers  = {};

// ── Login ─────────────────────────────────────────────
function doLogin() {
  var user = document.getElementById('login-user').value.trim();
  var pass = document.getElementById('login-pass').value;
  var err  = document.getElementById('login-err');

  if (!user || !pass) {
    err.style.display = 'block';
    err.textContent = 'Kullanici adi ve sifre gerekli';
    return;
  }

  err.style.display = 'none';
  var btn = document.getElementById('login-btn');
  btn.textContent = 'Baglanıyor...';
  btn.disabled = true;

  pendingLogin = { username: user, password: pass };
  connectWs();
}

function logout() {
  // ESP32'ye logout bildir (token silinsin)
  if (ws && connected) {
    ws.send(JSON.stringify({ type: 'logout', uid: currentUid }));
  }
  clearToken();
  if (ws) { try { ws.close(); } catch(e) {} ws = null; }
  Object.keys(previewTimers).forEach(function(k) {
    clearInterval(previewTimers[k]);
    delete previewTimers[k];
  });
  currentRole    = '';
  currentUid     = -1;
  adminPass      = '';
  storedProfiles = [];
  document.getElementById('login-user').value = '';
  document.getElementById('login-pass').value = '';
  var btn = document.getElementById('login-btn');
  btn.textContent = 'Giris Yap';
  btn.disabled = false;
  showScreen('screen-login');
  addLog('INFO', 'APP', 'Cikis yapildi');
}

function showScreen(id) {
  document.querySelectorAll('.screen').forEach(function(s) { s.classList.remove('active'); });
  document.getElementById(id).classList.add('active');
}

// ── WebSocket ─────────────────────────────────────────
function connectWs() {
  if (ws) {
    ws.onopen    = null;
    ws.onmessage = null;
    ws.onclose   = null;
    ws.onerror   = null;
    try { ws.close(); } catch(e) {}
    ws = null;
  }
  ws = new WebSocket(WS_URL);

  ws.onopen = function() {
    if (pendingLogin) {
      if (pendingLogin.token) {
        // Token ile otomatik login
        ws.send(JSON.stringify({ type: 'token_login', token: pendingLogin.token }));
      } else {
        // Normal login
        ws.send(JSON.stringify({ type: 'login', username: pendingLogin.username, password: pendingLogin.password }));
      }
    }
  };

  ws.onmessage = function(e) {
    try { var data = JSON.parse(e.data); handleMsg(data); }
    catch(ex) { addLog('WARN', 'WS', 'JSON parse hatasi'); }
  };

  ws.onclose = function() {
    connected = false;
    setDot(false);
    if (pendingLogin) {
      if (pendingLogin.token) {
        // Token geçersiz — login ekranı göster
        clearToken();
        pendingLogin = null;
        showScreen('screen-login');
        var btn = document.getElementById('login-btn');
        btn.textContent = 'Giris Yap';
        btn.disabled = false;
        return;
      }
      var btn = document.getElementById('login-btn');
      btn.textContent = 'Giris Yap';
      btn.disabled = false;
      var err = document.getElementById('login-err');
      err.style.display = 'block';
      err.textContent = "ESP32'ye ulasilamadi. WiFi bagli mi?";
      pendingLogin = null;
      return;
    }
    if (currentRole) {
      addLog('WARN', 'WS', 'Baglanti kesildi, yeniden deneniyor...');
      setTimeout(connectWs, 2000);
    }
  };

  ws.onerror = function() { if (ws) ws.close(); };
}

function handleMsg(data) {
  // Login cevabi
  if (pendingLogin && data.status !== undefined) {
    var btn = document.getElementById('login-btn');
    btn.textContent = 'Giris Yap';
    btn.disabled = false;

    if (data.status === 'error') {
      var err = document.getElementById('login-err');
      err.style.display = 'block';
      err.textContent = 'Hatali kullanici adi veya sifre';
      pendingLogin = null;
      ws.close();
      return;
    }

    currentRole = data.role;
    currentUid  = data.uid;
    currentUser = data.username || 'admin';
    connected   = true;
    setDot(true);
    pendingLogin = null;
    addLog('INFO', 'AUTH', currentUser + ' giris yapti (' + currentRole + ')');

    // Token varsa localStorage'a kaydet
    if (data.token) {
      saveToken(data.token);
      addLog('INFO', 'AUTH', 'Token kaydedildi');
    }

    if (data.fw) {
      document.querySelectorAll('#brand-ver,#sys-fw').forEach(function(el) {
        el.textContent = el.id === 'brand-ver' ? 'v' + data.fw + ' · ESP32' : data.fw;
      });
    }

    if (currentRole === 'admin') {
      adminPass = document.getElementById('login-pass').value;
      showScreen('screen-admin');
      buildPresets('a-presets', 'a-color');
      buildStrip('a-strip', 8);
      buildStops('a-stops', 'a-grad', ['#ff6600', '#0066ff']);
    } else {
      showScreen('screen-user');
      document.getElementById('u-badge').textContent = currentUser;
      buildPresets('u-presets', 'u-color');
      buildStrip('u-strip', 8);
      buildStops('u-stops', 'u-grad', ['#ff6600', '#0066ff']);
      if (data.profiles) {
        storedProfiles = data.profiles;
        renderProfiles(data.profiles);
      }
    }
    return;
  }

  if (data.type === 'profiles_update' && data.profiles) {
    storedProfiles = data.profiles;
    renderProfiles(data.profiles);
    return;
  }

  if (data.type === 'users_list') {
    renderUsersList(data.users, data.max);
    return;
  }

  if (data.msg === 'config saved') {
    showHint('cfg-hint', 'Config kaydedildi');
    addLog('INFO', 'CFG', 'LED config guncellendi');
    return;
  }

  // OTA mesajlari
  if (data.msg === 'ota_ready') {
    addLog('INFO', 'OTA', 'Hazir, chunk gonderiliyor...');
    setOtaProgress(1, 'Yukleniyor...');
    sendNextOtaChunk();
    return;
  }
  if (data.msg === 'chunk_ok') {
    setOtaProgress(data.pct, 'Yukleniyor... ' + data.pct + '%');
    if (window._otaBuffer && window._otaOffset < window._otaBuffer.byteLength) {
      sendNextOtaChunk();
    } else {
      ws.send(JSON.stringify({ type: 'ota_end' }));
      setOtaProgress(100, 'Dogrulaniyor...');
      addLog('INFO', 'OTA', 'Tum chunklar gonderildi, dogrulanıyor...');
    }
    return;
  }
  if (data.msg === 'ota_done') {
    otaRunning = false;
    window._otaBuffer = null;
    setOtaProgress(100, 'Tamamlandi! Yeniden baslatiliyor...');
    addLog('INFO', 'OTA', 'Guncelleme basarili, ESP32 yeniden basliyor');
    return;
  }
  if (data.msg === 'aborted') {
    otaRunning = false;
    addLog('WARN', 'OTA', 'Iptal edildi');
    return;
  }

  if (data.status === 'error') {
    var msg = data.msg || 'Hata';
    if (msg === 'wrong password')    msg = 'Hatali sifre';
    if (msg === 'username taken')    msg = 'Bu kullanici adi alinmis';
    if (msg === 'max users reached') msg = 'Max kullanici sayisina ulasildi (3)';
    // OTA hatası
    if (otaRunning) {
      otaRunning = false;
      window._otaBuffer = null;
      setOtaProgress(0, 'Hata: ' + msg);
      addLog('ERROR', 'OTA', msg);
      return;
    }
    showHint('cfg-hint', msg);
    showHint('a-hint', msg);
    showHint('u-hint', msg);
    var errEl = document.getElementById('new-user-err');
    if (errEl) { errEl.textContent = msg; errEl.style.display = 'block'; }
    addLog('WARN', 'WS', msg);
  }
}

// ── Connection Status ─────────────────────────────────
function setDot(on) {
  ['u-dot','a-dot'].forEach(function(id) {
    var d = document.getElementById(id);
    if (d) d.className = 'led-dot ' + (on ? 'on' : 'connecting');
  });
  var txt = on ? '192.168.4.1 — bagli' : 'Yeniden baglanıyor...';
  ['u-conn-text','a-conn-text'].forEach(function(id) {
    var el = document.getElementById(id);
    if (el) el.textContent = txt;
  });
}

// ── LED Strip Preview ─────────────────────────────────
function buildStrip(id, count) {
  var strip = document.getElementById(id);
  if (!strip) return;
  strip.innerHTML = '';
  for (var i = 0; i < Math.min(count, 16); i++) {
    var c = document.createElement('div');
    c.className = 'led-cell';
    strip.appendChild(c);
  }
}

function animateStrip(p) {
  var key = p + '-anim';
  if (previewTimers[key]) { clearInterval(previewTimers[key]); delete previewTimers[key]; }

  var e      = currentEffect[p];
  var stripQ = '#' + p + '-strip .led-cell';

  if (e === 'solid') {
    var color = document.getElementById(p + '-color').value;
    var b     = parseInt(document.getElementById(p + '-brightness').value) / 100;
    var bc    = applyB(color, b * b);
    var sh    = '0 0 8px ' + applyB(color, b * 0.5);
    document.querySelectorAll(stripQ).forEach(function(c) {
      c.style.background = bc; c.style.boxShadow = sh;
    });

  } else if (e === 'fade') {
    var color = document.getElementById(p + '-color').value;
    var t = 0;
    previewTimers[key] = setInterval(function() {
      if (currentEffect[p] !== 'fade') { clearInterval(previewTimers[key]); return; }
      t += 0.05;
      var b  = 0.15 + 0.85 * (0.5 + 0.5 * Math.sin(t));
      var bc = applyB(color, b);
      document.querySelectorAll(stripQ).forEach(function(c) {
        c.style.background = bc;
        c.style.boxShadow  = '0 0 8px ' + applyB(color, b * 0.5);
      });
    }, 40);

  } else if (e === 'rainbow') {
    var hue = 0;
    previewTimers[key] = setInterval(function() {
      if (currentEffect[p] !== 'rainbow') { clearInterval(previewTimers[key]); return; }
      document.querySelectorAll(stripQ).forEach(function(c, i) {
        var h = (hue + i * 30) % 360;
        c.style.background = 'hsl(' + h + ',100%,50%)';
        c.style.boxShadow  = '0 0 8px hsl(' + h + ',100%,40%)';
      });
      hue = (hue + 3) % 360;
    }, 40);
  }
}

function applyB(hex, b) {
  var r  = parseInt(hex.slice(1,3),16);
  var g  = parseInt(hex.slice(3,5),16);
  var bl = parseInt(hex.slice(5,7),16);
  return 'rgb('+Math.round(r*b)+','+Math.round(g*b)+','+Math.round(bl*b)+')';
}

// int RGB → hex string
function rgbHex(r, g, b) {
  return '#' +
    ('0' + ((r||0) & 0xFF).toString(16)).slice(-2) +
    ('0' + ((g||0) & 0xFF).toString(16)).slice(-2) +
    ('0' + ((b||0) & 0xFF).toString(16)).slice(-2);
}

// ── Presets ───────────────────────────────────────────
function buildPresets(cid, colorId) {
  var c = document.getElementById(cid);
  if (!c) return;
  c.innerHTML = '';
  PRESETS.forEach(function(col) {
    var d = document.createElement('div');
    d.className = 'preset-dot';
    d.style.background = col;
    d.addEventListener('touchend', function(ev) { ev.preventDefault(); pickPreset(col, cid, colorId); });
    d.onclick = function() { pickPreset(col, cid, colorId); };
    c.appendChild(d);
  });
}

function pickPreset(col, cid, colorId) {
  document.getElementById(colorId).value = col;
  document.querySelectorAll('#' + cid + ' .preset-dot').forEach(function(p) { p.classList.remove('active'); });
  liveUpdate(cid.charAt(0));
}

// ── Effects ───────────────────────────────────────────
function uSetEffect(e) { _setEffect('u', e); }
function aSetEffect(e) { _setEffect('a', e); }

// Animasyon efektleri — renk paneli gösterilir ama speed paneli de
var ANIM_EFFECTS = ['comet','wave','twinkle','fire','bounce','heart','lightning','rain','dna','star'];
// Rainbow dışındaki tüm efektler için renk paneli açık
// speed her zaman gösterilir animasyonlarda

function _setEffect(p, e) {
  currentEffect[p] = e;
  var scope = p === 'u' ? '#screen-user' : '#atab-led';
  document.querySelectorAll(scope + ' .effect-btn').forEach(function(b) { b.classList.remove('active'); });
  var btn = document.getElementById(p + '-btn-' + e);
  if (btn) btn.classList.add('active');
  var lbl = document.getElementById(p + '-effect-label');
  if (lbl) lbl.textContent = e.toUpperCase();

  var cp = document.getElementById(p + '-color-panel');
  var rp = document.getElementById(p + '-rainbow-panel');
  var sf = document.getElementById(p + '-speed-row');

  // Rainbow → özel panel
  // Diğer animasyonlar → renk paneli + speed göster
  if (e === 'rainbow') {
    if (cp) cp.style.display = 'none';
    if (rp) rp.style.display = 'block';
  } else {
    if (cp) cp.style.display = 'block';
    if (rp) rp.style.display = 'none';
  }

  // Speed: fade + tüm animasyonlarda göster
  var showSpeed = (e === 'fade' || ANIM_EFFECTS.indexOf(e) >= 0);
  if (sf) sf.style.display = showSpeed ? 'block' : 'none';

  // İkinci renk: sadece DNA'da göster
  var c2row = document.getElementById(p + '-color2-row');
  if (c2row) c2row.style.display = e === 'dna' ? 'block' : 'none';

  // DNA için presets2 oluştur (ilk kez)
  var pr2 = document.getElementById(p + '-presets2');
  if (pr2 && pr2.children.length === 0) buildPresets(p + '-presets2', p + '-color2');

  animateStrip(p);
}

function liveUpdate(p) {
  animateStrip(p);
  if (liveTimer) clearTimeout(liveTimer);
  liveTimer = setTimeout(function() {
    if (connected) sendLedCmd(p);
    liveTimer = null;
  }, 80);
}

// ── Rainbow Stops ─────────────────────────────────────
function buildStops(rowId, gradId, colors) {
  var row = document.getElementById(rowId);
  if (!row) return;
  row.innerHTML = '';
  colors.forEach(function(c) { addStop(rowId, gradId, c); });
  if (colors.length < 3) appendAddBtn(rowId, gradId);
  updateGrad(rowId, gradId);
}

function addStop(rowId, gradId, color) {
  var row = document.getElementById(rowId);
  var add = row.querySelector('.stop-add');
  if (add) row.removeChild(add);
  var idx  = row.querySelectorAll('.stop-slot').length;
  var slot = document.createElement('div');
  slot.className = 'stop-slot';
  slot.innerHTML = '<span>R'+(idx+1)+'</span>';
  var inp = document.createElement('input');
  inp.type = 'color'; inp.value = color || '#ffffff';
  inp.addEventListener('input', function() { updateGrad(rowId, gradId); });
  slot.appendChild(inp);
  var rm = document.createElement('button');
  rm.className = 'stop-remove'; rm.textContent = 'x';
  rm.onclick = function() { removeStop(slot, rowId, gradId); };
  slot.appendChild(rm);
  row.appendChild(slot);
  if (row.querySelectorAll('.stop-slot').length < 3) appendAddBtn(rowId, gradId);
  updateGrad(rowId, gradId);
}

function appendAddBtn(rowId, gradId) {
  var row = document.getElementById(rowId);
  var btn = document.createElement('button');
  btn.className = 'stop-add'; btn.textContent = '+';
  btn.onclick = function() { addStop(rowId, gradId, '#ffffff'); };
  row.appendChild(btn);
}

function removeStop(slot, rowId, gradId) {
  var row = document.getElementById(rowId);
  if (row.querySelectorAll('.stop-slot').length <= 2) return;
  row.removeChild(slot);
  if (!row.querySelector('.stop-add')) appendAddBtn(rowId, gradId);
  row.querySelectorAll('.stop-slot span').forEach(function(s,i) { s.textContent = 'R'+(i+1); });
  updateGrad(rowId, gradId);
}

function updateGrad(rowId, gradId) {
  var row  = document.getElementById(rowId);
  var grad = document.getElementById(gradId);
  if (!row || !grad) return;
  var cols = Array.from(row.querySelectorAll('input[type=color]')).map(function(i) { return i.value; });
  grad.style.background = cols.length === 1 ? cols[0] : 'linear-gradient(90deg,' + cols.join(',') + ')';
}

function getStops(rowId) {
  var row = document.getElementById(rowId);
  if (!row) return ['#ff6600'];
  return Array.from(row.querySelectorAll('input[type=color]')).map(function(i) { return i.value; });
}

function hexRgb(h) {
  return { r: parseInt(h.slice(1,3),16), g: parseInt(h.slice(3,5),16), b: parseInt(h.slice(5,7),16) };
}

// ── Send LED Command ──────────────────────────────────
function sendLedCmd(p) {
  if (!connected || !ws) return;
  var e      = currentEffect[p];
  var effMap = {
    solid:     0,
    fade:      1,
    rainbow:   2,
    comet:     7,
    wave:      8,
    twinkle:   9,
    fire:      10,
    bounce:    11,
    heart:     12,
    lightning: 13,
    rain:      14,
    dna:       15,
    star:      16
  };
  var cmd = { type: 'led_cmd', effect: effMap[e] !== undefined ? effMap[e] : 0, color_count: 1 };

  if (e === 'rainbow') {
    var stops = getStops(p + '-stops');
    var c1 = hexRgb(stops[0] || '#ff6600');
    var c2 = hexRgb(stops[1] || '#0066ff');
    var c3 = hexRgb(stops[2] || '#00ff88');
    cmd.r=c1.r; cmd.g=c1.g; cmd.b=c1.b;
    cmd.r2=c2.r; cmd.g2=c2.g; cmd.b2=c2.b;
    cmd.r3=c3.r; cmd.g3=c3.g; cmd.b3=c3.b;
    cmd.color_count = stops.length;
    cmd.brightness  = parseInt(document.getElementById(p + '-rb-brightness').value);
    cmd.speed       = parseInt(document.getElementById(p + '-rb-speed').value);
  } else {
    var c = hexRgb(document.getElementById(p + '-color').value);
    cmd.r=c.r; cmd.g=c.g; cmd.b=c.b;
    cmd.brightness = parseInt(document.getElementById(p + '-brightness').value);
    var isAnimated = (e === 'fade' || ANIM_EFFECTS.indexOf(e) >= 0);
    cmd.speed = isAnimated ? parseInt(document.getElementById(p + '-speed').value) : 3;

    // DNA için ikinci renk
    if (e === 'dna') {
      var c2el = document.getElementById(p + '-color2');
      if (c2el) {
        var c2 = hexRgb(c2el.value);
        cmd.r2 = c2.r; cmd.g2 = c2.g; cmd.b2 = c2.b;
      }
    }
  }

  ws.send(JSON.stringify(cmd));
}

function applyCmd(p) {
  if (!connected) { showHint(p + '-hint', 'Bagli degil'); return; }
  sendLedCmd(p);
  showHint(p + '-hint', 'Gonderildi');
}

function sendOff(p) {
  if (!connected || !ws) return;
  ws.send(JSON.stringify({ type: 'led_cmd', effect: 3 }));
  showHint(p + '-hint', 'Kapatildi');
  document.querySelectorAll('#' + p + '-strip .led-cell').forEach(function(c) {
    c.style.background = '#1a1a1a'; c.style.boxShadow = 'none';
  });
}

// ── Test Modes (Admin) ────────────────────────────────
function sendTest(mode) {
  if (!connected || !ws) { showHint('test-hint', 'Bagli degil'); return; }
  var c = hexRgb(document.getElementById('test-color').value);
  var s = parseInt(document.getElementById('test-speed').value);
  ws.send(JSON.stringify({ type: 'test', mode: mode, r: c.r, g: c.g, b: c.b, speed: s }));
  addLog('INFO', 'TEST', mode + ' baslatildi');
  showHint('test-hint', mode);
}

// ── Profiles ──────────────────────────────────────────
function renderProfiles(profiles) {
  var list = document.getElementById('u-profiles-list');
  if (!list) return;
  list.innerHTML = '';
  var hasAny = false;
  profiles.forEach(function(p) {
    if (!p.used) return;
    hasAny = true;
    var effNames = ['Solid','Fade','Rainbow','Kapali','Chase','Flash','Random','Comet','Wave','Twinkle','Fire','Bounce','Kalp','Yildirim','Yagmur','DNA','Yildiz'];
    var ename    = effNames[p.effect] || '?';
    var card     = document.createElement('div');
    card.className = 'profile-card';
    card.innerHTML =
      '<div class="prof-info">' +
        '<div class="prof-name">' + p.name + '</div>' +
        '<div class="prof-meta mono">' + ename + ' · slot ' + p.slot + '</div>' +
      '</div>' +
      '<div class="prof-actions">' +
        '<button class="btn-small" onclick="loadProfile(' + p.slot + ')">Yukle</button>' +
        '<button class="btn-small" style="color:var(--red);border-color:rgba(239,68,68,.3)" onclick="deleteProfile(' + p.slot + ')">Sil</button>' +
      '</div>';
    list.appendChild(card);
  });
  if (!hasAny) {
    var empty = document.createElement('div');
    empty.className = 'prof-empty';
    empty.textContent = 'Henuz kayitli profil yok';
    list.appendChild(empty);
  }
}

function loadProfile(slot) {
  // Saklanan profiller arasından bul
  var prof = null;
  for (var i = 0; i < storedProfiles.length; i++) {
    if (storedProfiles[i].slot === slot && storedProfiles[i].used) {
      prof = storedProfiles[i];
      break;
    }
  }
  if (!prof) { showHint('u-hint', 'Profil bulunamadi'); return; }

  // Efekti belirle ve UI panellerini aç
  var effKeys = ['solid','fade','rainbow','solid','solid','solid','solid','comet','wave','twinkle','fire','bounce','heart','lightning','rain','dna','star'];
  var effKey  = effKeys[prof.effect] || 'solid';
  _setEffect('u', effKey);

  if (effKey === 'rainbow') {
    // Renk stop'larını yeniden oluştur
    var stops = [];
    if (prof.color_count >= 1) stops.push(rgbHex(prof.r,  prof.g,  prof.b));
    if (prof.color_count >= 2) stops.push(rgbHex(prof.r2, prof.g2, prof.b2));
    if (prof.color_count >= 3) stops.push(rgbHex(prof.r3, prof.g3, prof.b3));
    if (stops.length < 2) stops.push('#0066ff');
    buildStops('u-stops', 'u-grad', stops);

    var bEl = document.getElementById('u-rb-brightness');
    var sEl = document.getElementById('u-rb-speed');
    if (bEl) { bEl.value = prof.brightness; document.getElementById('u-rb-bval').textContent = prof.brightness + '%'; }
    if (sEl) { sEl.value = prof.speed;      document.getElementById('u-rb-sval').textContent = prof.speed; }

  } else {
    // Solid veya Fade — rengi uygula
    var hex = rgbHex(prof.r, prof.g, prof.b);
    var colorEl = document.getElementById('u-color');
    if (colorEl) colorEl.value = hex;

    var bEl = document.getElementById('u-brightness');
    if (bEl) {
      bEl.value = prof.brightness;
      document.getElementById('u-bval').textContent = prof.brightness + '%';
    }

    if (effKey === 'fade') {
      var sEl = document.getElementById('u-speed');
      if (sEl) {
        sEl.value = prof.speed;
        document.getElementById('u-sval').textContent = prof.speed;
      }
    }
  }

  // Önizleme + LED komutu
  animateStrip('u');
  if (connected) sendLedCmd('u');

  showHint('u-hint', prof.name + ' yuklendi');
  addLog('INFO', 'PROF', 'Profil yuklendi: ' + prof.name + ' (slot ' + slot + ')');
}

function showSaveForm() {
  document.getElementById('u-save-form').style.display = 'block';
  document.getElementById('u-new-prof-btn').style.display = 'none';
  document.getElementById('u-prof-name').focus();
}

function cancelSaveForm() {
  document.getElementById('u-save-form').style.display = 'none';
  document.getElementById('u-new-prof-btn').style.display = 'block';
  document.getElementById('u-prof-name').value = '';
}

function confirmSaveProfile() {
  var name = document.getElementById('u-prof-name').value.trim();
  if (!name) { document.getElementById('u-prof-name').focus(); return; }
  if (!connected) { showHint('u-hint', 'Bagli degil'); return; }

  var e      = currentEffect['u'];
  var effMap = { solid: 0, fade: 1, rainbow: 2, comet: 7, wave: 8, twinkle: 9, fire: 10, bounce: 11, heart: 12, lightning: 13, rain: 14, dna: 15, star: 16 };
  var cmd    = { color_count: 1, speed: 3, brightness: 80, r: 255, g: 102, b: 0 };

  if (e === 'rainbow') {
    var stops = getStops('u-stops');
    var c1 = hexRgb(stops[0] || '#ff6600');
    var c2 = hexRgb(stops[1] || '#0066ff');
    var c3 = hexRgb(stops[2] || '#00ff88');
    cmd.r=c1.r; cmd.g=c1.g; cmd.b=c1.b;
    cmd.r2=c2.r; cmd.g2=c2.g; cmd.b2=c2.b;
    cmd.r3=c3.r; cmd.g3=c3.g; cmd.b3=c3.b;
    cmd.color_count = stops.length;
    cmd.brightness  = parseInt(document.getElementById('u-rb-brightness').value);
    cmd.speed       = parseInt(document.getElementById('u-rb-speed').value);
  } else {
    var c = hexRgb(document.getElementById('u-color').value);
    cmd.r=c.r; cmd.g=c.g; cmd.b=c.b;
    cmd.brightness = parseInt(document.getElementById('u-brightness').value);
    if (e === 'fade') cmd.speed = parseInt(document.getElementById('u-speed').value);
  }

  // Bos slot bul (0,1,2)
  var usedSlots = storedProfiles.filter(function(p) { return p.used; }).map(function(p) { return p.slot; });
  var slot = -1;
  for (var s = 0; s < 3; s++) {
    if (usedSlots.indexOf(s) === -1) { slot = s; break; }
  }
  if (slot === -1) { showHint('u-hint', 'Profil limiti doldu (3), once sil'); return; }

  ws.send(JSON.stringify(Object.assign({
    type: 'save_profile', uid: currentUid, slot: slot, name: name, effect: effMap[e]
  }, cmd)));

  cancelSaveForm();
  addLog('INFO', 'PROF', 'Profil kaydedildi: ' + name + ' slot:' + slot);
  showHint('u-hint', name + ' kaydedildi');
}

function deleteProfile(slot) {
  if (!connected) return;
  ws.send(JSON.stringify({ type: 'delete_profile', uid: currentUid, slot: slot }));
  addLog('INFO', 'PROF', 'Profil silindi: slot ' + slot);
}

// ── Users (Admin) ─────────────────────────────────────
function loadUsers() {
  if (!connected || !ws || currentRole !== 'admin') return;
  ws.send(JSON.stringify({ type: 'get_users', admin_pass: adminPass }));
}

function renderUsersList(users, max) {
  var list  = document.getElementById('users-list');
  var count = document.getElementById('users-count');
  if (!list) return;
  if (count) count.textContent = '(' + (users ? users.length : 0) + '/' + (max || 3) + ')';
  list.innerHTML = '';
  if (!users || users.length === 0) {
    list.innerHTML = '<p class="hint-sub">Henuz kullanici yok</p>';
    return;
  }
  users.forEach(function(u) {
    var row = document.createElement('div');
    row.className = 'user-row';
    row.innerHTML =
      '<div>' +
        '<div class="user-name">' + u.username + '</div>' +
        '<div class="user-uid mono">uid: ' + u.uid + '</div>' +
      '</div>' +
      '<button class="btn-small" style="color:var(--red);border-color:rgba(239,68,68,.3)" ' +
        'onclick="deleteUser(' + u.uid + ',\'' + u.username + '\')">Sil</button>';
    list.appendChild(row);
  });
}

function showNewUserForm() {
  document.getElementById('new-user-form').style.display = 'block';
  document.getElementById('new-uname').focus();
  var err = document.getElementById('new-user-err');
  err.style.display = 'none'; err.textContent = '';
}

function cancelNewUser() {
  document.getElementById('new-user-form').style.display = 'none';
  document.getElementById('new-uname').value = '';
  document.getElementById('new-upass').value = '';
}

function createUser() {
  var uname = document.getElementById('new-uname').value.trim();
  var upass = document.getElementById('new-upass').value;
  var err   = document.getElementById('new-user-err');
  err.style.display = 'none';
  if (!uname || uname.length < 2) { err.textContent = 'Min 2 karakter'; err.style.display = 'block'; return; }
  if (!upass || upass.length < 4) { err.textContent = 'Sifre min 4 karakter'; err.style.display = 'block'; return; }
  ws.send(JSON.stringify({ type: 'create_user', username: uname, password: upass, admin_pass: adminPass }));
  cancelNewUser();
}

function deleteUser(uid, username) {
  if (!confirm(username + ' silinsin mi?')) return;
  ws.send(JSON.stringify({ type: 'delete_user', uid: uid, admin_pass: adminPass }));
}

// ── LED Model Seçimi ──────────────────────────────
var selectedModel = 0; // 0=WS2812, 1=WS2811

function selectModel(m) {
    selectedModel = m;
    document.getElementById('model-ws2812').classList.toggle('active', m === 0);
    document.getElementById('model-ws2811').classList.toggle('active', m === 1);
}

// ── Config (Admin) ────────────────────────────────────
function updateCfgTotal() {
  var r  = parseInt(document.getElementById('cfg-rows').value) || 0;
  var c  = parseInt(document.getElementById('cfg-cols').value) || 0;
  var el = document.getElementById('cfg-total');
  if (el) el.textContent = 'Toplam: ' + (r*c) + ' LED' + (r*c === 0 ? ' (LED bagli degil)' : '');
}

function saveConfig() {
  if (!connected) { showHint('cfg-hint', 'Bagli degil'); return; }
  var rows = parseInt(document.getElementById('cfg-rows').value) || 0;
  var cols = parseInt(document.getElementById('cfg-cols').value) || 0;
  var gpio = parseInt(document.getElementById('cfg-gpio').value) || 18;
  var pass = document.getElementById('cfg-pass').value;
  if (!pass) { showHint('cfg-hint', 'Sifre gerekli'); return; }
  ws.send(JSON.stringify({
    type:      'config',
    rows:      rows,
    cols:      cols,
    gpio:      gpio,
    led_model: selectedModel,
    password:  pass
  }));
  showHint('cfg-hint', 'Gonderiliyor...');
  buildStrip('a-strip', rows * cols);
}

// ── Tabs ──────────────────────────────────────────────
function switchTab(el, tabId) {
  document.querySelectorAll('.tab').forEach(function(t) { t.classList.remove('active'); });
  document.querySelectorAll('.tab-content').forEach(function(t) { t.classList.remove('active'); });
  el.classList.add('active');
  document.getElementById(tabId).classList.add('active');
}

// ── Logs ──────────────────────────────────────────────
function addLog(type, tag, msg) {
  var ts = new Date().toLocaleTimeString('tr-TR', { hour:'2-digit', minute:'2-digit', second:'2-digit' });
  allLogs.push({ t: type, tag: tag, msg: msg, ts: ts });
  if (allLogs.length > 200) allLogs.shift();
  renderLogs();
}

function renderLogs() {
  var box = document.getElementById('log-box');
  if (!box) return;
  box.innerHTML = '';
  var list = logFilter === 'all' ? allLogs : allLogs.filter(function(l) { return l.t === logFilter; });
  list.forEach(function(l) {
    var d = document.createElement('div');
    d.className = 'log-line ' + l.t;
    d.textContent = '[' + l.ts + '] ' + l.t + ' ' + l.tag + ': ' + l.msg;
    box.appendChild(d);
  });
  box.scrollTop = box.scrollHeight;
}

function filterLogs(f, btn) {
  logFilter = f;
  document.querySelectorAll('.lf').forEach(function(b) { b.classList.remove('active'); });
  if (btn) btn.classList.add('active');
  renderLogs();
}

function clearLogs() { allLogs = []; renderLogs(); }

// ── Hint ──────────────────────────────────────────────
var hintTimers = {};
function showHint(id, msg) {
  var el = document.getElementById(id);
  if (!el) return;
  el.textContent = msg;
  if (hintTimers[id]) clearTimeout(hintTimers[id]);
  hintTimers[id] = setTimeout(function() { el.textContent = ''; }, 2800);
}

// ── OTA Upload ────────────────────────────────────
var otaChunkSize = 3072; // 3KB chunk — WebSocket için güvenli boyut
var otaRunning   = false;

function startOTA() {
    if (!connected) { showHint('ota-hint', 'Bagli degil'); return; }
    if (otaRunning)  { showHint('ota-hint', 'OTA zaten calisiyor'); return; }

    var file = document.getElementById('ota-file').files[0];
    if (!file) { showHint('ota-hint', 'Once .bin dosyasi sec'); return; }
    if (!file.name.endsWith('.bin')) { showHint('ota-hint', 'Sadece .bin dosyasi kabul edilir'); return; }

    var pass = document.getElementById('ota-pass').value;
    if (!pass) { showHint('ota-hint', 'Admin sifresi gerekli'); return; }

    otaRunning = true;
    document.getElementById('ota-bar-wrap').style.display = 'block';
    setOtaProgress(0, 'Dosya okunuyor...');
    addLog('INFO', 'OTA', 'Basladi: ' + file.name + ' (' + file.size + ' byte)');

    var reader = new FileReader();
    reader.onload = function(e) {
        window._otaBuffer = e.target.result;
        window._otaOffset = 0;
        window._otaPass   = pass;
        setOtaProgress(0, 'ESP32 ile iletisim kuruluyor...');
        ws.send(JSON.stringify({
            type:     'ota_begin',
            password: pass,
            size:     e.target.result.byteLength
        }));
    };
    reader.onerror = function() {
        otaRunning = false;
        showHint('ota-hint', 'Dosya okunamadi');
    };
    reader.readAsArrayBuffer(file);
}

function sendNextOtaChunk() {
    var buffer = window._otaBuffer;
    var offset = window._otaOffset;
    if (!buffer || offset >= buffer.byteLength) return;

    var end   = Math.min(offset + otaChunkSize, buffer.byteLength);
    var chunk = new Uint8Array(buffer, offset, end - offset);
    ws.send(JSON.stringify({ type: 'ota_chunk', data: uint8ToBase64(chunk) }));
    window._otaOffset = end;
}

function uint8ToBase64(bytes) {
    var binary = '';
    var len = bytes.byteLength;
    for (var i = 0; i < len; i++) {
        binary += String.fromCharCode(bytes[i]);
    }
    return btoa(binary);
}

function setOtaProgress(pct, msg) {
    var bar  = document.getElementById('ota-bar');
    var hint = document.getElementById('ota-hint');
    if (bar)  bar.style.width = pct + '%';
    if (hint) hint.textContent = msg || (pct + '%');
}

function abortOTA() {
    if (!connected || !ws) return;
    ws.send(JSON.stringify({ type: 'ota_abort' }));
    otaRunning = false;
    window._otaBuffer = null;
    window._otaOffset = 0;
    setOtaProgress(0, 'Iptal edildi');
    addLog('WARN', 'OTA', 'Kullanici tarafindan iptal edildi');
}

// ── Token Yönetimi ────────────────────────────────
var TOKEN_KEY = 'led_matrix_token';

function saveToken(token) {
    try { localStorage.setItem(TOKEN_KEY, token); } catch(e) {}
}

function getToken() {
    try { return localStorage.getItem(TOKEN_KEY); } catch(e) { return null; }
}

function clearToken() {
    try { localStorage.removeItem(TOKEN_KEY); } catch(e) {}
}

// ── Init ──────────────────────────────────────────────
addLog('INFO', 'APP', 'Arayuz yuklendi');

// Sayfa açılınca token varsa otomatik bağlan
// DOM tamamen yüklendikten sonra token dene
window.addEventListener('load', function() {
    var token = getToken();
    if (token) {
        addLog('INFO', 'AUTH', 'Kayitli token bulundu, otomatik baglaniliyor...');
        pendingLogin = { token: token };
        connectWs();
    }
});