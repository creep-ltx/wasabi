import { useEffect, useState, type ReactNode } from 'react';
import {
  Badge,
  Box,
  Button,
  Callout,
  Code,
  Card,
  DataList,
  Dialog,
  Flex,
  Grid,
  Heading,
  Progress,
  SegmentedControl,
  Switch,
  Text,
  TextField,
} from '@radix-ui/themes';
import {
  CheckCircledIcon,
  CrossCircledIcon,
  ExclamationTriangleIcon,
} from '@radix-ui/react-icons';
import { api, size, type Health, type Info, type Me } from '../api';
import { Sparkline } from '../ui/Sparkline';
import { LogoutButton, RebootButton } from '../ui/Machine';
import { MachinePicker } from '../ui/Machines';

const EVERY = 2;          // seconds between health readings
const KEEP = 90;          // readings kept for the sparklines: 3 minutes
// Network volumes report 1048575 MB total and free: no real numbers.
const FAKE_MB = 1048575;

function uptime(s: number): string {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  return d ? `${d} d ${h} h` : h ? `${h} h ${m} min` : `${m} min`;
}

/* A stat tile: label, value, optional trend - the figure contract. */
function Tile({ label, value, unit, children }: {
  label: string; value: string; unit?: string; children?: ReactNode;
}) {
  return (
    <Card size="2">
      <Text as="div" size="2" color="gray">{label}</Text>
      <Flex align="baseline" gap="1" mt="1">
        <Text size="7" weight="medium">{value}</Text>
        {unit && <Text size="3" color="gray">{unit}</Text>}
      </Flex>
      {children}
    </Card>
  );
}

/* The power line: status colour always with an icon and words. */
function Power({ h }: { h: Health }) {
  if (!h.problems_now) {
    return (
      <Tile label="Power" value="—">
        <Text as="div" size="2" color="gray" mt="2">Not available (not a Pi)</Text>
      </Tile>
    );
  }
  const now = h.problems_now;
  const ever = h.problems_since_boot ?? [];
  return (
    <Card size="2">
      <Text as="div" size="2" color="gray">Power and heat</Text>
      <Flex direction="column" gap="2" mt="2">
        {now.length ? (
          <Badge size="2" color="red"><CrossCircledIcon /> Now: {now.join(', ')}</Badge>
        ) : ever.length ? (
          <Badge size="2" color="amber"><ExclamationTriangleIcon /> Earlier: {ever.join(', ')}</Badge>
        ) : (
          <Badge size="2" color="green"><CheckCircledIcon /> OK since the Pi started</Badge>
        )}
        <Text size="1" color="gray">
          The Pi remembers any dip in power or overheating since it started,
          even one too short to see here.
        </Text>
      </Flex>
    </Card>
  );
}

function Meter({ label, free, total, note }: {
  label: string; free: number; total: number; note?: string;
}) {
  const used = total ? Math.round(((total - free) / total) * 100) : 0;
  return (
    <Box>
      <Flex justify="between" mb="1">
        <Text size="2">{label}</Text>
        <Text size="2" color="gray">{size(free)} free of {size(total)}</Text>
      </Flex>
      <Progress value={used} size="2" color={used > 90 ? 'red' : used > 75 ? 'amber' : 'blue'}
        aria-label={`${label}: ${used}% used`} />
      {note && <Text as="div" size="1" color="gray" mt="1">{note}</Text>}
    </Box>
  );
}

type Range = 'live' | '1' | '24';
type History = { every: number; readings: [number, number, number | null, number | null][] };

