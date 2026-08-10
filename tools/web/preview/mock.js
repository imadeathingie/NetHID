/*
 * NetHID web UI mock.
 *
 * Injected into a COPY of NetHID.html by tools/web/preview.py. Deliberately not part
 * of the page the firmware serves: every byte of NetHID.html is embedded in the
 * binary as a C string literal, and a preview harness has no business taking up
 * flash on a keyboard.
 *
 * It replaces window.fetch with a table of canned responses, so the page runs
 * exactly as it would against a board — same code paths, same render functions,
 * same bugs if there are any.
 *
 * From the browser console:
 *
 *     mock.board('modular')     switch fixture: modular | oledpad | mystery6x6
 *     mock.boards()             list them
 *     mock.online(2, false)     take a module off the bus
 *     mock.set('wpm', 120)      poke a status field
 *     mock.log()                every request the page has made
 *     mock.state                the whole fixture, editable in place
 */
(function () {
  'use strict';

  // ── Fixtures ──────────────────────────────────────────────────────────────
  // Shapes match the real endpoints exactly. If one of these drifts from the
  // firmware, the preview lies — so keep them next to the endpoint they mock.
  const BOARDS = {
    modular: {
      board: 'modular', rows: 9, cols: 6, layers: 8, compiled_layers: 4,
      encoders: 2,
      // Mirrors keyboards/modular/rules.cmake. `autoclick` is a slot COUNT.
      features: { consumer:true, mousekeys:true, macros:true, layers:true,
                  oneshot:true, caps_word:true, autoclick:0 },
      autoclicks: [],
      modules: [
        { id:0, rows:4, cols:6, row_offset:0, encoders:1, primary:true,  online:true },
        { id:1, rows:4, cols:6, row_offset:4, encoders:0, primary:false, online:true },
        { id:2, rows:1, cols:4, row_offset:8, encoders:1, primary:false, online:true },
      ],
      // Two 4x6 halves laid side by side, plus a macropad row underneath.
      geometry: (function () {
        const k = [];
        for (let r = 0; r < 3; r++)
          for (let c = 0; c < 6; c++) k.push([r, c, c * 100, r * 100]);
        for (let c = 2; c < 5; c++) k.push([3, c, c * 100, 300]);
        for (let r = 0; r < 3; r++)
          for (let c = 0; c < 6; c++) k.push([4 + r, c, 700 + (5 - c) * 100, r * 100]);
        for (let c = 2; c < 5; c++) k.push([7, c, 700 + (5 - c) * 100, 300]);
        for (let c = 0; c < 4; c++) k.push([8, c, 200 + c * 100, 450]);
        return k;
      })(),
    },
    oledpad: {
      board: 'oledpad', rows: 3, cols: 4, layers: 8, compiled_layers: 2,
      encoders: 1, modules: null,
      features: { consumer:true, mousekeys:true, macros:true, layers:true,
                  oneshot:false, caps_word:false, autoclick:3 },
      // = the AUTOCLICKS table in keyboards/oledpad/keyboard.h.
      // 0x5908 is MS_BTN1 (KMQ.MOUSE | 8); triggers are AC_HOLD 1, TAP2 2, TAP3 4.
      autoclicks: [ { target:0x5908, interval_ms:100, trigger:1 },
                    { target:0x002C, interval_ms:50,  trigger:2 },
                    { target:0x5908, interval_ms:25,  trigger:5 } ],
      geometry: (function () {
        const k = [];
        for (let r = 0; r < 3; r++)
          for (let c = 0; c < 4; c++) k.push([r, c, c * 100, r * 100]);
        return k;
      })(),
    },
    proto2x2: {
      board: 'proto2x2', rows: 2, cols: 2, layers: 8, compiled_layers: 2,
      encoders: 0, modules: null,
      // No CONSUMER and no AUTOCLICK — this is the board that shows the editor
      // correctly absent rather than present and inert.
      features: { consumer:false, mousekeys:true, macros:true, layers:true,
                  oneshot:true, caps_word:false, autoclick:0 },
      autoclicks: [],
      geometry: [[0,0,0,0],[0,1,100,0],[1,0,0,100],[1,1,100,100]],
    },
    mystery6x6: {
      board: 'mystery6x6', rows: 6, cols: 6, layers: 8, compiled_layers: 2,
      encoders: 0, modules: null,
      features: { consumer:true, mousekeys:true, macros:true, layers:true,
                  oneshot:false, caps_word:false, autoclick:3 },
      // = the AUTOCLICKS table in keyboards/mystery6x6/keyboard.h.
      autoclicks: [ { target:0x5908, interval_ms:100, trigger:1 },
                    { target:0x002C, interval_ms:50,  trigger:2 },
                    { target:0x5908, interval_ms:25,  trigger:5 } ],
      geometry: [
        [0,0,0,0],[0,1,100,0],[0,2,200,0],[0,3,300,0],[0,4,400,0],[0,5,500,0],
        [1,0,0,100],[1,1,100,100],[1,2,200,100],[1,3,300,100],[1,4,400,100],[1,5,500,100],
        [2,0,0,200],[2,1,100,200],[2,2,200,200],[2,3,300,200],[2,4,400,200],[2,5,500,200],
        [3,0,0,300],[3,1,100,300],[3,2,200,300],[3,3,300,300],[3,4,400,300],[3,5,500,300],
        [4,2,200,400],[4,3,300,400],[4,4,400,500],[4,5,500,500],
        [4,0,550,600],[4,1,500,700],[5,0,600,700],[5,1,550,800],
      ],
    },
  };

  const KC = { NO:0, TRNS:1, A:0x04, ENT:0x28, ESC:0x29, BSPC:0x2A, TAB:0x2B, SPC:0x2C };
  const MO = (l) => 0x5100 | l;
  const CC = { MUTE:0x5A00, VOLU:0x5A01, VOLD:0x5A02 };   // consumer indices

  // = include/kb/autoclick.h. The editor shows these in its warnings, so a
  // preview that invents its own numbers teaches you the wrong limits.
  const AC_MIN_MS = 8, AC_MAX_MS = 5000, AC_TAP_WINDOW_MS = 250;

  const state = {
    name: 'modular',
    fx: null,
    layers: [],          // layers[l] = flat rows*cols array
    macros: {},
    enc: [],
    dirty: false,
    stored: false,
    settings: null,
    wifi: null,
    pw: null,           // the mock's idea of the login password
    ac: null,           // {slots, stored, dirty} — null when the board has none
    log: [],
    unhandled: [],      // requests that reached the catch-all; see route()
  };

  function buildLayers() {
    const fx = state.fx, n = fx.rows * fx.cols;
    state.layers = [];
    state.enc = [];
    for (let l = 0; l < fx.layers; l++) {
      const e = [];
      for (let i = 0; i < (fx.encoders || 0); i++)
        e.push(l === 0 ? [CC.VOLD, CC.VOLU, CC.MUTE] : [KC.TRNS, KC.TRNS, KC.TRNS]);
      state.enc.push(e);
    }
    for (let l = 0; l < fx.layers; l++) {
      const a = new Array(n).fill(l === 0 ? KC.NO : KC.TRNS);
      if (l === 0) {
        // Something legible on the base layer: letters across the placed keys.
        fx.geometry.forEach((g, i) => { a[g[0] * fx.cols + g[1]] = KC.A + (i % 26); });
        if (fx.geometry.length > 3) {
          const g = fx.geometry[fx.geometry.length - 1];
          a[g[0] * fx.cols + g[1]] = MO(1);
        }
      }
      state.layers.push(a);
    }
  }

  function defaultSettings() {
    const f = (name, type, value, dflt, min, max, help) =>
      ({ name, type, value, default: dflt, min, max, overridden: value !== dflt, help });
    return { ok: true, dirty: false, fields: [
      f('quiet_boot','bool',0,0,0,1,'Stop typing boot diagnostics into the host'),
      f('debug_matrix','bool',0,0,0,1,'Log every matrix edge to the serial console'),
      f('type_delay_ms','int',8,8,0,100,'Delay between typed characters'),
      f('ap_auto_fallback','bool',0,0,0,1,'Start setup mode when no known network is in range'),
      f('session_timeout_s','int',300,300,30,86400,'Idle time before a web session expires'),
      f('lockout_s','int',30,30,5,3600,'Lockout after too many failed logins'),
      f('max_auth_attempts','int',5,5,1,50,'Failed logins before the lockout applies'),
      f('tapping_term_ms','int',185,200,50,1000,'How long a dual-role key must be held'),
    ]};
  }

  function defaultWifi() {
    return {
      ok: true, ap_mode: false, dirty: false, stored: 1, max: 8,
      networks: [
        { ssid: 'HomeNetwork', auth: 0, source: 'stored' },
        { ssid: 'CompiledNet', auth: 0, source: 'compiled' },
      ],
    };
  }

  function load(name) {
    if (!BOARDS[name]) throw new Error('unknown board: ' + name);
    state.name = name;
    state.fx = JSON.parse(JSON.stringify(BOARDS[name]));
    buildLayers();
    state.macros = { 0: [ { t:'text', value:'hello from the mock' } ] };
    state.settings = defaultSettings();
    state.wifi = defaultWifi();
    // can_change mirrors ALLOW_PLAINTEXT_AUTH||ENABLE_HTTPS on the device;
    // flip it from the console with mock.state.pw.can_change = false.
    state.pw = { value: 'changeme', changed: false, can_change: true, why: '' };
    state.ac = { slots: JSON.parse(JSON.stringify(state.fx.autoclicks || [])),
                 stored: false, dirty: false };
    state.dirty = false;
  }

  // ── Routes ────────────────────────────────────────────────────────────────
  function info() {
    const fx = state.fx;
    const o = { ok:true, board:fx.board, rows:fx.rows, cols:fx.cols,
                layers:fx.layers, compiled_layers:fx.compiled_layers,
                encoders:fx.encoders || 0,
                stored:state.stored, dirty:state.dirty, saving:false };
    // What the firmware was compiled with. Omitting this does not merely lose
    // the feature gating: the editor treats a missing `features` as an older
    // build and assumes everything is present, so the preview would offer
    // AUTO0-15 on a board with three slots and MEDIA on one with no consumer
    // report — the exact confusion this object exists to remove.
    if (fx.features) o.features = fx.features;
    if (fx.modules) o.modules = fx.modules;
    return o;
  }

  function route(url, opt) {
    const body = (opt && opt.body) ? JSON.parse(opt.body) : null;
    const post = !!(opt && opt.method === 'POST');
    state.log.push((post ? 'POST ' : 'GET  ') + url);

    if (url === '/api/authinfo')  return { multiuser: false };
    if (url === '/api/whoami')    return { user: 'preview' };
    if (url === '/api/ping')      return { ok: true };
    if (url === '/api/hostinfo')  return { os: 'unknown' };

    if (url === '/api/keymap/info')   return info();
    if (url === '/api/keymap/layout') return { ok:true, unit:100, keys:state.fx.geometry };
    let me = url.match(/^\/api\/keymap\/encoders\/(\d+)$/);
    if (me) return { ok:true, layer:+me[1], encoders: state.enc[+me[1]] || [] };
    if (url === '/api/keymap/encoders' && post) {
      state.enc[body.layer][body.index][body.action] = body.kc;
      state.dirty = true;
      return { ok:true, live:true };
    }

    let m = url.match(/^\/api\/keymap\/(\d+)$/);
    if (m) return { ok:true, layer:+m[1], keys: state.layers[+m[1]] || [] };

    if (url === '/api/keymap' && post) {
      state.layers[body.layer] = body.keys;
      state.dirty = true;
      return { ok:true, live:true };
    }
    if (url === '/api/keymap/save')  { state.dirty = false; state.stored = true; return { ok:true, queued:true }; }
    if (url === '/api/keymap/reset') { buildLayers(); state.dirty = true; return { ok:true }; }

    if (url === '/api/macro' && !post) {
      const list = Object.keys(state.macros).map(k => ({ id:+k, steps:state.macros[k] }));
      const used = JSON.stringify(state.macros).length;
      return { ok:true, count:16, used, size:3072, dirty:state.dirty,
               stored:state.stored, macros:list };
    }
    if (url === '/api/macro' && post) {
      if (body.steps && body.steps.length) state.macros[body.id] = body.steps;
      else delete state.macros[body.id];
      return { ok:true, bytes:0, used:JSON.stringify(state.macros).length, size:3072 };
    }
    if (url === '/api/macro/save')  return { ok:true, queued:true };
    if (url === '/api/macro/clear') { state.macros = {}; return { ok:true }; }

    // Autoclick. A board with no slots must answer as the firmware does — a
    // count of 0 — because the editor hides itself on that, and the catch-all
    // below would instead return {ok:true} with no count and look like a bug in
    // the page rather than a board without the feature.
    if (url === '/api/autoclick' && !post) {
      return { ok:true, count:state.ac.slots.length,
               min_ms:AC_MIN_MS, max_ms:AC_MAX_MS, tap_window_ms:AC_TAP_WINDOW_MS,
               stored:state.ac.stored, dirty:state.ac.dirty, saving:false,
               rate_override_ms:0, slots:state.ac.slots };
    }
    if (url === '/api/autoclick' && post) {
      const i = body.slot;
      if (!(i >= 0 && i < state.ac.slots.length)) return { error:'bad params' };
      // Mirrors the firmware's validation, so the preview rejects exactly what a
      // board rejects instead of accepting edits that would 400 on hardware.
      if (!body.trigger || (body.trigger & 2 && body.trigger & 4) || (body.trigger & ~7))
        return { error:'bad trigger' };
      if (body.interval_ms < AC_MIN_MS || body.interval_ms > AC_MAX_MS)
        return { error:'bad interval', min_ms:AC_MIN_MS, max_ms:AC_MAX_MS };
      if (!body.target) return { error:'rejected', detail:'target cannot be KC_NO' };
      state.ac.slots[i] = { target:body.target, interval_ms:body.interval_ms,
                            trigger:body.trigger };
      state.ac.dirty = true;
      return { ok:true };
    }
    if (url === '/api/autoclick/save') {
      state.ac.dirty = false; state.ac.stored = true;
      return { ok:true, queued:true };
    }
    if (url === '/api/autoclick/reset') {
      state.ac.slots = JSON.parse(JSON.stringify(state.fx.autoclicks || []));
      state.ac.dirty = true;
      if (body && body.erase) state.ac.stored = false;
      return { ok:true };
    }

    // Login password. The firmware never returns a password; neither does this,
    // so the preview cannot teach a habit the device does not support.
    if (url === '/api/password' && !post) {
      // Every field src/web.cpp sends, including can_change/why. A field the
      // firmware sends and this does not is drift the unhandled-route check
      // cannot see: the request is modelled, the RESPONSE is not, and the page
      // silently takes the default branch for a state it should be showing.
      return { ok:true, user:'preview', multiuser:false, min_len:8, max_len:63,
               changed:state.pw.changed, stored:state.pw.changed, saving:false,
               build_id:'preview0', secure:false,
               can_change:state.pw.can_change, why:state.pw.why };
    }
    if (url === '/api/password' && post) {
      if (!state.pw.can_change) return { error:'plaintext_forbidden' };
      if (body.current !== state.pw.value) return { error:'wrong_password' };
      if (!body.new || body.new.length < 8 || body.new.length > 63)
        return { error:'bad_length', min_len:8, max_len:63 };
      if (body.new === body.current) return { error:'unchanged' };
      state.pw.value = body.new; state.pw.changed = true;
      return { ok:true, queued:true, sessions_ended:0 };
    }
    // `&& post` like every other route here: the firmware only answers this to a
    // POST, and modelling a GET-able password reset teaches the wrong shape.
    if (url === '/api/password/reset' && post) {
      if (!state.pw.can_change) return { error:'plaintext_forbidden' };
      if (body.current !== state.pw.value) return { error:'wrong_password' };
      state.pw = Object.assign(state.pw, { value:'changeme', changed:false });
      return { ok:true, reset:true };
    }

    if (url === '/api/settings' && !post) return state.settings;
    if (url === '/api/settings' && post) {
      const f = state.settings.fields.find(x => x.name === body.field);
      if (!f) return { error: 'unknown_field' };
      if (body.value < f.min || body.value > f.max)
        return { error: 'unknown_field_or_out_of_range' };
      f.value = body.value; f.overridden = f.value !== f.default;
      state.settings.dirty = true;
      return { ok:true };
    }
    if (url === '/api/settings/reset' && post) {
      if (body.all) state.settings = defaultSettings();
      else {
        const f = state.settings.fields.find(x => x.name === body.field);
        if (!f) return { error:'unknown_field' };
        f.value = f.default; f.overridden = false;
      }
      state.settings.dirty = true;
      return { ok:true };
    }
    if (url === '/api/settings/save') { state.settings.dirty = false; return { ok:true, queued:true }; }

    if (url === '/api/wifi' && !post) return state.wifi;
    if (url === '/api/wifi/scan') return { ok:true, cached:true, networks:[
      { ssid:'HomeNetwork', rssi:-42, secure:true },
      { ssid:'Neighbour',   rssi:-71, secure:true },
      { ssid:'CoffeeShop',  rssi:-83, secure:false },
    ]};
    if (url === '/api/wifi' && post) {
      state.wifi.networks.unshift({ ssid:body.ssid, auth:body.auth, source:'stored' });
      state.wifi.stored++; state.wifi.dirty = true; return { ok:true };
    }
    if (url === '/api/wifi/forget' && post) {
      state.wifi.networks = state.wifi.networks.filter(
        n => !(n.ssid === body.ssid && n.source === 'stored'));
      state.wifi.dirty = true; return { ok:true };
    }
    if (url === '/api/wifi/save')  { state.wifi.dirty = false; return { ok:true, queued:true }; }
    if (url === '/api/wifi/apply') return { ok:true, rebooting:true };

    // Everything the mock does not model answers ok rather than 404, so a
    // preview is never blocked by an endpoint nobody is previewing.
    //
    // That leniency is also how the autoclick editor stayed invisible in the
    // preview long after it worked on hardware: /api/autoclick fell through to
    // here, came back without a `count`, and the panel hid itself exactly as it
    // does on a board with the feature compiled out. Record what fell through so
    // check_preview.js can fail on it instead of it being discovered by eye.
    state.unhandled.push((post ? 'POST ' : 'GET  ') + url);
    return { ok: true, mock: 'unhandled', path: url };
  }

  load('modular');

  const realFetch = window.fetch;
  window.fetch = function (url, opt) {
    if (typeof url === 'string' && url.charAt(0) === '/') {
      const data = route(url, opt);
      return Promise.resolve({
        ok: true, status: 200,
        json: async () => data,
        text: async () => JSON.stringify(data),
        headers: { get: () => 'application/json' },
      });
    }
    return realFetch.apply(window, arguments);
  };

  // ── Console API ───────────────────────────────────────────────────────────
  function reload() {
    // Re-run the page's own loaders rather than reimplementing them, so the
    // preview exercises the same code path a real board would.
    if (typeof window.kmLoad === 'function') window.kmLoad(true);
    if (typeof window.stLoad === 'function') window.stLoad();
    if (typeof window.wfLoad === 'function') window.wfLoad();
  }

  window.mock = {
    get state() { return state; },
    boards: () => Object.keys(BOARDS),
    board(name) {
      if (!name) return state.name;
      load(name);
      reload();
      return 'now previewing: ' + name;
    },
    online(id, up) {
      if (!state.fx.modules) return 'this board has no modules';
      const m = state.fx.modules.find(x => x.id === id);
      if (!m) return 'no module ' + id;
      m.online = (up !== false);
      reload();
      return 'module ' + id + (m.online ? ' online' : ' offline');
    },
    apMode(on) { state.wifi.ap_mode = (on !== false); reload(); return state.wifi.ap_mode; },
    set(field, value) {
      const f = state.settings.fields.find(x => x.name === field);
      if (!f) return 'no setting ' + field;
      f.value = value; f.overridden = f.value !== f.default; reload(); return 'ok';
    },
    log: () => state.log.slice(),
    // Endpoints the page asked for that this mock does not model. Anything in
    // here is a part of the UI the preview is silently not exercising.
    unhandled: () => state.unhandled.slice(),
    reload,
  };

  const banner = document.createElement('div');
  banner.style.cssText = 'position:fixed;bottom:8px;right:8px;z-index:9999;' +
    'background:#5c3a00;color:#ffce8a;padding:6px 10px;border-radius:5px;' +
    'font:600 11px/1.4 ui-monospace,monospace;opacity:.9';
  banner.textContent = 'PREVIEW - no board attached. mock.board() in the console.';
  banner.title = 'Generated by tools/web/preview.py';
  window.addEventListener('load', () => document.body.appendChild(banner));

  console.log('%cNetHID preview', 'font-weight:bold');
  console.log('mock.boards()        ', Object.keys(BOARDS).join(', '));
  console.log("mock.board('oledpad') switch fixture");
  console.log('mock.online(2, false) take a module off the bus');
  console.log('mock.log()            every request the page has made');
})();
