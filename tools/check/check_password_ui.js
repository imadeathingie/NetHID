/*
 * check_password_ui.js — the change-password panel, under jsdom.
 *
 * This panel can lock someone out of their own keyboard, and the only way back
 * in is a reflash. So the things asserted here are the ones whose failure is
 * expensive rather than merely annoying:
 *
 *   - it hides itself when the firmware has no password store, instead of
 *     offering a control that 404s;
 *   - a mistyped confirmation never reaches the wire, so you cannot set a
 *     password you did not mean to type twice;
 *   - the device's specific rejection is shown, not a generic failure — "that
 *     is not the current password" and "too short" send you to different places;
 *   - nothing ever displays a password the device sent back, because the device
 *     never sends one.
 *
 * Needs jsdom:  npm i jsdom@24
 *
 *     ./tools/check/check_password_ui.sh        # skips cleanly without jsdom
 */
const { JSDOM } = require('jsdom');
const fs = require('fs');

const html = fs.readFileSync(process.argv[2], 'utf8');

let fails = 0;
function ok(cond, what) {
  console.log((cond ? 'PASS  ' : 'FAIL  ') + what);
  if (!cond) fails++;
}

const DEFAULTS = {
  ok: true, user: 'admin', multiuser: false, min_len: 8, max_len: 63,
  changed: false, stored: false, saving: false, build_id: 'deadbeef',
  secure: false, can_change: true, why: '',
};

// `info` false means the endpoint is absent (a firmware without the store, or
// AP setup mode, where it is deliberately refused).
async function run(label, info, body) {
  const posts = [];
  const state = { info: info === false ? { error: 'not_found' }
                                       : Object.assign({}, DEFAULTS, info) };
  const dom = new JSDOM(html, {
    runScripts: 'dangerously', pretendToBeVisual: true,
    beforeParse(w) {
      w.fetch = (url, init) => {
        if (init && init.method === 'POST') {
          const b = JSON.parse(init.body);
          posts.push({ url, body: b });
          // Mirror src/web.cpp's validation exactly. A test that accepts what
          // the firmware rejects proves nothing about the firmware.
          if (url === '/api/password' || url === '/api/password/reset') {
            if (b.current !== 'changeme')
              return json({ error: 'wrong_password' });
            if (url === '/api/password/reset') return json({ ok: true, reset: true });
            if (!b.new || b.new.length < 8 || b.new.length > 63)
              return json({ error: 'bad_length', min_len: 8, max_len: 63 });
            if (b.new === b.current) return json({ error: 'unchanged' });
            state.info.changed = true;
            return json({ ok: true, queued: true, sessions_ended: 2 });
          }
          return json({ ok: true });
        }
        if (url === '/api/password') return json(state.info);
        if (url === '/api/keymap/info') return json({ error: 'not_found' });
        return json({ ok: true });
      };
      function json(o) {
        return Promise.resolve({ ok: true, status: 200, json: async () => o });
      }
      w.alert = () => {}; w.confirm = () => true;
      w.matchMedia = () => ({ matches: false, addEventListener() {}, removeEventListener() {} });
    },
  });
  const w = dom.window;
  await new Promise(r => w.addEventListener('load', r));
  await w.pwLoad();
  await new Promise(r => setTimeout(r, 20));

  console.log('\n── ' + label);
  const $ = id => w.document.getElementById(id);
  const type = (cur, neu, neu2) => {
    $('pwCur').value = cur; $('pwNew').value = neu === undefined ? '' : neu;
    $('pwNew2').value = neu2 === undefined ? neu : neu2;
  };
  await body({ w, $, posts, type, msg: () => $('pwMsg').textContent, state });
  w.close();
}

