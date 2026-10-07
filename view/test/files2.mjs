// Editing an Amiga file (with a backup and its protection bits kept),
// dropping a file onto the Amiga pane, and the mouse wheel in the view.
import puppeteer from 'puppeteer-core';
import { execFileSync } from 'node:child_process';
import { readdirSync, readFileSync } from 'node:fs';
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
  await p.goto(`${BASE}#files`);
  await sleep(1200);
  const boxes = await p.$$('input[type="text"], input:not([type])');
  await boxes[1].click({ count: 3 });
  await boxes[1].type('RAM:');
  await boxes[1].press('Enter');
  await sleep(1200);
  await p.click('button[aria-label="Select edit-test.txt"]');
  await p.click('button[aria-label="Edit"]');
  await p.waitForSelector('[role="dialog"] textarea');
  await sleep(800);
  const ta = await p.$('[role="dialog"] textarea');
  check('the editor shows the file', (await ta.evaluate((t) => t.value)) === 'line one\nline two\n');
  await ta.click();
  await p.keyboard.down('Control'); await p.keyboard.press('End'); await p.keyboard.up('Control');
  await p.keyboard.type('line three, with å\n');
  await clickText('[role="dialog"] button', 'Save');
  await sleep(1500);
  const now = wasabi('get', 'RAM:edit-test.txt', '-');
  check('Save writes the new text, å included', now === 'line one\nline two\nline three, with å', JSON.stringify(now));
  const list = wasabi('run', '--max-time', '10', 'List RAM:edit-test.txt');
  check('and keeps the script bit', /-s--rwed/.test(list), list.split('\n')[1]);
  const bdir = `${process.env.HOME}/.local/share/wasabi/backups/RAM_edit-test.txt`;
  const bk = readdirSync(bdir);
  check('and a backup of the old version is kept here', bk.length === 1 &&
    readFileSync(`${bdir}/${bk[0]}`, 'latin1') === 'line one\nline two\n');
  await p.keyboard.press('Escape');
  await sleep(500);

  // drop a file onto the Amiga pane
  await p.evaluate(() => {
    const dt = new DataTransfer();
    dt.items.add(new File(['dropped on the pane\n'], 'dropped.txt', { type: 'text/plain' }));
    const cards = [...document.querySelectorAll('.rt-Card')].filter((c) => c.textContent.includes('Amiga'));
    const target = cards[cards.length - 1];
    target.dispatchEvent(new DragEvent('dragover', { dataTransfer: dt, bubbles: true, cancelable: true }));
    target.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true }));
  });
  await sleep(2000);
  let dropped = '';
  try { dropped = wasabi('get', 'RAM:dropped.txt', '-'); } catch { /* */ }
  check('a file dropped on the Amiga pane lands in that drawer', dropped === 'dropped on the pane', JSON.stringify(dropped));

  // the wheel, in the live view
  wasabi('run', '--max-time', '10', 'Run >NIL: SYS:Utilities/MultiView SYS:S/Startup-Sequence');
  wasabi('wait', 'window', 'Startup', '--timeout', '15');
  await p.goto(`${BASE}#screen`);
  await p.waitForFunction(() => document.querySelector('canvas')?.width === 1280, { timeout: 15000 });
  await sleep(1500);
  wasabi('grab', '--window', 'SYS:S/Startup', '/tmp/wheel0.png');
  const r = await p.evaluate(() => { const c = document.querySelector('canvas').getBoundingClientRect(); return [c.left + c.width / 2, c.top + c.height / 2]; });
  await p.mouse.move(r[0], r[1]);
  for (let i = 0; i < 3; i++) { await p.mouse.wheel({ deltaY: 100 }); await sleep(150); }
  await sleep(800);
  let moved = false;
  try { wasabi('grab', '--window', 'SYS:S/Startup', '/tmp/wheel1.png', '--diff', '/tmp/wheel0.png'); } catch { moved = true; }
  check('the mouse wheel scrolls the Amiga window under it', moved);
  check('no page errors', !errors.length, errors.join(' | '));
} finally {
  await b.close();
  for (const f of ['RAM:edit-test.txt', 'RAM:dropped.txt']) { try { execFileSync('wasabi', ['del', f], { stdio: 'ignore' }); } catch { /* */ } }
  try { execFileSync('wasabi', ['kill', 'MultiView'], { stdio: 'ignore' }); } catch { /* */ }
}
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
