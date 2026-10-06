/*
 * PC keys to Amiga keys, by position.
 *
 * The browser's KeyboardEvent.code names the physical key (KeyQ is the
 * key where Q sits on a US board, whatever the layout says), and the
 * Amiga raw key codes are positions too. So the Amiga's own keymap
 * decides which character a key makes, exactly as at its real keyboard:
 * with a Swedish keymap on the Amiga, the key right of L types ö.
 *
 * The modifier keys are different: which PC key plays which Amiga
 * modifier is a setting (AmigaKeySettings), because the PC has no Amiga
 * keys and the compositor may own Super.
 */

const LETTER_ROWS: [string, number][] = [
  ['QWERTYUIOP', 0x10],
  ['ASDFGHJKL', 0x20],
  ['ZXCVBNM', 0x31],
];

function buildPositions(): Map<string, number> {
  const m = new Map<string, number>([
    ['Backquote', 0x00],
    ['Minus', 0x0b],
    ['Equal', 0x0c],
    ['Backspace', 0x41],
    ['Tab', 0x42],
    ['BracketLeft', 0x1a],
    ['BracketRight', 0x1b],
    ['Enter', 0x44],
    ['Semicolon', 0x29],
    ['Quote', 0x2a],
    // ISO boards: the key left of Enter, and the one between left
    // Shift and Z. Both exist on an international Amiga keyboard.
    ['Backslash', 0x2b],
    ['IntlBackslash', 0x30],
    ['Comma', 0x38],
    ['Period', 0x39],
    ['Slash', 0x3a],
    ['Space', 0x40],
    ['Escape', 0x45],
    ['Delete', 0x46],
    ['Insert', 0x5f], // the PC has no Help key; Insert sits where it would
    ['ArrowUp', 0x4c],
    ['ArrowDown', 0x4d],
    ['ArrowRight', 0x4e],
    ['ArrowLeft', 0x4f],
    ['ShiftLeft', 0x60],
    ['ShiftRight', 0x61],
    ['CapsLock', 0x62],
    ['Numpad0', 0x0f],
    ['Numpad1', 0x1d],
    ['Numpad2', 0x1e],
    ['Numpad3', 0x1f],
    ['Numpad4', 0x2d],
    ['Numpad5', 0x2e],
    ['Numpad6', 0x2f],
    ['Numpad7', 0x3d],
    ['Numpad8', 0x3e],
    ['Numpad9', 0x3f],
    ['NumpadDecimal', 0x3c],
    ['NumpadEnter', 0x43],
    ['NumpadSubtract', 0x4a],
    ['NumpadAdd', 0x5e],
    ['NumpadMultiply', 0x5d],
    ['NumpadDivide', 0x5c],
  ]);
  for (let i = 0; i < 10; i++) {
    m.set(`Digit${(i + 1) % 10}`, 0x01 + i);
    m.set(`F${i + 1}`, 0x50 + i);
  }
  for (const [row, base] of LETTER_ROWS) {
    [...row].forEach((ch, i) => m.set(`Key${ch}`, base + i));
  }
  return m;
}

/** Fixed positions: every key that is the same key on both machines. */
export const POSITIONS = buildPositions();

/** The Amiga modifiers whose PC key is a setting, and their raw codes. */
export const AMIGA_MODIFIERS = [
  { id: 'lamiga', label: 'Left Amiga', code: 0x66 },
  { id: 'ramiga', label: 'Right Amiga', code: 0x67 },
  { id: 'ctrl', label: 'Ctrl', code: 0x63 },
  { id: 'lalt', label: 'Left Alt', code: 0x64 },
  { id: 'ralt', label: 'Right Alt', code: 0x65 },
] as const;

export type AmigaModifier = (typeof AMIGA_MODIFIERS)[number]['id'];

/** PC keys that may play an Amiga modifier ('none' = not used). */
export const PC_MODIFIER_KEYS = [
  { code: 'ControlLeft', label: 'Left Ctrl' },
  { code: 'ControlRight', label: 'Right Ctrl' },
  { code: 'AltLeft', label: 'Left Alt' },
  { code: 'AltRight', label: 'Right Alt (AltGr)' },
  { code: 'MetaLeft', label: 'Left Super (Windows key)' },
  { code: 'MetaRight', label: 'Right Super' },
  { code: 'ContextMenu', label: 'Menu key' },
  { code: 'none', label: 'Not used' },
] as const;

export type AmigaKeySettings = Record<AmigaModifier, string>;

/*
 * The defaults, decided 2026-10-06: Right Amiga is the menu-shortcut
 * key, so it gets the easy Right Ctrl; the Menu key plays Left Amiga;
 * Right Alt stays Right Alt because on a Swedish board it is AltGr and
 * the Amiga's keymap needs it for @ { [ and the rest.
 */
export const DEFAULT_KEYS: AmigaKeySettings = {
  lamiga: 'ContextMenu',
  ramiga: 'ControlRight',
  ctrl: 'ControlLeft',
  lalt: 'AltLeft',
  ralt: 'AltRight',
};

/** Assign a PC key to one Amiga modifier; whoever had it loses it. */
export function assignKey(
  keys: AmigaKeySettings,
  modifier: AmigaModifier,
  pcCode: string,
): AmigaKeySettings {
  const next = { ...keys };
  if (pcCode !== 'none') {
    for (const m of AMIGA_MODIFIERS) {
      if (next[m.id] === pcCode) next[m.id] = 'none';
    }
  }
  next[modifier] = pcCode;
  return next;
}

/** The Amiga raw key for a PC key under these settings, or undefined. */
export function amigaCodeFor(code: string, keys: AmigaKeySettings): number | undefined {
  for (const m of AMIGA_MODIFIERS) {
    if (keys[m.id] === code) return m.code;
  }
  // A modifier PC key that plays nothing must not fall through to a
  // position (there is none for it anyway, but be explicit).
  if (PC_MODIFIER_KEYS.some((k) => k.code === code)) return undefined;
  return POSITIONS.get(code);
}