(async () => {
  await run('firmware without the password store', false, ({ $ }) => {
    ok($('pwWrap').hidden, 'panel hidden when /api/password does not answer');
  });

  await run('AP setup mode refuses it', { ok: false, error: 'setup_mode' },
    ({ $ }) => {
      ok($('pwWrap').hidden, 'panel hidden when setup mode forbids it');
    });

  await run('default state', {}, ({ $ }) => {
    ok(!$('pwWrap').hidden, 'panel shown');
    ok(/firmware/i.test($('pwState').textContent),
       'says the password is the compiled one');
    ok($('pwReset').disabled, 'revert disabled when nothing was changed');
    // The whole reason this feature is safe to offer. If the hint stops saying
    // it, the next person to forget their password has no idea what to do.
    ok(/flashing firmware clears it/i.test($('pwHint').textContent),
       'hint names the reflash recovery path');
    ok(/plain http/i.test($('pwHint').textContent),
       'hint warns that a new password crosses an unencrypted link');
  });

  // ALLOW_PLAINTEXT_AUTH=0 without HTTPS. The device refuses, and the page has
  // to say so where someone will read it — an inert form with no explanation is
  // the same dead end as a keycode for a compiled-out feature.
  await run('plaintext auth forbidden',
    { can_change: false, why: 'built with ALLOW_PLAINTEXT_AUTH=0, needs ENABLE_HTTPS' },
    async ({ $, posts, type, w }) => {
      ok(!$('pwWrap').hidden, 'panel still shown, not hidden');
      ok($('pwNew').disabled && $('pwSave').disabled, 'the form is disabled');
      ok(/ALLOW_PLAINTEXT_AUTH/.test($('pwHint').textContent),
         'the hint names the setting responsible');
      type('changeme', 'longenough1');
      await w.pwChange();
      ok(posts.length === 0, 'no password is sent even if pwChange is called');
    });

  await run('already changed on the device', { changed: true, stored: true },
    ({ $ }) => {
      ok(/changed on this device/i.test($('pwState').textContent),
         'says the password was changed here');
      ok(!$('pwReset').disabled, 'revert offered once a password is stored');
    });

  await run('mismatched confirmation', {}, async ({ $, posts, type, msg, w }) => {
    type('changeme', 'longenough1', 'longenough2');
    await w.pwChange();
    ok(posts.length === 0, 'nothing is sent when the two new fields differ');
    ok(/do not match/i.test(msg()), 'the message names the mismatch');
  });

  await run('too short', {}, async ({ posts, type, msg, w }) => {
    type('changeme', 'short');
    await w.pwChange();
    ok(posts.length === 0, 'a too-short password never reaches the wire');
    ok(/8/.test(msg()), 'the message names the minimum');
  });

  await run('same as current', {}, async ({ posts, type, msg, w }) => {
    type('changeme', 'changeme');
    await w.pwChange();
    ok(posts.length === 0, 'setting the same password is refused locally');
    ok(/already the current/i.test(msg()), 'the message says so');
  });

  await run('wrong current password', {}, async ({ posts, type, msg, w }) => {
    type('notit', 'longenough1');
    await w.pwChange();
    ok(posts.length === 1, 'the attempt is sent — only the device can judge it');
    ok(/not the current password/i.test(msg()),
       'the device rejection is shown, not a generic failure');
  });

  await run('lockout is reported', { }, async ({ w, $, type, msg }) => {
    // Swap in a firmware that is locked out, to prove the retry time surfaces:
    // "rejected" with no number reads as a bug in the page.
    w.fetch = (url, init) => Promise.resolve({
      ok: true, status: 429,
      json: async () => ({ error: 'locked', retry_after_s: 27 }),
    });
    type('changeme', 'longenough1');
    await w.pwChange();
    ok(/27/.test(msg()) && /locked/i.test(msg()),
       'the lockout and its remaining time are shown');
  });

  await run('successful change', {}, async ({ $, posts, type, msg, w }) => {
    type('changeme', 'longenough1');
    await w.pwChange();
    ok(posts.length === 1 && posts[0].url === '/api/password',
       'the change posts to /api/password');
    ok(posts[0].body.current === 'changeme' && posts[0].body.new === 'longenough1',
       'it sends the current and the new password');
    ok(/changed/i.test(msg()), 'success is reported');
    ok(/2 other session/i.test(msg()),
       'it says how many other sessions were signed out');
    // Leaving a password sitting in a form field on a shared screen is exactly
    // the kind of small carelessness this panel should not have.
    ok($('pwCur').value === '' && $('pwNew').value === '' && $('pwNew2').value === '',
       'the fields are cleared afterwards');
    ok(/changed on this device/i.test($('pwState').textContent),
       'the state line refreshes without a page reload');
  });

  await run('revert needs the current password',
    { changed: true, stored: true }, async ({ posts, type, msg, w }) => {
      type('', '');
      await w.pwRevert();
      ok(posts.length === 0, 'revert without the current password is not sent');
      ok(/current password/i.test(msg()), 'it says what is missing');
    });

  // Closing the panel mid-edit and reopening it must not leave the password
  // sitting in the field — on a screen someone else may be looking at, and with
  // a stale error message next to it.
  await run('reopening the panel', {}, async ({ $, type, msg, w }) => {
    type('changeme', 'short');
    await w.pwChange();                       // fails validation, leaves a message
    ok(msg() !== '', 'the failed attempt left a message');
    w.closeSettings();
    w.openSettings();
    await new Promise(r => setTimeout(r, 20));
    ok($('pwCur').value === '' && $('pwNew').value === '' && $('pwNew2').value === '',
       'reopening clears the fields');
    ok(msg() === '', 'reopening clears the stale message');
    ok(!$('pwWrap').hidden, 'and the panel is still there');
  });

  await run('revert', { changed: true, stored: true },
    async ({ posts, type, msg, w }) => {
      type('changeme', '');
      await w.pwRevert();
      ok(posts.length === 1 && posts[0].url === '/api/password/reset',
         'revert posts to /api/password/reset');
      ok(/firmware password/i.test(msg()), 'success names what is now in force');
    });

  console.log(fails ? '\n' + fails + ' FAILED' : '\nall green');
  process.exit(fails ? 1 : 0);
})();
