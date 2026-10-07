// The Developer page against the real Amiga: the log streams, the task
// list, running a command, the screen list.
// Needs: wasabi desktop --no-browser --stay --port 8071.
import puppeteer from 'puppeteer-core';
import { execFileSync } from 'node:child_process';
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
  // Radix tabs hold their label twice (to keep bold from shifting), so
  // a tab's text is matched by its start.
  const clickText = async (sel, t) => {
    const h = await p.waitForFunction((s, tt) => [...document.querySelectorAll(s)]
      .find((e) => (e.textContent.trim() === tt || (e.getAttribute('role') === 'tab' &&
        e.textContent.trim().startsWith(tt))) && e.offsetParent !== null), { timeout: 10000 }, sel, t);
    await h.asElement().click();
  };
  await p.goto(`${BASE}#dev`);
  await p.waitForFunction(() => document.body.innerText.includes('Debug output'), { timeout: 10000 });
  await sleep(1500);
  check('the debug stream comes on', await p.evaluate(() => [...document.querySelectorAll('label')]
    .some((l) => l.textContent.includes('Debug output') && /\bon$/.test(l.textContent.trim()))));

  // turn snoop on, make some DOS calls, see them
  await clickText('label', 'DOS calls (snoop) off');
  await sleep(1500);
  execFileSync('wasabi', ['run', '--max-time', '10', 'List SYS:S QUICK'], { stdio: 'ignore' });
  await sleep(2000);
  const t1 = await text();
  check('snoop lines appear', /snoop .*(Lock|Open|Examine)/.test(t1));
  await p.screenshot({ path: '/tmp/dev-logs.png' });

  // Tasks
  await clickText('[role="tab"]', 'Tasks');
  await p.waitForFunction(() => document.body.innerText.includes('Stack free'), { timeout: 10000 });
  await sleep(800);
  check('the task list shows', /Workbench/.test(await text()));
  await p.screenshot({ path: '/tmp/dev-tasks.png' });

  // Run
  await clickText('[role="tab"]', 'Run');
  await p.waitForSelector('input[placeholder^="An AmigaDOS command"]');
  await p.type('input[placeholder^="An AmigaDOS command"]', 'Version');
  await p.keyboard.press('Enter');
  await p.waitForFunction(() => document.body.innerText.includes('returned 0'), { timeout: 15000 });
  check('a command runs and shows its output and code', (await text()).includes('Kickstart'));
  await p.screenshot({ path: '/tmp/dev-run.png' });

  // Screens
  await clickText('[role="tab"]', 'Screens');
  await p.waitForFunction(() => document.body.innerText.includes('in front'), { timeout: 10000 });
  check('the screen list shows', true);
  await p.screenshot({ path: '/tmp/dev-screens.png' });

  // back to Logs: still alive, lines kept
  await clickText('[role="tab"]', 'Logs');
  await sleep(500);
  check('Logs kept its lines while away', /snoop .*(Lock|Open|Examine)/.test(await text()));
  check('no page errors', !errors.length, errors.join(' | '));
} finally {
  await b.close();
}
console.log(failed ? `${failed} failed` : 'all passed');
process.exit(failed ? 1 : 0);
