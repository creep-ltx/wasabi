// The phone layout's Overview (wasabi serve, a fresh login).
//   node test/docs-phone.mjs URL PASSWORD OUTFILE
import puppeteer from 'puppeteer-core';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const [URL, PW, OUT] = process.argv.slice(2);
const b = await puppeteer.launch({ browser: 'firefox', executablePath: '/usr/bin/firefox', headless: true });
try {
  const q = await b.newPage();
  await q.setViewport({ width: 390, height: 844, deviceScaleFactor: 2, hasTouch: true, isMobile: true });
  await q.goto(URL);
  await q.waitForSelector('input[type="password"]', { timeout: 15000 });
  for (const i of await q.$$('input[type="password"]')) await i.type(PW);
  await q.keyboard.press('Enter');
  await sleep(15000);
  await q.screenshot({ path: OUT });
  console.log(OUT);
} finally {
  await b.close();
}
