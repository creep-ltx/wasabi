// The Developer page's Slots tab: what Wasabi is running, and Stop.
// Needs: wasabi desktop --no-browser --stay --port 8071, against an Amiga
// (or the mock) with wasabid 0.4+. Starts its own commands in the slots
// with the wasabi command (WASABI_* in the environment pick the machine).
import puppeteer from 'puppeteer-core';
import { spawn } from 'node:child_process';
const BASE = process.argv[2] || 'http://127.0.0.1:8071/';
const WASABI = process.env.WASABI_BIN || 'wasabi';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (name, ok, extra = '') => {
  console.log(`${ok ? '  ok  ' : '  FAIL'}  ${name}${extra ? ' - ' + extra : ''}`);
  if (!ok) failed++;
};
const runs = [spawn(WASABI, ['run', 'sleep 30'], { stdio: 'ignore' }),
  spawn(WASABI, ['run', 'sleep 31 # stubborn'], { stdio: 'ignore' })];
const b = await puppeteer.launch({ browser: 'firefox', executablePath: '/usr/bin/firefox', headless: true });
try {
  const p = await b.newPage();
  const errors = [];
  p.on('pageerror', (e) => errors.push(String(e.message)));
  await p.setViewport({ width: 1440, height: 900 });
  const text = () => p.evaluate(() => document.body.innerText);
  await p.goto(`${BASE}#dev`);
  const tab = await p.waitForFunction(() => [...document.querySelectorAll('[role="tab"]')]
    .find((e) => e.textContent.trim().startsWith('Slots')), { timeout: 10000 });
  await tab.asElement().click();
  await p.waitForFunction(() => document.body.innerText.includes('slots in use'), { timeout: 10000 });
  await sleep(4000);
  const t1 = await text();
  check('two running commands are listed', t1.includes('2 of 4 slots in use') &&
    t1.includes('sleep 30') && t1.includes('sleep 31'), t1.match(/\d of 4 slots in use/)?.[0]);
  await p.screenshot({ path: '/tmp/dev-slots.png' });
  await p.setViewport({ width: 390, height: 800 });
  await sleep(500);
  check('fits a phone screen', await p.evaluate(() =>
    document.documentElement.scrollWidth <= window.innerWidth + 1));
  await p.screenshot({ path: '/tmp/dev-slots-phone.png' });
  await p.setViewport({ width: 1440, height: 900 });

  const stop = async (cmd) => {
    const h = await p.waitForFunction((c) => [...document.querySelectorAll('tr')]
      .find((r) => r.innerText.includes(c))?.querySelector('button'), { timeout: 5000 }, cmd);
    await h.asElement().click();
  };
  await stop('sleep 30');
  await p.waitForFunction(() => document.body.innerText.includes('the command stopped'), { timeout: 10000 });
  check('Stop ends a command that listens', true);
  await stop('sleep 31');
  await p.waitForFunction(() => document.body.innerText.includes('is free again'), { timeout: 15000 });
  await sleep(3500);
  const t2 = await text();
  check('a command that ignores Ctrl-C is let go of and shown as stuck',
    t2.includes('0 of 4 slots in use') && t2.includes('stuck'));
  await p.screenshot({ path: '/tmp/dev-slots-stuck.png' });
  check('no page errors', errors.length === 0, errors.join('; '));
} finally {
  await b.close();
  for (const r of runs) r.kill();
}
process.exit(failed ? 1 : 0);
