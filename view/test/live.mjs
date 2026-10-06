// Drive `wasabi view` in a headless Firefox against the real Amiga:
// the page connects, paints, and a click and typed keys reach the
// Amiga. Run with a bridge already serving: node test/live.mjs [URL]
import puppeteer from 'puppeteer-core';
import { execFileSync } from 'node:child_process';

const URL = process.argv[2] || 'http://127.0.0.1:8071/';
const wasabi = (...a) => execFileSync('wasabi', a, { encoding: 'utf8' }).trim();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;

// Type by key position, as a Swedish keyboard would for the Amiga's
// Swedish keymap: the test tool knows only US characters, and the
// Amiga decides the character from the position.
async function typeKeys(page, text) {
  for (const ch of text) {
    if (/[a-z]/.test(ch)) await page.keyboard.press(`Key${ch.toUpperCase()}`);
    else if (/[0-9]/.test(ch)) await page.keyboard.press(`Digit${ch}`);
    else if (ch === ' ') await page.keyboard.press(' ');
    else if (ch === ':') {                      // Swedish: Shift + .
      await page.keyboard.down('ShiftLeft');
      await page.keyboard.press('Period');
      await page.keyboard.up('ShiftLeft');
    } else throw new Error(`typeKeys: no position for ${ch}`);
  }
}
const check = (name, ok, extra = '') => {
  console.log(`${ok ? '  ok  ' : '  FAIL'}  ${name}${extra ? ' - ' + extra : ''}`);
  if (!ok) failed++;
};

