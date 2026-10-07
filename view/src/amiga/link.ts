/*
 * The live link to the Amiga, through the bridge (`wasabi view`).
 *
 * From the bridge, binary messages (all numbers big-endian):
 *
 *   0x10 head   u16 w, u16 h, u8 format (1 RGB565, 2 pen8), u8 full
 *   0x11 pal    u16 n, then n RGB triples (pen8 screens)
 *   0x12 rect   u16 x, y, w, h, then the rectangle's pixels
 *
 * - wasabid's LIVE frames, forwarded untouched: only what changed, in a
 * compact format. A daemon without LIVE gets the old form instead -
 *   0x01 band   u16 w, u16 h, u16 y, u16 rows, then rows*w RGB
 * - whole rows of a grab. Text messages are JSON status.
 *
 * A native non-interlaced screen (640x256) has pixels twice as wide as
 * tall: the canvas keeps the Amiga's own rows and is shown with each
 * row doubled (rowScale), and the pointer is sent in that doubled
 * space, which is what Intuition's IECLASS_POINTERPOS expects there.
 *
 * To the bridge: JSON - pointer moves and buttons in the Amiga screen's
 * own pixels, keys as Amiga raw codes going down or up. The bridge
 * turns those into wasabid's MOUSE and KEY commands.
 */

import { amigaCodeFor, type AmigaKeySettings } from './keys';
import { socketUrl } from '../api';

export type LinkStatus = {
  connected: boolean;
  banner: string;
  error: string;
  width: number;
  height: number;
  fps: number;
};

type Listener = (s: LinkStatus) => void;

const BUTTON_OF: Record<number, number> = { 0: 0, 2: 1, 1: 2 }; // DOM -> left/right/middle

export class AmigaLink {
  private ws: WebSocket | null = null;
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private image: ImageData | null = null;
  private rowScale = 1;
  private fmt = 0;
  private pens: Uint8Array | null = null;     // pen8 screens: pen per pixel
  private palette = new Uint8Array(256 * 3);
  private status: LinkStatus = {
    connected: false,
    banner: '',
    error: '',
    width: 0,
    height: 0,
    fps: 0,
  };
  private listener: Listener;
  private keys: AmigaKeySettings;
  private held = new Map<string, number>(); // PC code -> Amiga code held down
  private buttons = new Set<number>();
  private lastMove = 0;
  private pendingMove: { x: number; y: number } | null = null;
  private moveTimer = 0;
  private reconnectTimer = 0;
  private closed = false;
  private framesThisSecond = 0;
  private fpsTimer = 0;

  constructor(host: HTMLElement, keys: AmigaKeySettings, listener: Listener) {
    this.listener = listener;
    this.keys = keys;
    // The canvas is made here rather than in JSX: it has no Radix
    // component, and the page itself is built from Radix only.
    this.canvas = document.createElement('canvas');
    this.canvas.className = 'wv-screen';
    this.canvas.tabIndex = 0;
    this.canvas.width = 640;
    this.canvas.height = 512;
    host.appendChild(this.canvas);
    const ctx = this.canvas.getContext('2d');
    if (!ctx) throw new Error('no 2D canvas');
    this.ctx = ctx;
    this.attachInput();
    this.fpsTimer = window.setInterval(() => {
      this.update({ fps: this.framesThisSecond });
      this.framesThisSecond = 0;
    }, 1000);
    this.connect();
  }

  setKeys(keys: AmigaKeySettings) {
    this.releaseAll();
    this.keys = keys;
  }

  focus() {
    this.canvas.focus();
  }

  get element(): HTMLCanvasElement {
    return this.canvas;
  }

  close() {
    this.closed = true;
    window.clearInterval(this.fpsTimer);
    window.clearTimeout(this.reconnectTimer);
    this.ws?.close();
    this.canvas.remove();
  }

  private update(part: Partial<LinkStatus>) {
    this.status = { ...this.status, ...part };
    this.listener(this.status);
  }

