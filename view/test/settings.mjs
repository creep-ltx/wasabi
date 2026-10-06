// The Settings dialog: pick PC keys for Amiga keys, see the bridge save
// them, and see a PC key move from one Amiga key to another.
import puppeteer from 'puppeteer-core';
const URL = process.argv[2] || 'http://127.0.0.1:8071/?mode=view';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failed = 0;
const check = (name, ok, extra = '') => {
  console.log(`${ok ? '  ok  ' : '  FAIL'}  ${name}${extra ? ' - ' + extra : ''}`);
  if (!ok) failed++;
};
const b = await puppeteer.launch({ browser: 'firefox', executablePath: '/usr/bin/firefox', headless: true });
try {
  const p = await b.newPage();
  await p.setViewport({ width: 1400, height: 1000 });
  await p.goto(URL);
  await p.evaluate(() => fetch('api/settings', { method: 'PUT', headers: { 'X-Wasabi': '1' }, body: '{}' }));
  await p.reload();
  await p.waitForFunction(() => document.querySelector('canvas')?.width === 1280);
  const clickText = async (sel, text) => {
    const h = await p.waitForFunction((s, t) => [...document.querySelectorAll(s)]
      .find((e) => e.textContent.trim() === t), {}, sel, text);
    await h.asElement().click();
  };
  const pick = async (label, option) => {
    await p.click(`button[aria-label="${label}"]`);
    await sleep(300);
    await clickText('[role="option"]', option);
    await sleep(400);
  };
  const saved = () => p.evaluate(async () => (await (await fetch('api/settings')).json()).keys);
  await clickText('button', 'Settings');
  await sleep(500);
  await p.screenshot({ path: '/tmp/view-settings-1.png' });
  check('the dialog shows the default for Left Amiga (Menu key)',
    (await p.$eval('button[aria-label="Left Amiga"]', (e) => e.textContent)) === 'Menu key');
  await pick('Left Amiga', 'Left Super (Windows key)');
  check('choosing Left Super for Left Amiga is saved', (await saved()).lamiga === 'MetaLeft');
  await pick('Right Amiga', 'Left Super (Windows key)');
  const k = await saved();
  check('giving Left Super to Right Amiga takes it from Left Amiga',
    k.ramiga === 'MetaLeft' && k.lamiga === 'none', JSON.stringify(k));
  check('and the dialog says so',
    (await p.$eval('button[aria-label="Left Amiga"]', (e) => e.textContent)) === 'Not used');
  await p.screenshot({ path: '/tmp/view-settings-2.png' });
  await clickText('button', 'Default keys');
  await sleep(400);
  check('Default keys puts everything back', (await saved()).lamiga === 'ContextMenu');
} finally {
  await b.close();
}
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
