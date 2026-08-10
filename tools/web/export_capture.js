/*
 * export_capture.js — drive the web editor's Export button under jsdom and write
 * what it produces.
 *
 * Used by tools/check/check_export.sh. Captures the real Blob contents rather than
 * reimplementing the export, so what is checked is what a user downloads. C
 * generation is deliberately NOT here: it needs the board's LAYOUT() macro,
 * which the browser has no access to, so it lives in tools/keyboard/json_to_keymap.py.
 *
 *     node tools/web/export_capture.js <board> <outdir>
 */
// Drive the page's own Export buttons and capture what they produce, so the
// generated C is the real thing rather than a reimplementation.
const { JSDOM } = require('jsdom');
const fs = require('fs');
const dom = new JSDOM(fs.readFileSync('preview.html','utf8'),
  { runScripts:'dangerously', pretendToBeVisual:true,
    beforeParse(w){ w.console.error = () => {}; } });
const w = dom.window;
const captured = {};
w.URL.createObjectURL = (blob) => { captured._blob = blob; return 'blob:x'; };
w.HTMLAnchorElement.prototype.click = function () {
  captured[this.download] = captured._blob._text;
};
// jsdom Blobs do not expose their text synchronously; stash it on construction.
const RealBlob = w.Blob;
w.Blob = function (parts, opts) { const b = new RealBlob(parts, opts); b._text = parts.join(''); return b; };

const out = process.argv[3] || '/tmp';
setTimeout(async () => {
  await w.kmLoad(true);
  w.mock.board(process.argv[2] || 'modular');
  await new Promise(r => setTimeout(r, 120));
  w.kmExport();
  for (const k of Object.keys(captured)) {
    if (k === '_blob') continue;
    const name = 'keymap.json';
    fs.writeFileSync(out + '/' + name, captured[k]);
    console.log('wrote', name, captured[k].length, 'bytes');
  }
  w.close(); process.exit(0);
}, 400);