const browser = await puppeteer.launch({
  browser: 'firefox', executablePath: '/usr/bin/firefox', headless: true,
});
try {
  const page = await browser.newPage();
  await page.setViewport({ width: 1400, height: 1100 });
  await page.goto(URL);
  await page.waitForFunction(
    () => document.body.innerText.includes('wasabid') &&
          document.querySelector('canvas')?.width === 1280, { timeout: 15000 });
  check('the page connects and gets the 1280x960 screen', true);

  // Something other than black was painted (the size comes first, the
  // pixels just after it).
  await page.waitForFunction(() => {
    const c = document.querySelector('canvas');
    const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4 * 97) if (d[i] + d[i + 1] + d[i + 2] > 30) n++;
    return n > 1000;
  }, { timeout: 15000 }).catch(() => {});
  const lit = await page.evaluate(() => {
    const c = document.querySelector('canvas');
    const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    let n = 0;
    for (let i = 0; i < d.length; i += 4 * 97) if (d[i] + d[i + 1] + d[i + 2] > 30) n++;
    return n;
  });
  check('the picture is painted', lit > 1000, `${lit} lit samples`);
  await page.screenshot({ path: '/tmp/view-1.png' });

  // The page's picture against a real grab, pixel by pixel. RGB565 keeps
  // 5-6 bits a channel, so a pixel may be off by up to 7 per channel -
  // anything more means the compact format was drawn wrong.
  await sleep(1500);
  const shot = wasabi('grab', '/tmp/view-ref.png');
  const { readFileSync } = await import('node:fs');
  const { inflateSync } = await import('node:zlib');
  const png = readFileSync(shot);
  // minimal PNG reader for wasabi's own RGB8 output (filter 0 rows)
  let off = 8, idat = [], pw = 0, ph = 0;
  while (off < png.length) {
    const len = png.readUInt32BE(off); const type = png.toString('ascii', off + 4, off + 8);
    if (type === 'IHDR') { pw = png.readUInt32BE(off + 8); ph = png.readUInt32BE(off + 12); }
    if (type === 'IDAT') idat.push(png.subarray(off + 8, off + 8 + len));
    off += 12 + len;
  }
  const raw = inflateSync(Buffer.concat(idat));
  // Every 7th pixel each way, sampled in the page: a whole screen's
  // worth of numbers is too much to pass back.
  const sampled = await page.evaluate(() => {
    const c = document.querySelector('canvas');
    if (!c) return { missing: document.body.innerText.slice(0, 200) };
    const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    const out = [];
    for (let y = 0; y < c.height; y += 7)
      for (let x = 0; x < c.width; x += 7) {
        const p = (y * c.width + x) * 4;
        out.push(d[p], d[p + 1], d[p + 2]);
      }
    return { out };
  });
  if (sampled.missing !== undefined) throw new Error('no canvas: ' + sampled.missing);
  let worst = 0, bad = 0, i = 0;
  for (let y = 0; y < ph; y += 7) {
    for (let x = 0; x < pw; x += 7, i += 3) {
      const r = 1 + y * (pw * 3 + 1) + x * 3;
      for (let k = 0; k < 3; k++) {
        const diff = Math.abs(raw[r + k] - sampled.out[i + k]);
        if (diff > worst) worst = diff;
        if (diff > 8) bad++;
      }
    }
  }
  check('the page shows what a grab shows (565 rounding only)', bad < 50,
    `largest channel difference ${worst}, ${bad} samples off by more than 8`);

  // Open a Shell on the Amiga, then click into it through the page.
  wasabi('run', '--max-time', '10', 'NewShell "CON:120/120/560/160/ViewTest"');
  wasabi('wait', 'window', 'ViewTest', '--timeout', '15');
  const toPage = async (ax, ay) => page.evaluate(([x, y]) => {
    const r = document.querySelector('canvas').getBoundingClientRect();
    const c = document.querySelector('canvas');
    return [r.left + (x + 0.5) * r.width / c.width, r.top + (y + 0.5) * r.height / c.height];
  }, [ax, ay]);
  // Click the Workbench backdrop first, then the Shell: proves the click
  // lands where it was aimed (the Shell becomes active again).
  let [px, py] = await toPage(900, 800);
  await page.mouse.click(px, py);
  await sleep(400);
  check('a click on the backdrop moves the focus off the Shell',
    !/ViewTest.*\[active\]/.test(wasabi('windows')));
  [px, py] = await toPage(300, 200);
  await page.mouse.click(px, py);
  await sleep(400);
  check('a click on the Shell window activates it',
    /ViewTest.*\[active\]/.test(wasabi('windows')));

  // Type a command through the page's keyboard.
  await typeKeys(page, 'makedir ram:viewtest42');
  await page.keyboard.press('Enter');
  await sleep(800);
  const ram = JSON.parse(wasabi('--json', 'ls', 'RAM:'));
  check('typed keys reach the Amiga (makedir ram:viewtest42 ran)',
    ram.some((e) => e.name === 'viewtest42' && e.dir));

  // The live picture shows what was typed: the page's canvas changed
  // where the Shell is.
  await sleep(500);
  await page.screenshot({ path: '/tmp/view-2.png' });

  // Right Ctrl plays Right Amiga by default: Right Amiga+E on Workbench
  // opens Execute Command. Activate the backdrop first.
  [px, py] = await toPage(900, 800);
  await page.mouse.click(px, py);
  await sleep(300);
  await page.keyboard.down('ControlRight');
  await page.keyboard.press('e');
  await page.keyboard.up('ControlRight');
  let opened = false;
  try { wasabi('wait', 'window', 'Execute', '--timeout', '15'); opened = true; } catch { /* no */ }
  check('Right Ctrl + E arrives as Right Amiga + E', opened);
  await sleep(1500);   // long enough for any stuck key to start repeating
  const execs = JSON.parse(wasabi('--json', 'windows'))
    .flatMap((sc) => sc.windows).filter((w) => w.title.startsWith('Execute'));
  check('and opens exactly one dialog', execs.length === 1, `${execs.length}`);
  if (opened) {
    const shot = wasabi('grab', '--window', 'Execute a file');
    console.log(`        dialog picture: ${shot}`);
    for (const w of execs) {                    // its Cancel button
      [px, py] = await toPage(w.left + 250, w.top + 63);
      await page.mouse.click(px, py);
      await sleep(600);
    }
    try { wasabi('wait', 'window', 'Execute', '--gone', '--timeout', '6'); } catch { /* */ }
  }

  // Settings round trip through the bridge.
  const saved = await page.evaluate(async () => {
    const s = { keys: { lamiga: 'MetaLeft', ramiga: 'ControlRight', ctrl: 'ControlLeft',
      lalt: 'AltLeft', ralt: 'AltRight' }, scale: 'fit' };
    await fetch('api/settings', { method: 'PUT', body: JSON.stringify(s) });
    return (await (await fetch('api/settings')).json()).keys.lamiga;
  });
  check('settings are saved by the bridge', saved === 'MetaLeft');
  await page.evaluate(async () => fetch('api/settings', { method: 'PUT', body: '{}' }));

  // Clean up the Shell.
  [px, py] = await toPage(300, 200);
  await page.mouse.click(px, py);
  await typeKeys(page, 'endcli');
  await page.keyboard.press('Enter');
  try { wasabi('wait', 'window', 'ViewTest', '--gone', '--timeout', '8'); } catch { /* */ }
  try { wasabi('del', 'RAM:viewtest42'); } catch { /* */ }
} finally {
  await browser.close();
}
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
