# wasabi view / desktop - the web app

The app `wasabi desktop` serves (and `wasabi view`, which is the same app with `?mode=view`:
the Screen page alone). `../wasabi_view.py` is the bridge, `../wasabi_api.py` the API.
Pages: `src/pages/` (Overview, Screen, Files, Shots); `src/App.tsx` is the shell and the
`#hash` routing; `src/api.ts` sends `X-Wasabi` on every change (the bridge refuses
changes without it).

- **Stack:** Vite + React + Radix Themes + TypeScript. Only Radix components; never hand-roll
  a control Radix has. Exceptions, where Radix has no component: the Amiga screen's
  `<canvas>` (created from code in `src/amiga/link.ts`), the sparkline's SVG elements and
  screenshot `<img>` - listed in the ESLint rule; nothing else. Meters are Radix `Progress`.
- **Charts** follow the dataviz skill: one blue series (`--wv-viz-line`, validated on the
  dark card surface), stat tiles, a hover readout, status always icon + words.
- **Tokens for everything:** raw colours and sizes live only in `src/tokens.css` (`--wv-*`).
  `npm run lint` enforces it (ESLint + stylelint, the same rules as BBX's web part); keep it
  passing.
- **`dist/` is committed** so `wasabi view` works on a machine without Node. After any change
  in `src/`: `npm run lint && npm run build`, and commit `dist/` with the source.
- **Tests:** with `wasabi desktop --no-browser --stay --port 8071` running and the real
  Amiga: `node test/live.mjs`, `test/settings.mjs`, `test/latency.mjs` (screen-only page),
  `test/desktop.mjs` (every page; needs /tmp/wdt prepared, see its header) and
  `test/pages.mjs` (a picture of each page to look at): headless system Firefox through
  puppeteer-core. The test tool types US characters and has no IntlBackslash key - type by
  key position (`typeKeys` in live.mjs). The bridge itself is covered offline by
  `make test` in the repo root.
- The user is not a programmer: plain words, make the technical calls.
