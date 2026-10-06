/*
 * The live link to the Amiga, through the bridge (`wasabi view`).
 *
 * From the bridge: binary messages are bands of changed rows -
 *   u8 1, u16 width, u16 height, u16 y, u16 rows, then rows*width RGB
 * (big-endian), painted straight into the canvas; text messages are
 * JSON status. A new screen size repaints from scratch.
 *
 * To the bridge: JSON - pointer moves and buttons in the Amiga screen's
 * own pixels, keys as Amiga raw codes going down or up. The bridge
 * turns those into wasabid's MOUSE and KEY commands.
 */

import { amigaCodeFor, type AmigaKeySettings } from './keys';

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
    const url = new URL('ws', window.location.href);
    url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') this.onStatus(ev.data);
      else this.onBand(ev.data as ArrayBuffer);
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

  private onBand(buf: ArrayBuffer) {
    const v = new DataView(buf);
    if (v.getUint8(0) !== 1) return;
    const w = v.getUint16(1);
    const h = v.getUint16(3);
    const y = v.getUint16(5);
    const rows = v.getUint16(7);
    if (w !== this.canvas.width || h !== this.canvas.height || !this.image) {
      this.canvas.width = w;
      this.canvas.height = h;
      this.image = this.ctx.createImageData(w, h);
      this.update({ width: w, height: h });
    }
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

  private send(obj: object) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(obj));
    }
  }

  /* --- input -------------------------------------------------------- */

  private amigaXY(ev: MouseEvent): { x: number; y: number } {
    const r = this.canvas.getBoundingClientRect();
    const x = Math.floor(((ev.clientX - r.left) / r.width) * this.canvas.width);
    const y = Math.floor(((ev.clientY - r.top) / r.height) * this.canvas.height);
    return {
      x: Math.max(0, Math.min(this.canvas.width - 1, x)),
      y: Math.max(0, Math.min(this.canvas.height - 1, y)),
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

  private attachInput() {
    const c = this.canvas;
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
