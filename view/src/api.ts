/*
 * Talking to the bridge. Anything that changes something carries the
 * X-Wasabi header: the bridge refuses changes without it, because a
 * foreign web page cannot send it (see wasabi_api.py).
 */

export class NeedsForce extends Error {}

/*
 * Which Amiga this window is for (the machine picker); every call and
 * live connection names it. Kept per browser; changing it reloads.
 */
export const MACHINE = localStorage.getItem('wasabi-machine') ?? '';

export function withMachine(url: string): string {
  if (!MACHINE) return url;
  return url + (url.includes('?') ? '&' : '?') + `m=${encodeURIComponent(MACHINE)}`;
}

export function pickMachine(id: string) {
  localStorage.setItem('wasabi-machine', id);
  window.location.reload();
}

/* A WebSocket address for this page, for the chosen machine. */
export function socketUrl(path: string): string {
  const u = new URL(withMachine(path), window.location.href);
  u.protocol = u.protocol === 'https:' ? 'wss:' : 'ws:';
  return u.toString();
}

export async function api<T>(path: string, body?: object): Promise<T> {
  path = withMachine(path);
  const r = await fetch(path, body === undefined
    ? undefined
    : {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-Wasabi': '1' },
        body: JSON.stringify(body),
      });
  const data = (await r.json().catch(() => ({}))) as { error?: string };
  // A server with a login (Wasabi phone) answers 401 once the session
  // has gone: the app shows the login again.
  if (r.status === 401) window.dispatchEvent(new Event('wasabi-login'));
  if (r.status === 428) throw new NeedsForce(data.error ?? 'needs confirming');
  if (!r.ok) throw new Error(data.error ?? `${r.status}`);
  return data as T;
}

export async function putSettings(s: object) {
  await fetch('api/settings', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json', 'X-Wasabi': '1' },
    body: JSON.stringify(s),
  });
}

export type Health = {
  banner?: string;
  temp_c?: number;
  temp_max_c?: number;
  core_v?: number;
  arm_mhz?: number;
  arm_measured_mhz?: number;
  core_mhz?: number;
  mips_68k?: number;
  mips_arm?: number;
  uptime_s?: number;
  jit_used_pct?: number;
  jit_misses?: number;
  emu68?: boolean;
  emu68_version?: string;
  pi_model?: string;
  mailbox?: boolean;
  problems_now?: string[];
  problems_since_boot?: string[];
  chip_free_kb?: number;
  chip_largest_kb?: number;
  chip_total_kb?: number;
  fast_free_kb?: number;
  fast_largest_kb?: number;
  fast_total_kb?: number;
  last_guru?: string | null;
};

export type Info = {
  banner: string;
  host: string;
  system?: string;
  exec?: string;
  volumes: { name: string; total_mb: number; free_mb: number }[];
};

export type Entry = { name: string; dir: boolean; size: number | null; date: string };

export function size(kb: number): string {
  if (kb >= 1024 * 1024) return `${(kb / 1024 / 1024).toFixed(1)} GB`;
  if (kb >= 10240) return `${Math.round(kb / 1024)} MB`;
  return `${kb} KB`;
}

export function bytes(n: number | null): string {
  if (n === null) return '';
  if (n < 1024) return `${n} B`;
  return size(Math.round(n / 1024));
}

export type MachineRow = { id: string; name: string; host: string; port: number;
  auto?: boolean; online: boolean; banner: string };
export type Found = { host: string; port: number; name: string; banner: string; known: boolean };

export type Me = { mode: 'desktop' | 'server'; local_name: string; local_home: string; history?: boolean };
export type AuthState = { required: boolean; setup: boolean; logged_in: boolean };
