// How long from a key press in the page until the Amiga's answer is on
// the page's canvas: type into a Shell, watch the canvas where it is.
import puppeteer from 'puppeteer-core';
import { execFileSync } from 'node:child_process';
const wasabi = (...a) => execFileSync('wasabi', a, { encoding: 'utf8' }).trim();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const b = await puppeteer.launch({ browser: 'firefox', executablePath: '/usr/bin/firefox', headless: true });
try {
  const p = await b.newPage();
  await p.setViewport({ width: 1400, height: 1100 });
  await p.goto(process.argv[2] || 'http://127.0.0.1:8071/');
  await p.waitForFunction(() => document.querySelector('canvas')?.width === 1280);
  await p.evaluate(() => {
    window.bytes = 0;
    const orig = WebSocket.prototype.addEventListener;
    // count what arrives: wrap onmessage via a capturing listener
    const ws = [...document.querySelectorAll('canvas')];
    void ws; void orig;
  });
  wasabi('run', '--max-time', '10', 'NewShell "CON:120/120/560/160/Latency"');
  wasabi('wait', 'window', 'Latency', '--timeout', '15');
  await sleep(1500);
  const r = await p.evaluate(() => { const b = document.querySelector('canvas').getBoundingClientRect(); return [b.left, b.top, b.width, b.height]; });
  await p.mouse.click(r[0] + 300 * r[2] / 1280, r[1] + 200 * r[3] / 960);
  await sleep(800);
  const times = [];
  for (const k of ['KeyA', 'KeyB', 'KeyC', 'KeyD', 'KeyE', 'KeyF', 'KeyG', 'KeyH']) {
    // fingerprint the Shell's text area, press, wait for it to change
    const before = await p.evaluate(() => {
      const c = document.querySelector('canvas');
      return Array.from(c.getContext('2d').getImageData(124, 135, 552, 40).data).join(',');
    });
    const t0 = Date.now();
    await p.keyboard.press(k);
    await p.waitForFunction((b0) => {
      const c = document.querySelector('canvas');
      return Array.from(c.getContext('2d').getImageData(124, 135, 552, 40).data).join(',') !== b0;
    }, { polling: 5, timeout: 5000 }, before);
    times.push(Date.now() - t0);
    await sleep(300);
  }
  times.sort((a, b) => a - b);
  console.log(`key press -> on the page: ${times.join(', ')} ms (median ${times[times.length >> 1]})`);
  await p.keyboard.press('Enter');
  await p.mouse.click(r[0] + 300 * r[2] / 1280, r[1] + 200 * r[3] / 960);
  for (const k of ['KeyE', 'KeyN', 'KeyD', 'KeyC', 'KeyL', 'KeyI', 'Enter']) await p.keyboard.press(k);
  try { wasabi('wait', 'window', 'Latency', '--gone', '--timeout', '8'); } catch { /* */ }
} finally {
  await b.close();
}
