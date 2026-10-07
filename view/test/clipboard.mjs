// The Clipboard dialog against the real Amiga: it shows the Amiga's
// clipboard, and "...and paste it" pastes into the active window.
import puppeteer from 'puppeteer-core';
import { execFileSync } from 'node:child_process';
const BASE = process.argv[2] || 'http://127.0.0.1:8071/';
const wasabi = (...a) => execFileSync('wasabi', a, { encoding: 'latin1' }).trim();
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
  await p.setViewport({ width: 1440, height: 900 });
  const clickText = async (sel, t) => {
    const h = await p.waitForFunction((s, tt) => [...document.querySelectorAll(s)]
      .find((e) => e.textContent.trim() === tt && e.offsetParent !== null), { timeout: 10000 }, sel, t);
    await h.asElement().click();
  };
  execFileSync('wasabi', ['clip', 'set', 'from the amiga side'], { stdio: 'ignore' });
  wasabi('run', '--max-time', '10', 'NewShell "CON:120/120/560/160/ClipApp"');
  wasabi('wait', 'window', 'ClipApp', '--timeout', '15');
  wasabi('mouse', 'click', '300', '200');
  await p.goto(`${BASE}#screen`);
  await p.waitForFunction(() => document.querySelector('canvas')?.width === 1280, { timeout: 15000 });
  await clickText('button', 'Clipboard');
  await p.waitForFunction(() => [...document.querySelectorAll('[role="dialog"] textarea')]
    .some((t) => t.value === 'from the amiga side'), { timeout: 10000 }).catch(() => {});
  const shown = await p.evaluate(() => document.querySelector('[role="dialog"] textarea')?.value);
  check('the dialog shows the Amiga\'s clipboard', shown === 'from the amiga side', JSON.stringify(shown));
  const areas = await p.$$('[role="dialog"] textarea');
  await areas[1].type('echo pasted by the app >RAM:clipapp\n');
  await clickText('[role="dialog"] button', '…and paste it (Right Amiga+V)');
  await sleep(1500);
  let out = '';
  try { out = wasabi('get', 'RAM:clipapp', '-'); } catch { /* */ }
  check('"...and paste it" pastes into the active window', out === 'pasted by the app', JSON.stringify(out));
  await p.screenshot({ path: '/tmp/clip-dialog.png' });
  check('no page errors', !errors.length, errors.join(' | '));
} finally {
  await b.close();
  try { execFileSync('wasabi', ['del', 'RAM:clipapp'], { stdio: 'ignore' }); } catch { /* */ }
  try { execFileSync('wasabi', ['key', 'type', 'endcli', '--enter'], { stdio: 'ignore' }); } catch { /* */ }
}
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
