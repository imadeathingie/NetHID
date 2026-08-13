/*
 * check_picker_ui.js — the keycode picker, under jsdom.
 *
 * Two failures this exists to catch, both of which shipped:
 *
 *   1. Autoclick was reachable only from the advanced dropdown and absent from
 *      the searchable list, so searching the picker for "autoclick" or "click"
 *      found nothing and the feature looked like it did not exist at all.
 *
 *   2. A keycode whose feature was compiled out was still offered, accepted,
 *      stored and drawn on the key — and then did nothing when pressed, with no
 *      error anywhere. `features` in /api/keymap/info is what lets the editor
 *      say so; this asserts it is actually acted on.
 *
 * The third case — firmware that sends no `features` at all — matters because
 * a missing field is ignorance, not a "no": an older build must not have its
 * working options hidden.
 *
 * Needs jsdom:  npm i jsdom
 *
 *     ./tools/check/check_picker_ui.sh          # skips cleanly without jsdom
 */
const { JSDOM } = require('jsdom');
const fs = require('fs');

const html = fs.readFileSync(process.argv[2], 'utf8');

function makeInfo(features, layout) {
  const i = {
    ok: true, board: 'mystery6x6', rows: 6, cols: 6, layers: 4,
    compiled_layers: 2, encoders: 0, stored: false, dirty: false, saving: false,
  };
  if (features !== undefined) i.features = features;
  // Omitted entirely for the "older firmware" case: no `layout` must mean US,
  // which is what such a build was assuming anyway.
  if (layout !== undefined) i.layout = layout;
  return i;
}

let fails = 0;
function ok(cond, what) {
  console.log((cond ? 'PASS  ' : 'FAIL  ') + what);
  if (!cond) fails++;
}

// Autoclick state the fake firmware reports, and the POSTs it received — so a
// test can assert what actually went over the wire, not just what the DOM says.
function makeAutoclick(count) {
  return {
    ok: true, count, min_ms: 8, max_ms: 5000, tap_window_ms: 250,
    stored: false, dirty: false, saving: false, rate_override_ms: 0,
    slots: Array.from({ length: count }, (_, i) => ({
      target: 0x5908, interval_ms: 100 * (i + 1), trigger: 1,
    })),
  };
}

async function run(label, features, checks, opts) {
  opts = opts || {};
  const INFO = makeInfo(features, opts.layout);
  const AC = opts.autoclick === null ? null
           : makeAutoclick(opts.autoclick === undefined ? 3 : opts.autoclick);
  const posts = [];
  const dom = new JSDOM(html, {
    runScripts: 'dangerously', pretendToBeVisual: true,
    beforeParse(w) {
      w.fetch = (url, init) => {
        if (init && init.method === 'POST') {
          posts.push({ url, body: JSON.parse(init.body) });
          if (url === '/api/autoclick') {
            const b = posts[posts.length - 1].body;
            // Mirror the firmware's validation, so the test fails if the UI
            // starts sending something the device would reject.
            const t = b.trigger;
            if (!t || (t & ~7) || ((t & 2) && (t & 4)))
              return Promise.resolve({ ok: true, status: 400, json: async () => ({ error: 'bad trigger' }) });
            if (b.interval_ms < 8 || b.interval_ms > 5000)
              return Promise.resolve({ ok: true, status: 400, json: async () => ({ error: 'bad interval' }) });
            Object.assign(AC.slots[b.slot], {
              target: b.target, interval_ms: b.interval_ms, trigger: b.trigger,
            });
          }
          return Promise.resolve({ ok: true, status: 200, json: async () => ({ ok: true }) });
        }
        let body = {};
        if (url === '/api/keymap/info') body = INFO;
        else if (url === '/api/keymap/layout') body = { ok: true, unit: 100, keys: [] };
        else if (/^\/api\/keymap\/\d+$/.test(url))
          body = { ok: true, layer: 0, keys: new Array(36).fill(4) };
        else if (url === '/api/macro') body = { ok: true, count: 16, used: 0, size: 3072, macros: [] };
        else if (url === '/api/autoclick') body = AC || { error: 'not_found' };
        return Promise.resolve({ ok: true, status: 200, json: async () => body });
      };
      w.alert = () => {}; w.confirm = () => true;
      w.matchMedia = () => ({ matches: false, addEventListener() {}, removeEventListener() {} });
    },
  });
  const w = dom.window;
  await new Promise(r => w.addEventListener('load', r));
  await w.kmLoad(true);
  await new Promise(r => setTimeout(r, 30));

  console.log('\n── ' + label);
  w.kmPickOpen(0, 0);
  const els = [...w.document.querySelectorAll('#kmList .km-opt')];
  const groups = [...w.document.querySelectorAll('#kmList .km-glabel')].map(h => h.textContent);
  await checks({ w, opts: els, groups, names: els.map(b => b.dataset.kc), posts, AC });
  dom.window.close();
}

