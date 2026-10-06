import { useEffect, useRef, useState, type CSSProperties } from 'react';
import {
  Badge,
  Box,
  Button,
  Dialog,
  Flex,
  Select,
  Text,
} from '@radix-ui/themes';
import { AmigaLink, type LinkStatus } from './amiga/link';
import {
  AMIGA_MODIFIERS,
  DEFAULT_KEYS,
  PC_MODIFIER_KEYS,
  assignKey,
  type AmigaKeySettings,
  type AmigaModifier,
} from './amiga/keys';

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
  void fetch('api/settings', {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(s),
  });
}

const NO_STATUS: LinkStatus = {
  connected: false,
  banner: '',
  error: '',
  width: 0,
  height: 0,
  fps: 0,
};

export function App() {
  const stageRef = useRef<HTMLDivElement>(null);
  const linkRef = useRef<AmigaLink | null>(null);
  const [settings, setSettings] = useState<Settings | null>(null);
  const [status, setStatus] = useState<LinkStatus>(NO_STATUS);
  const [stage, setStage] = useState({ w: 0, h: 0 });

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
    <Flex direction="column" className="wv-page">
      <Flex className="wv-bar" align="center" gap="3">
        <Badge color={status.connected ? 'green' : 'red'}>
          {status.connected ? 'connected' : 'not connected'}
        </Badge>
        <Text size="2" color="gray">
          {status.connected
            ? `${status.banner} · ${status.width}×${status.height} · ${status.fps} fps`
            : status.error || 'looking for the Amiga…'}
        </Text>
        <Box flexGrow="1" />
        <Text size="1" color="gray">
          Click the screen to use the Amiga's mouse and keyboard
        </Text>
        <Button
          size="1"
          variant="soft"
          onClick={() => {
            void document.documentElement.requestFullscreen?.();
            linkRef.current?.focus();
          }}
        >
          Full screen
        </Button>
        {settings && (
          <SettingsDialog settings={settings} onChange={update} />
        )}
      </Flex>
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
          Settings
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
