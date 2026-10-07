import { useEffect, useRef, useState, type CSSProperties, type ReactNode, type RefObject } from 'react';
import {
  Badge,
  Box,
  Button,
  Checkbox,
  Dialog,
  Flex,
  Select,
  Text,
  TextArea,
  TextField,
} from '@radix-ui/themes';
import {
  ArrowDownIcon,
  ArrowLeftIcon,
  ArrowRightIcon,
  ArrowUpIcon,
  CameraIcon,
  EnterFullScreenIcon,
  GearIcon,
  ClipboardIcon,
  KeyboardIcon,
} from '@radix-ui/react-icons';
import { AmigaLink, type LinkStatus } from '../amiga/link';
import { api, putSettings } from '../api';
import {
  AMIGA_MODIFIERS,
  DEFAULT_KEYS,
  POSITIONS,
  PC_MODIFIER_KEYS,
  assignKey,
  type AmigaKeySettings,
  type AmigaModifier,
} from '../amiga/keys';

type Scale = 'fit' | 'one';
type Settings = { keys: AmigaKeySettings; scale: Scale };

const DEFAULTS: Settings = { keys: DEFAULT_KEYS, scale: 'fit' };

/* Settings live with the bridge (~/.config/wasabi/view.json), not in
 * the browser, so every browser - and later the phone - shares them. */
async function loadSettings(): Promise<Settings> {
  try {
    const r = await fetch('api/settings');
    if (!r.ok) return DEFAULTS;
    const s = (await r.json()) as Partial<Settings>;
    return {
      keys: { ...DEFAULT_KEYS, ...(s.keys ?? {}) },
      scale: s.scale === 'one' ? 'one' : 'fit',
    };
  } catch {
    return DEFAULTS;
  }
}

function saveSettings(s: Settings) {
  void putSettings(s);
}

const NO_STATUS: LinkStatus = {
  connected: false,
  banner: '',
  error: '',
  width: 0,
  height: 0,
  fps: 0,
};

/*
 * The live screen. `bare` is `wasabi view`: the screen and a slim bar
 * only; inside the desktop app the same page sits beside the sidebar.
 * The link to the Amiga lives only while this page is shown - an open
 * live view costs the Amiga ~5% of its CPU even when nothing moves.
 */
export function ScreenPage({ bare = false }: { bare?: boolean }) {
  const stageRef = useRef<HTMLDivElement>(null);
  const linkRef = useRef<AmigaLink | null>(null);
  const [settings, setSettings] = useState<Settings | null>(null);
  const [status, setStatus] = useState<LinkStatus>(NO_STATUS);
  const [stage, setStage] = useState({ w: 0, h: 0 });
  const [note, setNote] = useState('');

  useEffect(() => {
    void loadSettings().then(setSettings);
  }, []);

  // One link for the page's life; key settings are handed to it live.
  const ready = settings !== null;
  useEffect(() => {
    if (!ready || !stageRef.current) return;
    const link = new AmigaLink(stageRef.current, DEFAULT_KEYS, setStatus);
    linkRef.current = link;
    return () => {
      link.close();
      linkRef.current = null;
    };
  }, [ready]);

  useEffect(() => {
    if (settings) linkRef.current?.setKeys(settings.keys);
  }, [settings]);

  useEffect(() => {
    const el = stageRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setStage({ w: el.clientWidth, h: el.clientHeight }));
    ro.observe(el);
    return () => ro.disconnect();
  }, [ready]);

  const update = (next: Settings) => {
    setSettings(next);
    saveSettings(next);
  };

  // The displayed size: the whole Amiga screen, as large as fits, aspect
  // kept - or its own pixels, one to one.
  const w = status.width || 640;
  const h = status.height || 512;
  const scale =
    settings?.scale === 'one' || !stage.w || !stage.h
      ? 1
      : Math.min(stage.w / w, stage.h / h);
  const screenVars = {
    '--wv-screen-w': String(Math.floor(w * scale)),
    '--wv-screen-h': String(Math.floor(h * scale)),
  } as CSSProperties;

  return (
    <Flex direction="column" className={bare ? 'wv-page' : 'wv-screen-page'}>
      <Flex className="wv-bar" align="center" gap="3">
        <Badge color={status.connected ? 'green' : 'red'}>
          {status.connected ? 'connected' : 'not connected'}
        </Badge>
        <Text size="2" color="gray">
          {status.connected
            ? `${bare ? status.banner + ' · ' : ''}${status.width}×${status.height} · ${status.fps} fps`
            : status.error || 'looking for the Amiga…'}
        </Text>
        <Box flexGrow="1" />
        <Box display={{ initial: 'none', md: 'block' }}>
          <Text size="1" color="gray">
            {note || "Click the screen to use the Amiga's mouse and keyboard"}
          </Text>
        </Box>
        <Button
          size="1"
          variant="soft"
          onClick={() => {
            setNote('Taking a screenshot…');
            api<{ name: string }>('api/grab', {})
              .then((r) => setNote(`Saved ${r.name} in Pictures/Wasabi`))
              .catch((e: Error) => setNote(`Screenshot failed: ${e.message}`));
          }}
        >
          <CameraIcon /> Screenshot
        </Button>
        <Box display={{ initial: 'none', md: 'block' }}>
          <Button
            size="1"
            variant="soft"
            onClick={() => {
              void document.documentElement.requestFullscreen?.();
              linkRef.current?.focus();
            }}
          >
            <EnterFullScreenIcon /> Full screen
          </Button>
        </Box>
        <ClipboardDialog />
        {settings && (
          <SettingsDialog settings={settings} onChange={update} />
        )}
      </Flex>
      <KeyBar link={linkRef} />
      <Box
        ref={stageRef}
        className="wv-stage"
        data-scale={settings?.scale ?? 'fit'}
        style={screenVars}
      />
    </Flex>
  );
}