export function OverviewPage({ login = false, onLogout, me }: {
  login?: boolean; onLogout?: () => void; me?: Me | null;
}) {
  const [range, setRange] = useState<Range>('live');
  const [hist, setHist] = useState<History | null>(null);
  // The NAS keeps a day of readings; the PC app has only what this page
  // has seen, so the switch shows only where there is history.
  useEffect(() => {
    if (range === 'live') return;
    let stop = false;
    const load = () => api<History>(`api/history?hours=${range}`)
      .then((h) => { if (!stop) setHist(h); }).catch(() => {});
    void load();
    const t = window.setInterval(load, 30000);
    return () => { stop = true; window.clearInterval(t); };
  }, [range]);
  const histTemps = (hist?.readings ?? []).map((r) => r[2]).filter((v): v is number => v !== null);
  const histMips = (hist?.readings ?? []).map((r) => r[3]).filter((v): v is number => v !== null);
  const live = range === 'live' || !hist;
  const [h, setH] = useState<Health | null>(null);
  const [info, setInfo] = useState<Info | null>(null);
  const [error, setError] = useState('');
  const [temps, setTemps] = useState<number[]>([]);
  const [mips, setMips] = useState<number[]>([]);

  useEffect(() => {
    let stop = false;
    const tick = async () => {
      try {
        const r = await api<Health>('api/health');
        if (stop) return;
        const t = r.temp_c;
        const m = r.mips_68k;
        if (t !== undefined) setTemps((a) => [...a, t].slice(-KEEP));
        if (m !== undefined) setMips((a) => [...a, m].slice(-KEEP));
        setH(r);
        setError('');
      } catch (e) {
        if (!stop) setError((e as Error).message);
      }
    };
    void tick();
    const t = window.setInterval(tick, EVERY * 1000);
    return () => { stop = true; window.clearInterval(t); };
  }, []);

  useEffect(() => {
    let stop = false;
    const load = () => api<Info>('api/info')
      .then((r) => { if (!stop) setInfo(r); })
      .catch(() => {});
    void load();
    const t = window.setInterval(load, 30000);
    return () => { stop = true; window.clearInterval(t); };
  }, []);

  return (
    <Box className="wv-page-body">
      <Flex justify="between" align="center" mb="1" gap="3" wrap="wrap">
        <Flex gap="3" align="center">
          <Heading size="6">Overview</Heading>
          {/* the sidebar's picker is hidden on a phone */}
          <Box display={{ initial: 'block', md: 'none' }}><MachinePicker /></Box>
        </Flex>
        {me?.history && (
          <SegmentedControl.Root size="1" value={range} onValueChange={(v) => setRange(v as Range)}>
            <SegmentedControl.Item value="live">Live</SegmentedControl.Item>
            <SegmentedControl.Item value="1">Last hour</SegmentedControl.Item>
            <SegmentedControl.Item value="24">Last day</SegmentedControl.Item>
          </SegmentedControl.Root>
        )}
      </Flex>
      <Text as="p" size="2" color="gray" mb="4">
        {info ? `${info.system ?? 'AmigaOS'} · ${info.banner} · ${info.host}` : 'Reading the Amiga…'}
      </Text>
      {error && (
        <Callout.Root color="red" mb="4">
          <Callout.Icon><CrossCircledIcon /></Callout.Icon>
          <Callout.Text>{error}</Callout.Text>
        </Callout.Root>
      )}
      {h && (
        <>
          <Box className="wv-tiles" mb="3">
            <Tile label="Temperature" value={h.temp_c?.toFixed(1) ?? '—'} unit="°C">
              <Sparkline values={live ? temps : histTemps} every={live ? EVERY : hist.every}
                unit="°C" label="Temperature" minSpan={10} />
              {h.temp_max_c !== undefined && (
                <Text as="div" size="1" color="gray" mt="1">The Pi slows down at {h.temp_max_c.toFixed(0)} °C</Text>
              )}
            </Tile>
            <Tile label="68k CPU" value={h.mips_68k?.toFixed(1) ?? '…'} unit="MIPS">
              <Sparkline values={live ? mips : histMips} every={live ? EVERY : hist.every}
                unit="MIPS" label="68k CPU" minSpan={100} floor={0} />
              <Text as="div" size="1" color="gray" mt="1">Near 0 when the Amiga is idle</Text>
            </Tile>
            <Power h={h} />
            <Tile label="Pi up" value={h.uptime_s !== undefined ? uptime(h.uptime_s) : '—'}>
              <Text as="div" size="1" color="gray" mt="2">
                Since the Pi started; an Amiga reboot does not reset it
              </Text>
            </Tile>
          </Box>

          <Grid columns={{ initial: '1', md: '2' }} gap="3" mb="3" align="start">
            <Card size="2">
              <Heading size="3" mb="3">Memory</Heading>
              <Flex direction="column" gap="4">
                {h.chip_total_kb !== undefined && (
                  <Meter label="Chip" free={h.chip_free_kb ?? 0} total={h.chip_total_kb}
                    note={`Largest free block ${size(h.chip_largest_kb ?? 0)}`} />
                )}
                {h.fast_total_kb !== undefined && (
                  <Meter label="Fast" free={h.fast_free_kb ?? 0} total={h.fast_total_kb}
                    note={`Largest free block ${size(h.fast_largest_kb ?? 0)}`} />
                )}
              </Flex>
            </Card>
            <Card size="2">
              <Heading size="3" mb="3">Disks</Heading>
              <Flex direction="column" gap="4">
                {(info?.volumes ?? []).map((v) => v.total_mb >= FAKE_MB ? (
                  <Flex key={v.name} justify="between">
                    <Text size="2">{v.name}</Text>
                    <Text size="2" color="gray">network volume</Text>
                  </Flex>
                ) : (
                  <Meter key={v.name} label={v.name} free={v.free_mb * 1024} total={v.total_mb * 1024} />
                ))}
                {!info && <Text size="2" color="gray">Reading the volumes…</Text>}
              </Flex>
            </Card>
          </Grid>

          <Card size="2">
            <Heading size="3" mb="3">System</Heading>
            <DataList.Root size="2">
              <DataList.Item>
                <DataList.Label>Wasabi daemon</DataList.Label>
                <DataList.Value>{h.banner}</DataList.Value>
              </DataList.Item>
              <DataList.Item>
                <DataList.Label>AmigaOS</DataList.Label>
                <DataList.Value>{info?.system ?? '…'}{info?.exec ? ` · exec ${info.exec}` : ''}</DataList.Value>
              </DataList.Item>
              {h.emu68 && (
                <DataList.Item>
                  <DataList.Label>Emu68</DataList.Label>
                  <DataList.Value>{h.emu68_version}</DataList.Value>
                </DataList.Item>
              )}
              {h.pi_model && (
                <DataList.Item>
                  <DataList.Label>Board</DataList.Label>
                  <DataList.Value>{h.pi_model}</DataList.Value>
                </DataList.Item>
              )}
              {h.arm_mhz !== undefined && (
                <DataList.Item>
                  <DataList.Label>ARM clock</DataList.Label>
                  <DataList.Value>
                    {h.arm_mhz} MHz{h.core_v !== undefined ? ` · core ${h.core_v.toFixed(2)} V` : ''}
                  </DataList.Value>
                </DataList.Item>
              )}
              {h.jit_used_pct !== undefined && (
                <DataList.Item>
                  <DataList.Label>JIT cache</DataList.Label>
                  <DataList.Value>{h.jit_used_pct.toFixed(0)}% used · {h.jit_misses} misses</DataList.Value>
                </DataList.Item>
              )}
              <DataList.Item>
                <DataList.Label>Last guru</DataList.Label>
                <DataList.Value>{h.last_guru ?? 'none'}</DataList.Value>
              </DataList.Item>
            </DataList.Root>
          </Card>
          {me?.history && <AlertsCard />}
          {/* On a phone the sidebar is hidden: its buttons live here. */}
          <Flex gap="3" mt="4" display={{ initial: 'flex', md: 'none' }}>
            <RebootButton />
            {login && onLogout && <LogoutButton onDone={onLogout} />}
          </Flex>
        </>
      )}
    </Box>
  );
}