  private connect() {
    const ws = new WebSocket(socketUrl('ws'));
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') this.onStatus(ev.data);
      else this.onBinary(ev.data as ArrayBuffer);
    };
    ws.onclose = () => {
      this.update({ connected: false });
      if (!this.closed) {
        // The bridge restarting, a reboot: keep trying, quietly.
        this.reconnectTimer = window.setTimeout(() => this.connect(), 1000);
      }
    };
  }

  private onStatus(text: string) {
    let msg: { t?: string; connected?: boolean; banner?: string; error?: string };
    try {
      msg = JSON.parse(text);
    } catch {
      return;
    }
    if (msg.t === 'status') {
      this.update({
        connected: !!msg.connected,
        banner: msg.banner ?? '',
        error: msg.error ?? '',
      });
    } else if (msg.t === 'frame') {
      this.framesThisSecond++;
    }
  }

  private onBinary(buf: ArrayBuffer) {
    const v = new DataView(buf);
    switch (v.getUint8(0)) {
      case 0x01:
        return this.onBand(v, buf);
      case 0x10:
        return this.onHead(v);
      case 0x11:
        return this.onPalette(v, buf);
      case 0x12:
        return this.onRect(v, buf);
    }
  }

  /* The same rule `wasabi grab` uses: double the rows until the shape
   * is plausible. No square-pixel mode is 2:1 or wider. */
  private static rowScaleFor(w: number, h: number): number {
    let k = 1;
    while (h && w >= 2 * h * k) k *= 2;
    return k;
  }

  private resize(w: number, h: number, rowScale: number) {
    if (w === this.canvas.width && h === this.canvas.height && this.image &&
        rowScale === this.rowScale) return;
    this.canvas.width = w;
    this.canvas.height = h;
    this.rowScale = rowScale;
    this.image = this.ctx.createImageData(w, h);
    this.update({ width: w, height: h * rowScale });
  }

  /* Old form: whole RGB rows, already row-doubled by the bridge. */
  private onBand(v: DataView, buf: ArrayBuffer) {
    const w = v.getUint16(1);
    const h = v.getUint16(3);
    const y = v.getUint16(5);
    const rows = v.getUint16(7);
    this.resize(w, h, 1);
    if (!this.image) return;
    // RGB in, RGBA out: the canvas wants an alpha byte per pixel.
    const src = new Uint8Array(buf, 9, rows * w * 3);
    const dst = this.image.data;
    let s = 0;
    let d = y * w * 4;
    const end = d + rows * w * 4;
    while (d < end) {
      dst[d] = src[s];
      dst[d + 1] = src[s + 1];
      dst[d + 2] = src[s + 2];
      dst[d + 3] = 255;
      d += 4;
      s += 3;
    }
    this.ctx.putImageData(this.image, 0, 0, 0, y, w, rows);
  }

  private onHead(v: DataView) {
    const w = v.getUint16(1);
    const h = v.getUint16(3);
    const fmt = v.getUint8(5);
    const full = v.getUint8(6);
    if (full || fmt !== this.fmt) {
      this.image = null;                     // force a fresh canvas
      this.pens = fmt === 2 ? new Uint8Array(w * h) : null;
    }
    this.fmt = fmt;
    this.resize(w, h, AmigaLink.rowScaleFor(w, h));
  }

  private onPalette(v: DataView, buf: ArrayBuffer) {
    const n = Math.min(256, v.getUint16(1));
    this.palette.set(new Uint8Array(buf, 3, n * 3));
    // Every pixel may have changed colour without changing its pen.
    if (this.pens && this.image) {
      this.paintPens(0, 0, this.canvas.width, this.canvas.height);
      this.ctx.putImageData(this.image, 0, 0);
    }
  }

  private paintPens(x0: number, y0: number, rw: number, rh: number) {
    if (!this.pens || !this.image) return;
    const w = this.canvas.width;
    const dst = this.image.data;
    const pal = this.palette;
    for (let y = y0; y < y0 + rh; y++) {
      let p = y * w + x0;
      let d = p * 4;
      for (let x = 0; x < rw; x++, p++, d += 4) {
        const c = this.pens[p] * 3;
        dst[d] = pal[c];
        dst[d + 1] = pal[c + 1];
        dst[d + 2] = pal[c + 2];
        dst[d + 3] = 255;
      }
    }
  }

  private onRect(v: DataView, buf: ArrayBuffer) {
    if (!this.image) return;
    const x0 = v.getUint16(1);
    const y0 = v.getUint16(3);
    const rw = v.getUint16(5);
    const rh = v.getUint16(7);
    const w = this.canvas.width;
    if (this.fmt === 2 && this.pens) {
      const src = new Uint8Array(buf, 9, rw * rh);
      for (let y = 0; y < rh; y++) {
        this.pens.set(src.subarray(y * rw, (y + 1) * rw), (y0 + y) * w + x0);
      }
      this.paintPens(x0, y0, rw, rh);
    } else {
      // RGB565, big-endian: widen each channel back to 8 bits.
      const dst = this.image.data;
      let s = 9;
      for (let y = 0; y < rh; y++) {
        let d = ((y0 + y) * w + x0) * 4;
        for (let x = 0; x < rw; x++, s += 2, d += 4) {
          const px = v.getUint16(s);
          const r = px >> 11;
          const g = (px >> 5) & 63;
          const b = px & 31;
          dst[d] = (r << 3) | (r >> 2);
          dst[d + 1] = (g << 2) | (g >> 4);
          dst[d + 2] = (b << 3) | (b >> 2);
          dst[d + 3] = 255;
        }
      }
    }
    this.ctx.putImageData(this.image, 0, 0, x0, y0, rw, rh);
  }

  private send(obj: object) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(obj));
    }
  }

  /* --- input -------------------------------------------------------- */

  private amigaXY(ev: MouseEvent): { x: number; y: number } {
    const r = this.canvas.getBoundingClientRect();
    const x = Math.floor(((ev.clientX - r.left) / r.width) * this.canvas.width);
    // In the shown (row-doubled) space: the one the pointer uses.
    const shownH = this.canvas.height * this.rowScale;
    const y = Math.floor(((ev.clientY - r.top) / r.height) * shownH);
    return {
      x: Math.max(0, Math.min(this.canvas.width - 1, x)),
      y: Math.max(0, Math.min(shownH - 1, y)),
    };
  }

  /* Moves are sent at most every 20 ms and only the latest: a busy
   * pointer must not queue up behind itself. */
  private queueMove(p: { x: number; y: number }) {
    this.pendingMove = p;
    const wait = 20 - (performance.now() - this.lastMove);
    if (wait <= 0) this.flushMove();
    else if (!this.moveTimer) this.moveTimer = window.setTimeout(() => this.flushMove(), wait);
  }

  private flushMove() {
    window.clearTimeout(this.moveTimer);
    this.moveTimer = 0;
    if (!this.pendingMove) return;
    this.send({ t: 'move', ...this.pendingMove });
    this.pendingMove = null;
    this.lastMove = performance.now();
  }

  releaseAll() {
    for (const code of this.held.values()) this.send({ t: 'key', code, down: false });
    this.held.clear();
    for (const b of this.buttons) this.send({ t: 'button', b, down: false });
    this.buttons.clear();
  }

  /* Text from a phone's keyboard: the Amiga's keymap picks the keys. */
  sendText(s: string) {
    this.send({ t: 'text', s });
  }

  /* One key, with modifier qualifier bits (0x80 = Right Amiga ...). */
  press(code: number, qual = 0) {
    this.send({ t: 'press', code, qual });
  }

  /*
   * Fingers on a phone: a tap is a left click where it lands; a finger
   * that moves is a left-button drag; a finger held still for half a
   * second is the RIGHT button held - the Amiga's menus: keep holding,
   * slide onto the menu item, lift. A cancelled pointerdown stops the
   * browser's own mouse events, so a tap is not also a click.
   */
  private attachTouch() {
    const c = this.canvas;
    let start: { x: number; y: number; at: { x: number; y: number } } | null = null;
    let mode: 'none' | 'left' | 'right' = 'none';
    let timer = 0;
    c.addEventListener('pointerdown', (ev) => {
      if (ev.pointerType === 'mouse') return;
      ev.preventDefault();
      c.setPointerCapture(ev.pointerId);
      const at = this.amigaXY(ev);
      start = { x: ev.clientX, y: ev.clientY, at };
      mode = 'none';
      window.clearTimeout(timer);
      timer = window.setTimeout(() => {
        if (!start || mode !== 'none') return;
        mode = 'right';
        this.queueMove(start.at);
        this.flushMove();
        this.buttons.add(1);
        this.send({ t: 'button', b: 1, down: true });
        navigator.vibrate?.(20);
      }, 500);
    });
    c.addEventListener('pointermove', (ev) => {
      if (ev.pointerType === 'mouse' || !start) return;
      const far = Math.hypot(ev.clientX - start.x, ev.clientY - start.y) > 10;
      if (mode === 'none' && far) {
        window.clearTimeout(timer);
        mode = 'left';
        this.queueMove(start.at);
        this.flushMove();
        this.buttons.add(0);
        this.send({ t: 'button', b: 0, down: true });
      }
      if (mode !== 'none') this.queueMove(this.amigaXY(ev));
    });
    const end = (ev: PointerEvent) => {
      if (ev.pointerType === 'mouse' || !start) return;
      window.clearTimeout(timer);
      if (mode === 'none') {
        this.queueMove(start.at);
        this.flushMove();
        this.send({ t: 'button', b: 0, down: true });
        this.send({ t: 'button', b: 0, down: false });
      } else {
        this.queueMove(this.amigaXY(ev));
        this.flushMove();
        const b = mode === 'right' ? 1 : 0;
        this.buttons.delete(b);
        this.send({ t: 'button', b, down: false });
      }
      start = null;
      mode = 'none';
    };
    c.addEventListener('pointerup', end);
    c.addEventListener('pointercancel', end);
  }

  private attachInput() {
    const c = this.canvas;
    this.attachTouch();
    c.addEventListener('mousemove', (ev) => this.queueMove(this.amigaXY(ev)));
    c.addEventListener('mousedown', (ev) => {
      ev.preventDefault();
      c.focus();
      const b = BUTTON_OF[ev.button];
      if (b === undefined) return;
      this.queueMove(this.amigaXY(ev));
      this.flushMove(); // the press lands where the pointer is
      this.buttons.add(b);
      this.send({ t: 'button', b, down: true });
    });
    window.addEventListener('mouseup', (ev) => {
      const b = BUTTON_OF[ev.button];
      if (b === undefined || !this.buttons.has(b)) return;
      this.flushMove();
      this.buttons.delete(b);
      this.send({ t: 'button', b, down: false });
    });
    c.addEventListener('contextmenu', (ev) => ev.preventDefault());
    // The mouse wheel, the NewMouse way: one notch is one press of raw
    // key $7A (up) or $7B (down) - MultiView, Workbench drawers and MUI
    // lists scroll to it. A touchpad's small deltas are gathered into
    // notches, so a gentle swipe is not a flood of presses.
    let wheel = 0;
    c.addEventListener('wheel', (ev) => {
      ev.preventDefault();
      wheel += ev.deltaMode === 1 ? ev.deltaY * 40 : ev.deltaMode === 2 ? ev.deltaY * 400 : ev.deltaY;
      while (Math.abs(wheel) >= 40) {
        const down = wheel > 0;
        this.press(down ? 0x7b : 0x7a);
        wheel += down ? -40 : 40;
      }
    }, { passive: false });
    c.addEventListener('keydown', (ev) => {
      const code = amigaCodeFor(ev.code, this.keys);
      if (code === undefined) return;
      ev.preventDefault();
      if (ev.code === 'CapsLock') {
        // An Amiga's Caps Lock reports its LED: down when it lights,
        // up when it goes out. One PC press toggles it.
        const on = !this.held.has('CapsLock');
        if (on) this.held.set('CapsLock', code);
        else this.held.delete('CapsLock');
        this.send({ t: 'key', code, down: on });
        return;
      }
      this.held.set(ev.code, code);
      this.send({ t: 'key', code, down: true, repeat: ev.repeat });
    });
    c.addEventListener('keyup', (ev) => {
      if (ev.code === 'CapsLock') return;
      const code = this.held.get(ev.code);
      if (code === undefined) return;
      ev.preventDefault();
      this.held.delete(ev.code);
      this.send({ t: 'key', code, down: false });
    });
    // Leaving the window with a key or button held would leave it held
    // on the Amiga: let go of everything instead.
    c.addEventListener('blur', () => {
      const caps = this.held.get('CapsLock');
      this.held.delete('CapsLock');
      this.releaseAll();
      if (caps !== undefined) this.held.set('CapsLock', caps);
    });
  }
}