(async () => {
  // A board with autoclick slots: they must be in the list AND findable by the
  // word someone would type, which is the bug being fixed.
  await run('mystery6x6: consumer + 3 autoclick slots',
    { consumer: true, mousekeys: true, macros: true, layers: true,
      oneshot: false, caps_word: false, autoclick: 3 },
    ({ w, opts, groups, names }) => {
      ok(groups.includes('AUTOCLICK'), 'AUTOCLICK group present');
      ok(groups.includes('MEDIA'), 'MEDIA group present');
      ok(names.includes('AUTO0') && names.includes('AUTO2'), 'AUTO0..AUTO2 offered');
      ok(!names.includes('AUTO3'), 'AUTO3 not offered (only 3 slots)');
      ok(names.includes('KC_MUTE') || names.includes('MUTE'), 'media keys offered');

      // Search must find it. This is precisely what failed before.
      const search = w.document.getElementById('kmSearch');
      search.value = 'click'; w.kmFilter();
      const visible = opts.filter(b => b.style.display !== 'none').map(b => b.dataset.kc);
      ok(visible.includes('AUTO0'), 'searching "click" finds AUTO0');
      search.value = 'autoclick'; w.kmFilter();
      ok(opts.filter(b => b.style.display !== 'none').map(b => b.dataset.kc).includes('AUTO1'),
         'searching "autoclick" finds AUTO1');

      // Punctuation by the character it prints, not by its four-letter name.
      // Typing ";" used to match nothing at all: the searchable text was the
      // name plus the group's keywords, and neither contains a ";".
      const seek = q => {
        search.value = q; w.kmFilter();
        return opts.filter(b => b.style.display !== 'none').map(b => b.dataset.kc);
      };
      const finds = (q, name) => ok(seek(q).includes(name),
        'searching ' + JSON.stringify(q) + ' finds ' + name);
      finds(';', 'SCLN');
      finds(':', 'SCLN');          // the shifted face points at the same key
      finds('.', 'DOT');
      finds('.', 'PDOT');          // and at the keypad's own period
      finds(',', 'COMM');
      finds('/', 'SLSH');
      finds('/', 'PSLS');
      finds('\\', 'BSLS');
      finds('[', 'LBRC');
      finds('!', '1');             // shifted number row
      finds('*', 'PAST');
      finds('comma', 'COMM');      // and by the name you say out loud
      finds('semicolon', 'SCLN');
      finds('backslash', 'BSLS');
      finds('asterisk', 'PAST');

      // A character must not drag in everything that merely mentions it in a
      // group keyword — "-" once matched the whole INTERNATIONAL group,
      // through the hyphen in "non-us".
      const dash = seek('-');
      ok(dash.includes('MINS') && dash.includes('PMNS'),
         'searching "-" finds MINS and PMNS');
      ok(!dash.includes('KANA') && !dash.includes('HANJ'),
         'searching "-" does not drag in the international group');

      search.value = ''; w.kmFilter();

      // Slot range is clamped to what the board declared.
      w.document.getElementById('kmAdvType').value = 'AUTOCLICK';
      w.kmAdvSync();
      ok(w.document.getElementById('kmAdvNum').max === '2', 'slot # max clamped to 2');
      ok(w.document.getElementById('kmAdvNote').textContent === '', 'no warning when supported');

      // A supported family warns about nothing; encoding is unchanged.
      w.document.getElementById('kmAdvNum').value = '1';
      w.kmAdvSync();
      ok(w.kmAdvValue() === (0x5B00 | 1), 'AUTOCLK(1) encodes to 0x5B01');
    });

  // ── UK layout ─────────────────────────────────────────────────────────────
  // A usage is a position on the board, not a character. On UK, "the key that
  // types @" is the apostrophe key and "the key that types a backslash" is one
  // a US board does not have at all — so a picker that answers with the US key
  // is not merely unhelpful, it is wrong.
  await run('UK layout', undefined, ({ w, opts }) => {
    const search = w.document.getElementById('kmSearch');
    const seek = q => {
      search.value = q; w.kmFilter();
      return opts.filter(b => b.style.display !== 'none').map(b => b.dataset.kc);
    };
    const face = n => {
      const b = opts.find(e => e.dataset.kc === n);
      const f = b && b.querySelector('.km-face');
      return f ? f.textContent : '';
    };

    const at = seek('@');
    ok(at.includes('QUOT'), 'UK: "@" finds QUOT');
    ok(!at.includes('2'), 'UK: "@" does not offer the 2 key');

    const dq = seek('"');
    ok(dq.includes('2'), 'UK: a double quote finds the 2 key');
    ok(!dq.includes('QUOT'), 'UK: a double quote does not offer QUOT');

    const bs = seek('\\');
    ok(bs.includes('NUBS'), 'UK: a backslash finds NUBS');
    ok(!bs.includes('BSLS'), 'UK: a backslash does not offer BSLS, which types #');

    const hash = seek('#');
    ok(hash.includes('NUHS'), 'UK: "#" finds NUHS');
    ok(!hash.includes('3'), 'UK: "#" does not offer the 3 key');

    ok(seek('£').includes('3'), 'UK: a pound sign finds the 3 key');
    ok(seek('~').includes('NUHS'), 'UK: "~" finds NUHS');
    ok(!seek('~').includes('GRV'), 'UK: "~" does not offer GRV, which types not-sign');

    // Layout-independent keys must keep working, or the delta has replaced the
    // table instead of being merged over it.
    ok(seek(';').includes('SCLN'), 'UK: ";" still finds SCLN');
    ok(seek('[').includes('LBRC'), 'UK: "[" still finds LBRC');

    search.value = ''; w.kmFilter();
    ok(face('QUOT') === '\' @', 'UK: QUOT is drawn as \' @');
    ok(face('2') === '2 "', 'UK: the 2 key is drawn as 2 "');
    ok(face('SCLN') === '; :', 'UK: SCLN keeps its US face');
  }, { layout: 1 });

  await run('US layout is unaffected', undefined, ({ w, opts }) => {
    const search = w.document.getElementById('kmSearch');
    const seek = q => {
      search.value = q; w.kmFilter();
      return opts.filter(b => b.style.display !== 'none').map(b => b.dataset.kc);
    };
    ok(seek('@').includes('2'), 'US: "@" finds the 2 key');
    ok(seek('"').includes('QUOT'), 'US: a double quote finds QUOT');
    ok(seek('\\').includes('BSLS'), 'US: a backslash finds BSLS');
    ok(seek('#').includes('3'), 'US: "#" finds the 3 key');
    search.value = ''; w.kmFilter();
  }, { layout: 0 });

  // Features compiled out must not be silently offered.
  await run('board without consumer/mousekeys/autoclick',
    { consumer: false, mousekeys: false, macros: true, layers: true,
      oneshot: false, caps_word: false, autoclick: 0 },
    ({ w, groups, names }) => {
      ok(!groups.includes('MEDIA'), 'MEDIA group hidden');
      ok(!groups.includes('MOUSE'), 'MOUSE group hidden');
      ok(!groups.includes('AUTOCLICK'), 'AUTOCLICK group hidden');
      ok(!names.includes('AUTO0'), 'no autoclick slots offered');

      w.document.getElementById('kmAdvType').value = 'MEDIA';
      w.kmAdvSync();
      ok(/KB_FEATURE_CONSUMER/.test(w.document.getElementById('kmAdvNote').textContent),
         'MEDIA warns it was compiled out');
      ok(w.document.getElementById('kmAdvNoteWrap').style.display !== 'none',
         'warning row visible');

      w.document.getElementById('kmAdvType').value = 'AUTOCLICK';
      w.kmAdvSync();
      ok(/no autoclick slots/.test(w.document.getElementById('kmAdvNote').textContent),
         'AUTOCLICK warns there are no slots');
    });

  // Older firmware sends no `features` at all: assume everything works rather
  // than hiding options that may well be fine.
  await run('firmware with no features field (older build)', undefined,
    ({ groups, names }) => {
      ok(groups.includes('MEDIA'), 'MEDIA still offered');
      ok(groups.includes('MOUSE'), 'MOUSE still offered');
      ok(groups.includes('AUTOCLICK'), 'AUTOCLICK still offered');
      ok(names.includes('AUTO15'), 'full 16-slot range offered when unknown');
    });

  // ── The slot editor ───────────────────────────────────────────────────────
  // Slots were compile-time only: changing what AUTOCLK(0) repeats, or how
  // fast, meant editing keyboard.h and reflashing.
  await run('autoclick slot editor', undefined, async ({ w, posts, AC }) => {
    w.kmPickClose();
    ok(!w.document.getElementById('acWrap').hidden, 'editor shown when slots exist');
    ok(w.document.querySelectorAll('#acSlots .mb-slot').length === 3, 'three slot chips');
    ok(w.document.getElementById('acMs').value === '100', 'slot 0 rate shown');

    // Rate.
    w.document.getElementById('acMs').value = '40';
    await w.acPush();
    const rate = posts.filter(p => p.url === '/api/autoclick').pop();
    ok(rate && rate.body.slot === 0 && rate.body.interval_ms === 40,
       'rate change POSTs slot 0 at 40 ms');

    // Trigger: ticking triple must untick double, because the firmware rejects
    // both and would otherwise 400 on every save.
    w.document.getElementById('acTap3').checked = true;
    await w.document.getElementById('acTap3').onchange();
    ok(!w.document.getElementById('acTap2').checked, 'triple-tap unticks double-tap');
    const trig = posts.filter(p => p.url === '/api/autoclick').pop();
    ok(trig.body.trigger === (1 | 4), 'trigger sent as hold|tap3');

    // A slot with no trigger can never start; refuse before sending.
    const before = posts.length;
    w.document.getElementById('acHold').checked = false;
    w.document.getElementById('acTap3').checked = false;
    await w.acPush();
    ok(posts.length === before, 'empty trigger is not POSTed');
    ok(/at least one trigger/.test(w.document.getElementById('acWarn').textContent),
       'empty trigger explains itself');

    // Out-of-range rate likewise.
    w.document.getElementById('acHold').checked = true;
    w.document.getElementById('acMs').value = '2';
    const before2 = posts.length;
    await w.acPush();
    ok(posts.length === before2, 'below-minimum rate is not POSTed');
    ok(/8–5000|8-5000/.test(w.document.getElementById('acWarn').textContent),
       'rate warning names the range');

    // Retargeting through the shared key picker.
    w.acPickOpen(1);
    ok(w.document.getElementById('kmPicker').classList.contains('on'), 'picker opens for a target');
    await w.kmApply(w.KC ? w.KC.A : 0x04);
    const tgt = posts.filter(p => p.url === '/api/autoclick').pop();
    ok(tgt.body.slot === 1 && tgt.body.target === 0x04, 'target retargets slot 1');
    ok(AC.slots[1].target === 0x04, 'firmware-side slot updated');

    // A slot repeating another autoclick key is a loop that never fires.
    const before3 = posts.length;
    w.acPickOpen(2);
    await w.kmApply(0x5B00);
    ok(posts.length === before3, 'autoclick target rejected client-side');

    // Esc-ing the picker must not leave the target armed, or the next key edit
    // silently retargets a slot instead of setting a key.
    w.acPickOpen(0);
    w.kmPickClose();
    ok(w.acSel === null || w.acSel === undefined, 'closing the picker disarms the target');
  });

  await run('board with autoclick compiled out', undefined,
    async ({ w }) => {
      ok(w.document.getElementById('acWrap').hidden, 'editor hidden when the API 404s');
    }, { autoclick: null });

  console.log(fails ? '\n' + fails + ' FAILED' : '\nall green');
  process.exit(fails ? 1 : 0);
})();
