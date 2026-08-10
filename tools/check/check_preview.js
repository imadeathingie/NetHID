/*
 * check_preview.js — the generated preview actually runs, with no board.
 *
 * The preview is worth exactly as much as its fidelity: a mock whose response
 * shapes have drifted from the firmware shows you a UI that cannot exist. These
 * assertions are the cheap half of keeping it honest — that the page loads, the
 * fixtures differ, and mock.board()/mock.online() reach the real render code
 * rather than a parallel implementation.
 *
 *     node tools/check/check_preview.js preview.html
 */
const { JSDOM } = require('jsdom');
const fs = require('fs');

const dom = new JSDOM(fs.readFileSync(process.argv[2], 'utf8'), {
  runScripts: 'dangerously',
  pretendToBeVisual: true,
  beforeParse(w) { w.console.error = () => {}; },
});
const w = dom.window;

setTimeout(async () => {
  const out = [];
  const ck = (n, ok) => out.push((ok ? 'PASS  ' : 'FAIL  ') + n);
  const tabs = () => w.document.querySelectorAll('#tabBar .tab').length;
  // Count keys in whichever view is showing. The fixtures carry geometry, so
  // the editor correctly defaults to the physical layout and #kmGrid is empty —
  // counting only that container asserts against a view nobody is looking at.
  const visibleKeys = () =>
    w.document.querySelectorAll('#kmPhys .km-key, #kmGrid .km-key').length;
  const modBtns = () => w.document.querySelectorAll('#kmModBtns .km-mbtn').length;

  try {
    ck('mock installed', typeof w.mock === 'object');
    ck('page built its tab bar', tabs() > 0);

    await w.kmLoad(true);
    ck('default fixture is the modular board',
       w.mock.board() === 'modular' && modBtns() === 4);
    // Derived from the fixture, not hard-coded: an expectation typed by hand
    // is one more thing to drift when a fixture changes, and it drifted the
    // first time this was written.
    ck('every placed key is rendered',
       visibleKeys() === w.mock.state.fx.geometry.length);
    ck('layout view is the default when geometry exists',
       !w.document.getElementById('kmPhys').hidden);

    // Switching fixtures must go through the page's own loader.
    w.mock.board('oledpad');
    await new Promise(r => setTimeout(r, 60));
    ck('switching to oledpad reshapes the board',
       visibleKeys() === 12 && w.mock.state.fx.geometry.length === 12);
    ck('single-board fixture hides the module row',
       w.document.getElementById('kmModBtns').hidden);

    w.mock.board('modular');
    await new Promise(r => setTimeout(r, 60));
    w.mock.online(2, false);
    await new Promise(r => setTimeout(r, 60));
    const warn = w.document.getElementById('kmModWarn');
    ck('taking a module offline surfaces a warning',
       !warn.hidden && /not answering/.test(warn.textContent));

    // The settings tab renders from the mock's field table.
    await w.stLoad();
    ck('settings render from the mock',
       w.document.querySelectorAll('#stList .st-row').length === 8);

    await w.wfLoad();
    ck('wifi list renders from the mock',
       w.document.querySelectorAll('#wfList .wf-net').length === 2);

    // ── Encoders ────────────────────────────────────────────────────────────
    // They are not matrix positions, so nothing in the grid or layout view
    // would ever reveal a bug here.
    const encCards = () => w.document.querySelectorAll('#kmEncs .km-enc').length;
    const encKeys  = () => w.document.querySelectorAll('#kmEncs .km-key').length;

    ck('modular board shows two encoder cards', encCards() === 2);
    ck('each card exposes CCW, press and CW', encKeys() === 6);
    ck('encoder cards name their module',
       /M0/.test(w.document.querySelector('#kmEncs .km-enctitle').textContent));

    // Editing one action must reach the encoder endpoint, not the keymap one.
    const before = w.mock.log().length;
    w.document.querySelectorAll('#kmEncs .km-key')[0].click();
    ck('clicking an encoder action opens the picker',
       w.document.getElementById('kmPicker').classList.contains('on'));
    await w.kmApply(0x5A01);                       // KC_VOLU
    await new Promise(r => setTimeout(r, 40));
    const posted = w.mock.log().slice(before)
                    .filter(l => l.indexOf('POST /api/keymap/encoders') === 0);
    ck('the edit posts to /api/keymap/encoders', posted.length === 1);
    ck('the edit is reflected in the mock state',
       w.mock.state.enc[0][0][0] === 0x5A01);

    w.mock.board('mystery6x6');
    await new Promise(r => setTimeout(r, 60));
    ck('a board with no encoders hides the section',
       w.document.getElementById('kmEncWrap').hidden);

    w.mock.board('modular');
    await new Promise(r => setTimeout(r, 60));

    // ── Board switching must not leave the previous board behind ────────────
    // Every one of these caught a real bug: containers were hidden rather than
    // cleared, so a board with no modules kept the last board's chips and a
    // board with no encoders kept its cards.
    const mods = () => w.document.querySelectorAll('#kmModBtns .km-mbtn').length;
    const encs = () => w.document.querySelectorAll('#kmEncs .km-enc').length;

    w.mock.board('modular');
    await new Promise(r => setTimeout(r, 80));
    ck('modular lists 3 modules plus All', mods() === 4);
    ck('modular shows 2 encoders', encs() === 2);

    // Select a module, then switch: a selection naming a module that no longer
    // exists filters every row out and leaves an empty editor.
    w.document.querySelectorAll('#kmModBtns .km-mbtn')[3].click();
    w.mock.board('oledpad');
    await new Promise(r => setTimeout(r, 80));
    ck('single-board fixture leaves no module chips behind', mods() === 0);
    ck('oledpad shows its 1 encoder', encs() === 1);
    ck('oledpad still renders its keys', visibleKeys() === 12);

    w.mock.board('mystery6x6');
    await new Promise(r => setTimeout(r, 80));
    ck('a board with no encoders leaves no cards behind', encs() === 0);
    ck('mystery6x6 renders its keys', visibleKeys() === 32);

    w.mock.board('modular');
    await new Promise(r => setTimeout(r, 80));
    ck('modules come back on return', mods() === 4 && encs() === 2);
    ck('returning resets the module selection',
       visibleKeys() === w.mock.state.fx.geometry.length);

    ck('requests were logged', w.mock.log().length > 5);

    // ── Autoclick editor ────────────────────────────────────────────────────
    // The panel hides itself when the board reports no slots, which is also
    // exactly what it did when the mock had no /api/autoclick route at all — so
    // "not visible in the preview" looked like a UI bug for as long as it took
    // to notice the mock was the thing missing the endpoint.
    const acHidden = () => w.document.getElementById('acWrap').hidden;
    const acChips  = () => w.document.querySelectorAll('#acSlots .mb-slot').length;

    for (const [board, slots] of [['mystery6x6', 3], ['oledpad', 3],
                                  ['proto2x2', 0], ['modular', 0]]) {
      w.mock.board(board);
      await new Promise(r => setTimeout(r, 80));
      ck(board + ': autoclick panel ' + (slots ? 'shown' : 'hidden'),
         acHidden() === (slots === 0));
      ck(board + ': ' + slots + ' slot chip(s)', acChips() === slots);
      // The picker's slot range comes from the same number. A board with three
      // slots offering AUTO0-15 is the symptom of a missing `features` object.
      ck(board + ': picker offers ' + slots + ' autoclick slot(s)',
         w.kmAutoclickSlots() === slots);
    }

    // Feature gating drives whole groups, not just autoclick.
    w.mock.board('proto2x2');
    await new Promise(r => setTimeout(r, 60));
    w.kmBuildList();
    const groups = () => [...w.document.querySelectorAll('#kmList .km-glabel')]
                           .map(e => e.textContent);
    ck('a board without KB_FEATURE_CONSUMER offers no MEDIA group',
       !groups().includes('MEDIA'));
    w.mock.board('mystery6x6');
    await new Promise(r => setTimeout(r, 60));
    w.kmBuildList();
    ck('a board with KB_FEATURE_CONSUMER does offer it',
       groups().includes('MEDIA'));

    // ── Settings overlay ────────────────────────────────────────────────────
    // Opened explicitly, because nothing else on the page touches it — so
    // /api/password was never requested during this check and the fidelity
    // assertion below could not have noticed the mock lacking that route.
    w.openSettings();
    await new Promise(r => setTimeout(r, 60));
    ck('the password panel renders against the mock',
       !w.document.getElementById('pwWrap').hidden);
    ck('opening the panel leaves no password in the fields',
       w.document.getElementById('pwCur').value === '');

    // ── Mock fidelity ───────────────────────────────────────────────────────
    // The generic form of the bug above: the mock answers {ok:true} to anything
    // it does not model, so a new endpoint silently previews as a feature that
    // does not exist. Every request the page makes must be modelled.
    const un = w.mock.unhandled();
    if (un.length) out.push('      unmodelled: ' + [...new Set(un)].join(', '));
    ck('every endpoint the page calls is modelled by the mock', un.length === 0);
  } catch (e) {
    out.push('FAIL  threw: ' + e.message);
  }

  console.log(out.join('\n'));
  const bad = out.some(l => l.startsWith('FAIL'));
  console.log(bad ? '\nFAILURES' : '\nall green');
  w.close();
  process.exit(bad ? 1 : 0);
}, 400);