function SettingsDialog({
  settings,
  onChange,
}: {
  settings: Settings;
  onChange: (s: Settings) => void;
}) {
  return (
    <Dialog.Root>
      <Dialog.Trigger>
        <Button size="1" variant="soft">
          <GearIcon /> Settings
        </Button>
      </Dialog.Trigger>
      <Dialog.Content maxWidth="var(--wv-dialog-width)">
        <Dialog.Title>Settings</Dialog.Title>
        <Dialog.Description size="2" color="gray" mb="4">
          Which PC key plays each Amiga key. A PC key can play only one;
          choosing it here takes it from the other.
        </Dialog.Description>
        <Flex direction="column" gap="3">
          {AMIGA_MODIFIERS.map((m) => (
            <Flex key={m.id} align="center" justify="between" gap="3">
              <Text size="2">{m.label}</Text>
              <Select.Root
                value={settings.keys[m.id]}
                onValueChange={(v) =>
                  onChange({
                    ...settings,
                    keys: assignKey(settings.keys, m.id as AmigaModifier, v),
                  })
                }
              >
                <Select.Trigger aria-label={m.label} />
                <Select.Content>
                  {PC_MODIFIER_KEYS.map((k) => (
                    <Select.Item key={k.code} value={k.code}>
                      {k.label}
                    </Select.Item>
                  ))}
                </Select.Content>
              </Select.Root>
            </Flex>
          ))}
          <Flex align="center" justify="between" gap="3" mt="2">
            <Text size="2">Screen size</Text>
            <Select.Root
              value={settings.scale}
              onValueChange={(v) => onChange({ ...settings, scale: v as Scale })}
            >
              <Select.Trigger aria-label="Screen size" />
              <Select.Content>
                <Select.Item value="fit">Fit the window</Select.Item>
                <Select.Item value="one">Actual pixels (1:1)</Select.Item>
              </Select.Content>
            </Select.Root>
          </Flex>
          <Text size="1" color="gray">
            The Super (Windows) keys may be taken by the desktop before the
            browser sees them. Some Ctrl shortcuts (Ctrl+W, Ctrl+T) belong
            to the browser; Full screen lets the page keep more of them.
          </Text>
        </Flex>
        <Flex gap="3" mt="4" justify="end">
          <Button
            variant="soft"
            color="gray"
            onClick={() => onChange({ ...settings, keys: DEFAULT_KEYS })}
          >
            Default keys
          </Button>
          <Dialog.Close>
            <Button>Done</Button>
          </Dialog.Close>
        </Flex>
      </Dialog.Content>
    </Dialog.Root>
  );
}

/* Keys a phone's keyboard has not got, and the phone's keyboard itself
 * for text. Shown on small screens; a PC has its real keyboard. */
const BAR_KEYS: { label: ReactNode; code: number; name: string }[] = [
  { label: 'Return', code: 0x44, name: 'Return' },
  { label: '⌫', code: 0x41, name: 'Backspace' },
  { label: 'Del', code: 0x46, name: 'Delete' },
  { label: 'Esc', code: 0x45, name: 'Escape' },
  { label: 'Tab', code: 0x42, name: 'Tab' },
  { label: <ArrowUpIcon />, code: 0x4c, name: 'Up' },
  { label: <ArrowDownIcon />, code: 0x4d, name: 'Down' },
  { label: <ArrowLeftIcon />, code: 0x4f, name: 'Left' },
  { label: <ArrowRightIcon />, code: 0x4e, name: 'Right' },
  { label: 'Help', code: 0x5f, name: 'Help' },
];

