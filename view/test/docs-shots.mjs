// The README's pictures: every page of the desktop app at 1440x900, the
// Files page with an Amiga drawer open, and the phone layout. Run against
// an emulator (nothing private on screen) - see docs/README for how.
//   node test/docs-shots.mjs http://127.0.0.1:8071/ OUTDIR [phone-url pw]
import puppeteer from 'puppeteer-core';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const [BASE = 'http://127.0.0.1:8071/', OUT = '/tmp/shots', PHONE, PW] =
  process.argv.slice(2);
const b = await puppeteer.launch({ browser: 'firefox', executablePath: '/usr/bin/firefox', headless: true });
const clickText = async (p, sel, t) => {
  const h = await p.waitForFunction((s, tt) => [...document.querySelectorAll(s)]
    .find((e) => e.children.length === 0 && e.textContent.trim().endsWith(tt) &&
      e.offsetParent !== null),
  { timeout: 15000 }, sel, t);
  await h.asElement().click();
};
try {
  const p = await b.newPage();
  await p.setViewport({ width: 1440, height: 900 });
  for (const page of ['overview', 'screen', 'dev']) {
    await p.goto(`${BASE}#${page}`);
    await sleep(page === 'overview' ? 9000 : 4000);
    await p.screenshot({ path: `${OUT}/desktop-${page}.png` });
    console.log(`${OUT}/desktop-${page}.png`);
  }
  await p.goto(`${BASE}#files`);
  await sleep(3000);
  await clickText(p, '*', 'AmigaOS3.2:');
  await sleep(2500);
  await p.screenshot({ path: `${OUT}/desktop-files.png` });
  console.log(`${OUT}/desktop-files.png`);

  if (PHONE) {
    const q = await b.newPage();
    await q.setViewport({ width: 390, height: 844, deviceScaleFactor: 2, hasTouch: true, isMobile: true });
    await q.goto(PHONE);
    await q.waitForSelector('input[type="password"]', { timeout: 15000 });
    const pws = await q.$$('input[type="password"]');
    for (const i of pws) await i.type(PW);
    await q.keyboard.press('Enter');
    await sleep(9000);
    await q.screenshot({ path: `${OUT}/phone-overview.png` });
    console.log(`${OUT}/phone-overview.png`);
    await q.goto(`${PHONE}#screen`);
    await sleep(15000);
    await q.screenshot({ path: `${OUT}/phone-screen.png` });
    console.log(`${OUT}/phone-screen.png`);
  }
} finally {
  await b.close();
}
