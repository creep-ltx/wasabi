# wasabi view - the web page

The page `wasabi view` serves (`../wasabi_view.py` is the bridge it talks to).

- **Stack:** Vite + React + Radix Themes + TypeScript. Only Radix components; never hand-roll
  a control Radix has. The one exception is the Amiga screen's `<canvas>`, which Radix has no
  component for: it is created from code in `src/amiga/link.ts`, so JSX stays Radix-only.
- **Tokens for everything:** raw colours and sizes live only in `src/tokens.css` (`--wv-*`).
  `npm run lint` enforces it (ESLint + stylelint, the same rules as BBX's web part); keep it
  passing.
- **`dist/` is committed** so `wasabi view` works on a machine without Node. After any change
  in `src/`: `npm run lint && npm run build`, and commit `dist/` with the source.
- **Tests:** `node test/live.mjs` (needs `wasabi view --no-browser --stay --port 8071` running
  and the real Amiga) and `node test/settings.mjs`: headless system Firefox through
  puppeteer-core. The test tool types US characters and has no IntlBackslash key - type by
  key position (`typeKeys` in live.mjs). The bridge itself is covered offline by
  `make test` in the repo root.
- The user is not a programmer: plain words, make the technical calls.
