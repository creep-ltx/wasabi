// The desktop app's pages, driven like a person would, against the real
// Amiga: the overview fills in, files go PC -> Amiga -> PC (a folder
// too), a delete asks first, a screenshot appears in the gallery.
// Needs: wasabi desktop --no-browser --stay --port 8071, and /tmp/wdt/
// prepared (pc/hello.txt, pc/folder1/inner/deep.txt, empty back/).
import puppeteer from 'puppeteer-core';
import { execFileSync } from 'node:child_process';
import { readFileSync, existsSync } from 'node:fs';
const BASE = process.argv[2] || 'http://127.0.0.1:8071/';
const wasabi = (...a) => execFileSync('wasabi', a, { encoding: 'utf8' }).trim();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (name, ok, extra = '') => {
  console.log(`${ok ? '  ok  ' : '  FAIL'}  ${name}${extra ? ' - ' + extra : ''}`);
  if (!ok) failed++;
};
for (const f of ['RAM:hello.txt', 'RAM:folder1/inner/deep.txt', 'RAM:folder1/inner', 'RAM:folder1']) {
  try { execFileSync('wasabi', ['del', f], { stdio: 'ignore' }); } catch { /* not there */ }
}
const b = await puppeteer.launch({ browser: 'firefox', executablePath: '/usr/bin/firefox', headless: true });
try {
  const p = await b.newPage();
  const errors = [];
  p.on('pageerror', (e) => errors.push(String(e.message)));
  await p.setViewport({ width: 1440, height: 900 });
  const text = () => p.evaluate(() => document.body.innerText);
  const clickText = async (sel, t, within) => {
    const h = await p.waitForFunction((s, tt, w) => [...(w ? document.querySelector(w) : document)
      .querySelectorAll(s)].find((e) => e.textContent.trim() === tt), { timeout: 10000 }, sel, t, within || null);
    await h.asElement().click();
  };

  // Overview
  await p.goto(`${BASE}#overview`);
  await p.waitForFunction(() => document.body.innerText.includes('Kickstart'), { timeout: 20000 });
  const t = await text();
  check('overview shows the temperature', /Temperature\s+\d+\.\d/.test(t));
  check('overview shows versions', t.includes('Kickstart') && t.includes('wasabid'));
  check('overview shows the power status', t.includes('OK since the Pi started') || t.includes('Earlier') || t.includes('Now:'));
  check('overview shows disks', t.includes('Work') && t.includes('free of'));

  // Files: PC side to /tmp/wdt/pc, Amiga side to RAM:
  await p.goto(`${BASE}#files`);
  await sleep(1500);
  const pathBox = async (i, value) => {
    const boxes = await p.$$('input[type="text"], input:not([type])');
    await boxes[i].click({ count: 3 });
    await boxes[i].type(value);
    await boxes[i].press('Enter');
    await sleep(1200);
  };
  await pathBox(0, '/tmp/wdt/pc');
  await pathBox(1, 'RAM:');
  const tick = async (name) => p.click(`button[aria-label="Select ${name}"]`);
  await tick('hello.txt');
  await tick('folder1');
  await clickText('button', 'Copy');                       // the → one is first
  await sleep(3000);
  const ram = JSON.parse(wasabi('--json', 'ls', 'RAM:'));
  check('PC -> Amiga copies a file', ram.some((e) => e.name === 'hello.txt'));
  check('and a folder', ram.some((e) => e.name === 'folder1' && e.dir));
  check('with what is inside it', wasabi('get', 'RAM:folder1/inner/deep.txt', '-') === 'deep');

  // Amiga -> PC into /tmp/wdt/back
  await pathBox(0, '/tmp/wdt/back');
  await tick('hello.txt');
  await tick('folder1');
  const copies = await p.$$('button');
  for (const c of copies) {
    const tt = await c.evaluate((e) => e.textContent.trim());
    if (tt === 'Copy' && !(await c.evaluate((e) => e.disabled))) {
      const isLeft = await c.evaluate((e) => e.innerHTML.indexOf('svg') < e.innerHTML.indexOf('Copy'));
      if (isLeft) { await c.click(); break; }
    }
  }
  await sleep(3000);
  check('Amiga -> PC copies a file', existsSync('/tmp/wdt/back/hello.txt') &&
    readFileSync('/tmp/wdt/back/hello.txt', 'utf8') === 'hello from the PC\n');
  check('and a folder, inside too', existsSync('/tmp/wdt/back/folder1/inner/deep.txt'));

  // Delete on the Amiga asks first
  await tick('hello.txt');
  await tick('folder1');
  await p.click('button[aria-label="Delete"]');
  await sleep(500);
  check('delete asks before it deletes', (await text()).includes('cannot be undone'));
  await clickText('button', 'Yes, go ahead');
  await sleep(2500);
  const ram2 = JSON.parse(wasabi('--json', 'ls', 'RAM:'));
  check('and then deletes', !ram2.some((e) => e.name === 'hello.txt'));
  check('a folder too, with everything in it', !ram2.some((e) => e.name === 'folder1'));

  // Screenshots
  await p.goto(`${BASE}#shots`);
  await sleep(1500);
  const before = await p.$$eval('img', (i) => i.length);
  await clickText('button', 'Take screenshot');
  await p.waitForFunction((n) => document.querySelectorAll('img').length > n, { timeout: 15000 }, before);
  check('a screenshot appears in the gallery', true);
  check('no page errors', !errors.length, errors.join(' | '));
} finally {
  await b.close();
}
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