function KeyBar({ link }: { link: RefObject<AmigaLink | null> }) {
  const [text, setText] = useState('');
  const [amiga, setAmiga] = useState(false);
  const send = () => {
    const l = link.current;
    if (!l || !text) return;
    // With Right Amiga held, one letter or digit is a shortcut: send it
    // by key position with the qualifier; text goes through the keymap.
    const one = text.length === 1 ? POSITIONS.get(/[0-9]/.test(text)
      ? `Digit${text}` : `Key${text.toUpperCase()}`) : undefined;
    if (amiga && one !== undefined) l.press(one, 0x80);
    else l.sendText(text);
    setText('');
  };
  return (
    <Flex className="wv-keybar" gap="1" display={{ initial: 'flex', md: 'none' }}>
      <Dialog.Root>
        <Dialog.Trigger>
          <Button size="1" variant="soft" aria-label="Type on the Amiga"><KeyboardIcon /></Button>
        </Dialog.Trigger>
        <Dialog.Content maxWidth="var(--wv-dialog-width)">
          <Dialog.Title>Type on the Amiga</Dialog.Title>
          <Dialog.Description size="2" color="gray" mb="3">
            Goes to the Amiga's active window. Enter sends.
          </Dialog.Description>
          <TextField.Root autoFocus value={text} placeholder="Text"
            onChange={(e) => setText(e.target.value)}
            onKeyDown={(e) => { if (e.key === 'Enter') send(); }} />
          <Text as="label" size="2" mt="3">
            <Flex gap="2" align="center" mt="3">
              <Checkbox checked={amiga} onCheckedChange={(v) => setAmiga(v === true)} />
              Hold Right Amiga (one letter = a menu shortcut)
            </Flex>
          </Text>
          <Flex gap="3" mt="4" justify="end">
            <Button variant="soft" color="gray" onClick={() => link.current?.press(0x44)}>Return</Button>
            <Button onClick={send} disabled={!text}>Send</Button>
          </Flex>
        </Dialog.Content>
      </Dialog.Root>
      {BAR_KEYS.map((k) => (
        <Button key={k.name} size="1" variant="soft" color="gray" aria-label={k.name}
          onClick={() => link.current?.press(k.code)}>
          {k.label}
        </Button>
      ))}
    </Flex>
  );
}

/*
 * The two clipboards: the Amiga's (what its Copy put there, what Right
 * Amiga+V pastes) and this device's. The browser lets a page use the
 * device's clipboard only on a secure page (this PC's own app is one;
 * the phone over plain http is not) - there the text box stands in.
 */
function ClipboardDialog() {
  const [amiga, setAmiga] = useState('');
  const [send, setSend] = useState('');
  const [note, setNote] = useState('');
  const [busy, setBusy] = useState(false);
  const deviceClip = window.isSecureContext && !!navigator.clipboard;

  const load = () => {
    setBusy(true);
    api<{ text: string }>('api/clip')
      .then((r) => { setAmiga(r.text); setNote(''); })
      .catch((e: Error) => setNote(e.message))
      .finally(() => setBusy(false));
  };
  const put = (paste: boolean) => {
    api('api/clip', { text: send, paste })
      .then(() => setNote(paste ? 'Pasted into the Amiga\'s active window' : 'On the Amiga\'s clipboard'))
      .catch((e: Error) => setNote(e.message));
  };
  return (
    <Dialog.Root onOpenChange={(o) => { if (o) load(); }}>
      <Dialog.Trigger>
        <Button size="1" variant="soft"><ClipboardIcon /> Clipboard</Button>
      </Dialog.Trigger>
      <Dialog.Content maxWidth="var(--wv-dialog-wide)">
        <Dialog.Title>Clipboard</Dialog.Title>
        <Dialog.Description size="2" color="gray" mb="3">
          The Amiga's clipboard is what Copy put there and what Right Amiga+V pastes.
        </Dialog.Description>
        <Flex direction="column" gap="2">
          <Text size="2" weight="medium">On the Amiga's clipboard</Text>
          <TextArea readOnly value={busy ? 'Reading…' : amiga} placeholder="(empty)" rows={4} />
          <Flex gap="2">
            <Button size="1" variant="soft" color="gray" onClick={load}>Read again</Button>
            {deviceClip && (
              <Button size="1" variant="soft" disabled={!amiga} onClick={() => {
                void navigator.clipboard.writeText(amiga).then(() => setNote('Copied to this device'));
              }}>Copy to this device</Button>
            )}
            <Button size="1" variant="soft" disabled={!amiga} onClick={() => setSend(amiga)}>
              Edit below
            </Button>
          </Flex>
          <Text size="2" weight="medium" mt="3">Send to the Amiga</Text>
          <TextArea value={send} onChange={(e) => setSend(e.target.value)} rows={4}
            placeholder={deviceClip ? 'Type or paste here, or take this device\'s clipboard' : 'Type or paste here'} />
          <Flex gap="2" wrap="wrap">
            {deviceClip && (
              <Button size="1" variant="soft" color="gray" onClick={() => {
                navigator.clipboard.readText().then(setSend).catch(() =>
                  setNote('This browser would not hand over its clipboard - paste into the box instead'));
              }}>Take this device's clipboard</Button>
            )}
            <Button size="1" variant="soft" disabled={!send} onClick={() => put(false)}>
              Put on the Amiga clipboard
            </Button>
            <Button size="1" disabled={!send} onClick={() => put(true)}>
              …and paste it (Right Amiga+V)
            </Button>
          </Flex>
          {note && <Text size="1" color="gray">{note}</Text>}
        </Flex>
        <Flex justify="end" mt="4">
          <Dialog.Close><Button variant="soft" color="gray">Close</Button></Dialog.Close>
        </Flex>
      </Dialog.Content>
    </Dialog.Root>
  );
}
