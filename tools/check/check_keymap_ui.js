/*
 * check_keymap_ui.js — the keymap editor's module handling, under jsdom.
 *
 * A modular board's matrix is one flat array; only the module table says which
 * rows belong to which physical board. Getting that mapping wrong does not
 * throw — it silently shows you the wrong half of the keyboard — so it is worth
 * asserting rather than eyeballing.
 *
 * Note the test drives the selection by CLICKING the module chips rather than
 * assigning kmModSel. A `let` at the top level of a <script> is not a property
 * of window (unlike a function declaration), so a test that assigns it silently
 * creates a second variable the page never reads, and every assertion after it
 * is meaningless.
 *
 * Needs jsdom:  npm i jsdom
 *
 *     ./tools/check/check_keymap_ui.sh          # skips cleanly without jsdom
 */
// Exercise the editor's module logic against a simulated modular board, so the
// row-to-module mapping is checked rather than eyeballed.
const { JSDOM } = require('jsdom');
const fs = require('fs');

const html = fs.readFileSync(process.argv[2], 'utf8');
const INFO = {
  ok: true, board: 'modular', rows: 9, cols: 6, layers: 8, compiled_layers: 4,
  stored: false, dirty: false, saving: false,
  modules: [
    { id:0, rows:4, cols:6, row_offset:0, encoders:1, primary:true,  online:true  },
    { id:1, rows:4, cols:6, row_offset:4, encoders:0, primary:false, online:true  },
    { id:2, rows:1, cols:4, row_offset:8, encoders:1, primary:false, online:false },
  ],
};

const dom = new JSDOM(html, {
  runScripts: 'dangerously', pretendToBeVisual: true,
  beforeParse(w) {
    w.fetch = (url) => {
      let body = {};
      if (url === '/api/keymap/info') body = INFO;
      else if (url === '/api/keymap/layout') body = { ok:true, unit:100, keys:[] };
      else if (/^\/api\/keymap\/\d+$/.test(url))
        body = { ok:true, layer:0, keys:new Array(54).fill(4) };
      else if (url === '/api/macro') body = { ok:true, count:16, used:0, size:3072, macros:[] };
      return Promise.resolve({ ok:true, status:200, json: async () => body });
    };
    w.console.error = () => {};
  },
});

const w = dom.window;
setTimeout(async () => {
  const out = [];
  const ck = (n, ok) => out.push((ok ? 'PASS  ' : 'FAIL  ') + n);
  try {
    await w.kmLoad(true);

    ck('module buttons rendered',
       w.document.querySelectorAll('#kmModBtns .km-mbtn').length === 4);   // All + 3

    ck('row 0 belongs to module 0', w.kmModuleOfRow(0).id === 0);
    ck('row 5 belongs to module 1', w.kmModuleOfRow(5).id === 1);
    ck('row 8 belongs to module 2', w.kmModuleOfRow(8).id === 2);
    ck('row 9 belongs to nothing',  w.kmModuleOfRow(9) === null);

    // Drive the selection by CLICKING, not by poking kmModSel: `let` at the top
    // level of a script is not a window property (unlike a function
    // declaration), so a test that assigns it silently creates a second
    // variable the page never reads. Clicking is what a user does anyway.
    const btn = (i) => w.document.querySelectorAll('#kmModBtns .km-mbtn')[i];
    const gridKeys = () => w.document.querySelectorAll('#kmGrid .km-key').length;
    const warn = w.document.getElementById('kmModWarn');

    btn(2).click();                                  // 0=All, 1=M0, 2=M1, 3=M2
    ck('selecting M1 shows its 24 keys', gridKeys() === 24);

    btn(3).click();
    ck('M2 shows 4 keys, not 6', gridKeys() === 4);  // 1 row x 4 cols
    ck('offline module warns',
       !warn.hidden && /not answering/.test(warn.textContent));

    btn(1).click();
    ck('online module does not warn', warn.hidden);

    btn(0).click();
    ck('All shows every populated position', gridKeys() === 4*6 + 4*6 + 4);
  } catch (e) {
    out.push('FAIL  threw: ' + e.message);
  }
  console.log(out.join('\n'));
  console.log(out.some(l => l.startsWith('FAIL')) ? '\nFAILURES' : '\nall green');
  w.close();
  process.exit(out.some(l => l.startsWith('FAIL')) ? 1 : 0);
}, 300);
