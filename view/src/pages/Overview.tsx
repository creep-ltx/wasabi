import { useEffect, useState, type ReactNode } from 'react';
import {
  Badge,
  Box,
  Callout,
  Card,
  DataList,
  Flex,
  Grid,
  Heading,
  Progress,
  Text,
} from '@radix-ui/themes';
import {
  CheckCircledIcon,
  CrossCircledIcon,
  ExclamationTriangleIcon,
} from '@radix-ui/react-icons';
import { api, size, type Health, type Info } from '../api';
import { Sparkline } from '../ui/Sparkline';

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

export function OverviewPage() {
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
      <Heading size="6" mb="1">Overview</Heading>
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
              <Sparkline values={temps} every={EVERY} unit="°C" label="Temperature" minSpan={10} />
              {h.temp_max_c !== undefined && (
                <Text as="div" size="1" color="gray" mt="1">The Pi slows down at {h.temp_max_c.toFixed(0)} °C</Text>
              )}
            </Tile>
            <Tile label="68k CPU" value={h.mips_68k?.toFixed(1) ?? '…'} unit="MIPS">
              <Sparkline values={mips} every={EVERY} unit="MIPS" label="68k CPU" minSpan={100} floor={0} />
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
        </>
      )}
    </Box>
  );
}
