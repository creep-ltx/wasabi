// A picture of every page of the desktop app, for looking at the design.
import puppeteer from 'puppeteer-core';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const BASE = process.argv[2] || 'http://127.0.0.1:8071/';
const b = await puppeteer.launch({ browser: 'firefox', executablePath: '/usr/bin/firefox', headless: true });
try {
  const p = await b.newPage();
  const errors = [];
  p.on('pageerror', (e) => errors.push(String(e.message)));
  await p.setViewport({ width: 1440, height: 900 });
  for (const page of ['overview', 'screen', 'files', 'shots']) {
    await p.goto(`${BASE}#${page}`);
    await sleep(page === 'overview' ? 9000 : 3500);
    await p.screenshot({ path: `/tmp/desk-${page}.png` });
    console.log(`/tmp/desk-${page}.png`);
  }
  if (errors.length) console.log('page errors:', errors.join(' | '));
} finally {
  await b.close();
}