/* --- alerts (Wasabi phone: the NAS watches and tells the phone) ------- */

type AlertSettings = { enabled: boolean; ntfy_url: string; phone_url: string; topic: string; temp_c: number };
type AlertEvent = { ts: number; kind: string; text: string };

function AlertsCard() {
  const [cfg, setCfg] = useState<AlertSettings | null>(null);
  const [events, setEvents] = useState<AlertEvent[]>([]);
  const [tick, setTick] = useState(0);
  const [note, setNote] = useState('');
  useEffect(() => {
    let stop = false;
    api<{ settings: AlertSettings; events: AlertEvent[] }>('api/alerts')
      .then((r) => { if (!stop) { setCfg(r.settings); setEvents(r.events); } })
      .catch(() => {});
    return () => { stop = true; };
  }, [tick]);
  useEffect(() => {
    const t = window.setInterval(() => setTick((n) => n + 1), 30000);
    return () => window.clearInterval(t);
  }, []);
  const save = (next: Partial<AlertSettings>) => api<AlertSettings>('api/alerts', next)
    .then(setCfg).catch((e: Error) => setNote(e.message));
  if (!cfg) return null;
  const feed = `${cfg.phone_url.replace(/\/$/, '')}/${cfg.topic}`;
  return (
    <Card size="2" mt="3">
      <Flex justify="between" align="center" mb="3" gap="3">
        <Heading size="3">Alerts</Heading>
        <Flex gap="2" align="center">
          <Badge color={cfg.enabled ? 'green' : 'gray'}>{cfg.enabled ? 'on' : 'off'}</Badge>
          <Dialog.Root>
            <Dialog.Trigger><Button size="1" variant="soft">Settings</Button></Dialog.Trigger>
            <Dialog.Content maxWidth="var(--wv-dialog-wide)">
              <Dialog.Title>Alerts</Dialog.Title>
              <Dialog.Description size="2" color="gray" mb="3">
                The NAS checks the Amiga every 30 seconds and tells your phone when it gets too
                hot, has a power or heat problem, stops answering (and comes back), or shows a guru.
              </Dialog.Description>
              <Flex direction="column" gap="3">
                <Text as="label" size="2">
                  <Flex gap="2" align="center">
                    <Switch checked={cfg.enabled} onCheckedChange={(v) => void save({ enabled: v })} />
                    Send alerts to the phone
                  </Flex>
                </Text>
                <Flex gap="2" align="center">
                  <Text size="2">Too hot at</Text>
                  <TextField.Root size="1" type="number" defaultValue={String(cfg.temp_c)}
                    onBlur={(e) => void save({ temp_c: Number(e.target.value) || 75 })} />
                  <Text size="2">°C</Text>
                </Flex>
                <Box>
                  <Text as="div" size="2" weight="medium" mb="1">On your phone</Text>
                  <Text as="div" size="2" color="gray">
                    Install the free <Text weight="medium">ntfy</Text> app, tap +, turn on
                    "Use another server", and enter:
                  </Text>
                  <Text as="div" size="2" mt="1">Server: <Code>{cfg.phone_url}</Code></Text>
                  <Text as="div" size="2">Topic: <Code>{cfg.topic}</Code></Text>
                  <Text as="div" size="1" color="gray" mt="1">
                    The topic is the feed's only key, so it is random. Full address: {feed}
                  </Text>
                </Box>
                <Flex gap="2">
                  <Button size="1" variant="soft" onClick={() => api<{ sent: boolean }>('api/alerts/test', {})
                    .then((r) => setNote(r.sent ? 'Test sent - check your phone' :
                      'Could not reach ntfy - is its container running on the NAS?'))
                    .catch((e: Error) => setNote(e.message))}>
                    Send a test alert
                  </Button>
                </Flex>
                {note && <Text size="1" color="gray">{note}</Text>}
              </Flex>
              <Flex justify="end" mt="4">
                <Dialog.Close><Button variant="soft" color="gray">Close</Button></Dialog.Close>
              </Flex>
            </Dialog.Content>
          </Dialog.Root>
        </Flex>
      </Flex>
      {events.length ? (
        <Flex direction="column" gap="2">
          {events.slice(0, 8).map((e) => (
            <Flex key={e.ts} gap="3">
              <Text size="1" color="gray" className="wv-nowrap">
                {new Date(e.ts * 1000).toLocaleString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' })}
              </Text>
              <Text size="2">{e.text}</Text>
            </Flex>
          ))}
        </Flex>
      ) : (
        <Text size="2" color="gray">Nothing to report.</Text>
      )}
    </Card>
  );
}
