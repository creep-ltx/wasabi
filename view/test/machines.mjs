// Two machines in one app: the real A1200 (found automatically) and a
// second one added by address - here the test suite's fake Amiga on
// 127.0.0.1:18500 with its own key. Switching shows the other machine.
import puppeteer from 'puppeteer-core';
const BASE = process.argv[2] || 'http://127.0.0.1:8071/';
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
  const text = () => p.evaluate(() => document.body.innerText);
  const clickText = async (sel, t) => {
    const h = await p.waitForFunction((s, tt) => [...document.querySelectorAll(s)]
      .find((e) => e.textContent.trim() === tt && e.offsetParent !== null), { timeout: 10000 }, sel, t);
    await h.asElement().click();
  };
  await p.goto(`${BASE}#overview`);
  await p.evaluate(() => localStorage.removeItem('wasabi-machine'));
  await p.reload();
  await p.waitForFunction(() => document.body.innerText.includes('Kickstart'), { timeout: 20000 });
  check('the first machine is the real A1200', (await text()).includes('wasabid 0.3b5'));

  // add the fake one by address, with its own key
  await p.waitForSelector('button[aria-label="Machine"]', { visible: true, timeout: 10000 });
  await p.click('button[aria-label="Machine"]');
  await clickText('[role="option"]', 'Manage machines…');
  await p.waitForSelector('input[placeholder="Name (FS-UAE)"]');
  await p.type('input[placeholder="Name (FS-UAE)"]', 'Fake Amiga');
  await p.type('input[placeholder^="Address"]', '127.0.0.1:18500');
  await p.type('input[placeholder^="Key, if"]', 'fakekey');
  await clickText('[role="dialog"] button', 'Add');
  await p.waitForFunction(() => document.body.innerText.includes('mock-wasabid'), { timeout: 10000 }).catch(() => {});
  check('an added machine shows as online, with its banner', (await text()).includes('mock-wasabid'));
  await p.screenshot({ path: '/tmp/machines-dialog.png' });
  await clickText('[role="dialog"] button', 'Done');
  await sleep(500);

  // switch to it
  await p.waitForSelector('button[aria-label="Machine"]', { visible: true, timeout: 10000 });
  await p.click('button[aria-label="Machine"]');
  await clickText('[role="option"]', '● Fake Amiga');
  await p.waitForFunction(() => document.body.innerText.includes('51.5'), { timeout: 20000 }).catch(() => {});
  check('switching shows the other machine (the fake one is 51.5 °C)', (await text()).includes('51.5'));
  await p.evaluate(() => { window.location.hash = 'files'; });
  await sleep(1500);
  const boxes = await p.$$('input[type="text"], input:not([type])');
  await boxes[boxes.length - 1].click({ count: 3 });
  await boxes[boxes.length - 1].type('C:');
  await boxes[boxes.length - 1].press('Enter');
  await sleep(1500);
  check("and its Files are that machine's", (await text()).includes('fake.txt'));

  // and back
  await p.waitForSelector('button[aria-label="Machine"]', { visible: true, timeout: 10000 });
  await p.click('button[aria-label="Machine"]');
  await clickText('[role="option"]', '● Amiga');
  await p.waitForFunction(() => document.body.innerText.includes('0.3b5'), { timeout: 20000 }).catch(() => {});
  check('switching back shows the A1200 again', (await text()).includes('wasabid 0.3b5'));
  check('no page errors', !errors.length, errors.join(' | '));
} finally {
  await b.close();
}
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
