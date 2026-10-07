// Wasabi phone: the server (wasabi serve) in a phone-sized window, with
// the login, the bottom bar, typing text from the key bar and a tap.
// Needs: wasabi serve --port 8078 running with the password below set
// (the test sets it on a fresh server), and the real Amiga.
import puppeteer from 'puppeteer-core';
import { execFileSync } from 'node:child_process';
const BASE = process.argv[2] || 'http://127.0.0.1:8078/';
const PW = process.argv[3] || 'testpass-123';
const wasabi = (...a) => execFileSync('wasabi', a, { encoding: 'utf8' }).trim();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (name, ok, extra = '') => {
  console.log(`${ok ? '  ok  ' : '  FAIL'}  ${name}${extra ? ' - ' + extra : ''}`);
  if (!ok) failed++;
};
const b = await puppeteer.launch({ browser: 'firefox', executablePath: '/usr/bin/firefox', headless: true });
try {
  const p = await b.newPage();
  const errors = [];
  p.on('pageerror', (e) => errors.push(String(e.message)));
  await p.setViewport({ width: 390, height: 844, deviceScaleFactor: 1, hasTouch: true, isMobile: true });
  const text = () => p.evaluate(() => document.body.innerText);
  const clickText = async (sel, t) => {
    const h = await p.waitForFunction((s, tt) => [...document.querySelectorAll(s)]
      .find((e) => e.textContent.trim() === tt && e.offsetParent !== null), { timeout: 10000 }, sel, t);
    await h.asElement().click();
  };

  await p.goto(BASE);
  await p.waitForSelector('input[type="password"]', { timeout: 10000 });
  const setup = (await text()).includes('Choose a password');
  await p.screenshot({ path: '/tmp/phone-login.png' });
  const boxes = await p.$$('input[type="password"]');
  await boxes[0].type(PW);
  if (setup) await boxes[1].type(PW);
  await clickText('button', setup ? 'Set the password' : 'Log in');
  await p.waitForFunction(() => document.body.innerText.includes('Temperature'), { timeout: 20000 });
  check('the login lets us in', true);
  await sleep(6000);
  await p.screenshot({ path: '/tmp/phone-overview.png', fullPage: false });
  check('the bottom bar is there', await p.$eval('.wv-bottom-nav', (e) => e.offsetParent !== null || getComputedStyle(e).display !== 'none'));
  check('the sidebar is not', await p.$eval('.wv-sidebar', (e) => getComputedStyle(e).display === 'none'));

  // Files: the NAS pane is the server's folder
  await clickText('button', 'Files');
  await sleep(1500);
  await p.screenshot({ path: '/tmp/phone-files.png' });
  check('Files offers Amiga and NAS', (await text()).includes('NAS'));

  // Screen: tap into a Shell, type text from the key bar
  wasabi('run', '--max-time', '10', 'NewShell "CON:120/120/560/160/PhoneTest"');
  wasabi('wait', 'window', 'PhoneTest', '--timeout', '15');
  await clickText('button', 'Screen');
  await p.waitForFunction(() => document.querySelector('canvas')?.width === 1280, { timeout: 15000 });
  await sleep(1500);
  const r = await p.evaluate(() => { const c = document.querySelector('canvas').getBoundingClientRect(); return [c.left, c.top, c.width, c.height]; });
  await p.touchscreen.tap(r[0] + (900 * r[2]) / 1280, r[1] + (800 * r[3]) / 960);  // backdrop
  await sleep(700);
  await p.touchscreen.tap(r[0] + (300 * r[2]) / 1280, r[1] + (200 * r[3]) / 960);  // the Shell
  await sleep(700);
  check('a tap activates the window it lands on', /PhoneTest.*\[active\]/.test(wasabi('windows')));
  await p.screenshot({ path: '/tmp/phone-screen.png' });
  await p.click('button[aria-label="Type on the Amiga"]');
  await sleep(500);
  await p.keyboard.type('echo från telefonen >RAM:phone-test');
  await clickText('[role="dialog"] button', 'Send');
  await sleep(500);
  await clickText('[role="dialog"] button', 'Return');
  await sleep(1200);
  let out = '';
  // the Amiga writes Latin-1: read it as that, or å turns into junk here
  try { out = execFileSync('wasabi', ['get', 'RAM:phone-test', '-'], { encoding: 'latin1' }).trim(); } catch { /* */ }
  check('text from the phone reaches the Amiga, å and > included', out === 'från telefonen', JSON.stringify(out));
  await p.keyboard.press('Escape');
  await sleep(300);
  try { wasabi('del', 'RAM:phone-test'); } catch { /* */ }
  wasabi('key', 'type', 'endcli', '--enter');
  check('no page errors', !errors.length, errors.join(' | '));
} finally {
  await b.close();
}
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
