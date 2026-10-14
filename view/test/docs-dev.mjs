// The Developer page with the DOS call trace running, and an Overview of
// whatever machine the app at BASE is looking at.
//   node test/docs-dev.mjs BASE OUT what   (what: dev | overview)
import puppeteer from 'puppeteer-core';
import { spawn } from 'node:child_process';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const [BASE, OUT, WHAT] = process.argv.slice(2);
const b = await puppeteer.launch({ browser: 'firefox', executablePath: '/usr/bin/firefox', headless: true });
try {
  const p = await b.newPage();
  await p.setViewport({ width: 1440, height: 900 });
  if (WHAT === 'overview') {
    await p.goto(`${BASE}#overview`);
    await sleep(15000);
    await p.screenshot({ path: `${OUT}/desktop-overview.png` });
  } else {
    await p.goto(`${BASE}#dev`);
    await sleep(3000);
    const h = await p.waitForFunction(() => [...document.querySelectorAll('label, span')]
      .find((e) => e.textContent.includes('DOS calls (snoop)')), { timeout: 10000 });
    await h.asElement().click();
    await sleep(2000);
    spawn('../wasabi', ['run', '--max-time', '20', 'list SYS:Prefs'], { stdio: 'ignore' });
    await sleep(8000);
    await p.screenshot({ path: `${OUT}/desktop-dev.png` });
  }
  console.log('ok');
} finally {
  await b.close();
}
