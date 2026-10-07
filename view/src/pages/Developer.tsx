import { useEffect, useMemo, useRef, useState } from 'react';
import {
  AlertDialog,
  Badge,
  Box,
  Button,
  Callout,
  Card,
  Checkbox,
  Code,
  Flex,
  Heading,
  IconButton,
  Select,
  Spinner,
  Table,
  Tabs,
  Text,
  TextField,
  Tooltip,
} from '@radix-ui/themes';
import {
  CrossCircledIcon,
  DownloadIcon,
  PauseIcon,
  PlayIcon,
  ReloadIcon,
  StopIcon,
  TrashIcon,
} from '@radix-ui/react-icons';
import { api, socketUrl } from '../api';

/* --- shared: a WebSocket to the bridge ------------------------------- */


/* --- Logs: the debug (serial) and snoop (DOS calls) streams ---------- */

type Line = { s: 'debug' | 'snoop'; ts: number; text: string; self: boolean; noise: boolean };
type StreamState = { state: string; msg?: string };
const MAX_LINES = 5000;
const SHOWN = 1500;

function stamp(ts: number) {
  const d = new Date(ts * 1000);
  return d.toTimeString().slice(0, 8) + '.' + String(d.getMilliseconds()).padStart(3, '0');
}

function stateBadge(st: StreamState | undefined) {
  const s = st?.state ?? 'off';
  const color = s === 'on' ? 'green' : s === 'off' ? 'gray' : s === 'busy' ? 'amber' : 'red';
  const word = { on: 'on', off: 'off', busy: 'in use elsewhere', waiting: 'reconnecting', error: 'error' }[s] ?? s;
  return <Badge color={color}>{word}</Badge>;
}

function LogsTab() {
  const ws = useRef<WebSocket | null>(null);
  const [lines, setLines] = useState<Line[]>([]);
  const [states, setStates] = useState<Record<string, StreamState>>({});
  const [want, setWant] = useState({ debug: true, snoop: false });
  const [hideSelf, setHideSelf] = useState(true);
  const [hideNoise, setHideNoise] = useState(true);
  const [find, setFind] = useState('');
  const [paused, setPaused] = useState(false);
  const [task, setTask] = useState('');
  const [entry, setEntry] = useState(false);
  const pending = useRef<Line[]>([]);
  const pausedRef = useRef(false);
  const viewport = useRef<HTMLDivElement>(null);
  const stick = useRef(true);

  useEffect(() => { pausedRef.current = paused; }, [paused]);

  // One socket for the tab's life; lines are gathered and drawn five
  // times a second - a busy snoop can send hundreds a second.
  useEffect(() => {
    const sock = new WebSocket(socketUrl('ws/logs'));
    ws.current = sock;
    sock.onmessage = (ev) => {
      const m = JSON.parse(ev.data as string);
      if (m.t === 'line') pending.current.push(m as Line);
      else if (m.t === 'backlog') pending.current.push(...(m.lines as Line[]));
      else if (m.t === 'state') setStates((s) => ({ ...s, [m.stream]: { state: m.state, msg: m.msg } }));
    };
    sock.onopen = () => {
      sock.send(JSON.stringify({ t: 'start', stream: 'debug' }));
    };
    const t = window.setInterval(() => {
      if (!pending.current.length || pausedRef.current) return;
      const add = pending.current;
      pending.current = [];
      setLines((l) => [...l, ...add].slice(-MAX_LINES));
    }, 200);
    return () => { window.clearInterval(t); sock.close(); };
  }, []);

  const toggle = (stream: 'debug' | 'snoop', on: boolean) => {
    setWant((w) => ({ ...w, [stream]: on }));
    ws.current?.send(JSON.stringify({ t: on ? 'start' : 'stop', stream }));
  };
  const applySnoop = () => ws.current?.send(JSON.stringify({ t: 'snoop', task, entry }));

  const shown = useMemo(() => {
    const f = find.toLowerCase();
    return lines.filter((l) => (want[l.s]) && !(hideSelf && l.self) && !(hideNoise && l.noise) &&
      (!f || l.text.toLowerCase().includes(f))).slice(-SHOWN);
  }, [lines, want, hideSelf, hideNoise, find]);

  // Keep to the bottom unless the reader has scrolled up to look.
  useEffect(() => {
    const v = viewport.current;
    if (v && stick.current) v.scrollTop = v.scrollHeight;
  }, [shown]);

  const download = () => {
    const text = shown.map((l) => `${stamp(l.ts)} ${l.s} | ${l.text}`).join('\n') + '\n';
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
    a.download = `wasabi-log-${new Date().toISOString().slice(0, 19).replace(/:/g, '')}.txt`;
    a.click();
    URL.revokeObjectURL(a.href);
  };

  return (
    <Flex direction="column" gap="3">
      <Flex gap="4" wrap="wrap" align="center">
        <Text as="label" size="2">
          <Flex gap="2" align="center">
            <Checkbox checked={want.debug} onCheckedChange={(v) => toggle('debug', v === true)} />
            Debug output (serial / KPrintF) {stateBadge(states.debug)}
          </Flex>
        </Text>
        <Text as="label" size="2">
          <Flex gap="2" align="center">
            <Checkbox checked={want.snoop} onCheckedChange={(v) => toggle('snoop', v === true)} />
            DOS calls (snoop) {stateBadge(states.snoop)}
          </Flex>
        </Text>
      </Flex>
      {[states.debug, states.snoop].filter((s) => s?.msg && s.state !== 'on').map((s) => (
        <Callout.Root key={s?.msg} color="amber" size="1">
          <Callout.Text>{s?.msg}</Callout.Text>
        </Callout.Root>
      ))}
      {want.snoop && (
        <Flex gap="2" align="center" wrap="wrap">
          <TextField.Root size="1" placeholder="Only tasks matching (AmigaDOS pattern, e.g. myprog#?)"
            value={task} onChange={(e) => setTask(e.target.value)}
            onKeyDown={(e) => { if (e.key === 'Enter') applySnoop(); }} className="wv-grow" />
          <Text as="label" size="1">
            <Flex gap="1" align="center">
              <Checkbox size="1" checked={entry} onCheckedChange={(v) => setEntry(v === true)} />
              log calls on the way in too
            </Flex>
          </Text>
          <Button size="1" variant="soft" onClick={applySnoop}>Apply</Button>
        </Flex>
      )}
      <Flex gap="3" align="center" wrap="wrap">
        <TextField.Root size="1" placeholder="Find in the log" value={find}
          onChange={(e) => setFind(e.target.value)} />
        <Text as="label" size="1">
          <Flex gap="1" align="center">
            <Checkbox size="1" checked={hideSelf} onCheckedChange={(v) => setHideSelf(v === true)} />
            hide Wasabi's own traffic
          </Flex>
        </Text>
        <Text as="label" size="1">
          <Flex gap="1" align="center">
            <Checkbox size="1" checked={hideNoise} onCheckedChange={(v) => setHideNoise(v === true)} />
            hide routine noise
          </Flex>
        </Text>
        <Box flexGrow="1" />
        <Text size="1" color="gray">{shown.length} lines</Text>
        <Tooltip content={paused ? 'Carry on' : 'Pause (keeps collecting)'}>
          <IconButton size="1" variant="soft" aria-label={paused ? 'Carry on' : 'Pause'}
            onClick={() => setPaused((p) => !p)}>
            {paused ? <PlayIcon /> : <PauseIcon />}
          </IconButton>
        </Tooltip>
        <Tooltip content="Clear">
          <IconButton size="1" variant="soft" color="gray" aria-label="Clear" onClick={() => setLines([])}>
            <TrashIcon />
          </IconButton>
        </Tooltip>
        <Tooltip content="Save what is shown as a text file">
          <IconButton size="1" variant="soft" color="gray" aria-label="Download log" onClick={download}>
            <DownloadIcon />
          </IconButton>
        </Tooltip>
      </Flex>
      <Card size="1">
        <Box className="wv-log" ref={viewport}
          onScroll={(e) => {
            const v = e.currentTarget;
            stick.current = v.scrollHeight - v.scrollTop - v.clientHeight < 40;
          }}>
          {shown.map((l, i) => (
            <Box key={i} className="wv-log-line">
              <Text color="gray">{stamp(l.ts)} </Text>
              <Text color={l.s === 'debug' ? 'blue' : 'violet'}>{l.s} </Text>
              <Text>{l.text}</Text>
            </Box>
          ))}
          {!shown.length && (
            <Text size="2" color="gray">
              Nothing yet. Debug output appears when an Amiga program prints with KPrintF;
              DOS calls appear as programs open, read and lock files.
            </Text>
          )}
        </Box>
      </Card>
    </Flex>
  );
}

/* --- Tasks ------------------------------------------------------------ */

type Task = {
  addr: string; kind: string; pri: number; state: string; stack: number;
  free: number | null; cli: number | null; name: string; command: string; tight: boolean;
};

function TasksTab() {
  const [tasks, setTasks] = useState<Task[]>([]);
  const [error, setError] = useState('');
  const [find, setFind] = useState('');
  const [tick, setTick] = useState(0);
  const [remove, setRemove] = useState<Task | null>(null);
  const [note, setNote] = useState('');

  useEffect(() => {
    let stop = false;
    api<{ tasks: Task[] }>('api/ps')
      .then((r) => { if (!stop) { setTasks(r.tasks); setError(''); } })
      .catch((e: Error) => { if (!stop) setError(e.message); });
    return () => { stop = true; };
  }, [tick]);
  useEffect(() => {
    const t = window.setInterval(() => setTick((n) => n + 1), 3000);
    return () => window.clearInterval(t);
  }, []);

  const f = find.toLowerCase();
  const shown = tasks.filter((t) => !f || t.name.toLowerCase().includes(f) || t.command.toLowerCase().includes(f));
  const kill = (t: Task, force: boolean) => api('api/kill', { addr: t.addr, force })
    .then(() => { setNote(force ? `Removed ${t.command || t.name}` : `Sent Ctrl-C to ${t.command || t.name}`); setTick((n) => n + 1); })
    .catch((e: Error) => setNote(e.message));

  return (
    <Flex direction="column" gap="3">
      <Flex gap="3" align="center">
        <TextField.Root size="1" placeholder="Find a task" value={find} onChange={(e) => setFind(e.target.value)} />
        <Text size="1" color="gray">{shown.length} of {tasks.length} · refreshes every 3 s</Text>
        <Box flexGrow="1" />
        {note && <Text size="1" color="gray">{note}</Text>}
      </Flex>
      {error && (
        <Callout.Root color="red" size="1">
          <Callout.Icon><CrossCircledIcon /></Callout.Icon>
          <Callout.Text>{error}</Callout.Text>
        </Callout.Root>
      )}
      <Card size="1">
        <Box className="wv-tasks">
          <Table.Root size="1">
            <Table.Header>
              <Table.Row>
                <Table.ColumnHeaderCell>Name</Table.ColumnHeaderCell>
                <Table.ColumnHeaderCell>CLI</Table.ColumnHeaderCell>
                <Table.ColumnHeaderCell>Pri</Table.ColumnHeaderCell>
                <Table.ColumnHeaderCell>State</Table.ColumnHeaderCell>
                <Table.ColumnHeaderCell justify="end">Stack free</Table.ColumnHeaderCell>
                <Table.ColumnHeaderCell />
              </Table.Row>
            </Table.Header>
            <Table.Body>
              {shown.map((t) => (
                <Table.Row key={t.addr}>
                  <Table.Cell>
                    <Text size="2">{t.command && t.command !== t.name ? t.command : t.name}</Text>
                    {t.command && t.command !== t.name && <Text size="1" color="gray"> ({t.name})</Text>}
                    <Text as="div" size="1" color="gray">{t.kind} · {t.addr}</Text>
                  </Table.Cell>
                  <Table.Cell><Text size="2">{t.cli ?? '—'}</Text></Table.Cell>
                  <Table.Cell><Text size="2">{t.pri}</Text></Table.Cell>
                  <Table.Cell><Text size="2">{t.state}</Text></Table.Cell>
                  <Table.Cell justify="end" className="wv-nowrap">
                    {t.free === null ? <Text size="2" color="gray">—</Text> : (
                      <Text size="2">
                        {t.tight && <Badge color="amber" mr="1">low</Badge>}
                        {t.free} / {t.stack}
                      </Text>
                    )}
                  </Table.Cell>
                  <Table.Cell justify="end" className="wv-nowrap">
                    <Tooltip content="Ask it to stop (Ctrl-C)">
                      <Button size="1" variant="soft" color="gray" onClick={() => void kill(t, false)}>Ctrl-C</Button>
                    </Tooltip>{' '}
                    <Tooltip content="Remove it by force - can crash the Amiga">
                      <Button size="1" variant="soft" color="red" onClick={() => setRemove(t)}>Remove</Button>
                    </Tooltip>
                  </Table.Cell>
                </Table.Row>
              ))}
            </Table.Body>
          </Table.Root>
        </Box>
      </Card>
      <AlertDialog.Root open={!!remove} onOpenChange={(o) => { if (!o) setRemove(null); }}>
        <AlertDialog.Content maxWidth="var(--wv-dialog-width)">
          <AlertDialog.Title>Remove {remove?.command || remove?.name}?</AlertDialog.Title>
          <AlertDialog.Description size="2">
            This pulls the task out by force (RemTask). Its memory and files stay open, and the
            Amiga may crash. Try Ctrl-C first.
          </AlertDialog.Description>
          <Flex gap="3" mt="4" justify="end">
            <AlertDialog.Cancel><Button variant="soft" color="gray">Cancel</Button></AlertDialog.Cancel>
            <AlertDialog.Action>
              <Button color="red" onClick={() => { const t = remove; setRemove(null); if (t) void kill(t, true); }}>
                Remove it
              </Button>
            </AlertDialog.Action>
          </Flex>
        </AlertDialog.Content>
      </AlertDialog.Root>
    </Flex>
  );
}

/* --- Run --------------------------------------------------------------- */

function RunTab() {
  const ws = useRef<WebSocket | null>(null);
  const [cmd, setCmd] = useState('');
  const [max, setMax] = useState('30');
  const [out, setOut] = useState('');
  const [running, setRunning] = useState(false);
  const [result, setResult] = useState<{ rc?: number; msg?: string } | null>(null);
  const [history, setHistory] = useState<string[]>(() => {
    try { return JSON.parse(localStorage.getItem('wasabi-run-history') ?? '[]') as string[]; }
    catch { return []; }
  });

  useEffect(() => {
    const sock = new WebSocket(socketUrl('ws/run'));
    ws.current = sock;
    sock.onmessage = (ev) => {
      const m = JSON.parse(ev.data as string);
      if (m.t === 'out') setOut((o) => (o + m.text).slice(-200000));
      else if (m.t === 'exit') { setRunning(false); setResult({ rc: m.rc }); }
      else if (m.t === 'error') { setRunning(false); setResult({ msg: m.msg }); }
    };
    return () => sock.close();
  }, []);

  const run = () => {
    const c = cmd.trim();
    if (!c || running || !ws.current) return;
    setOut('');
    setResult(null);
    setRunning(true);
    ws.current.send(JSON.stringify({ t: 'run', cmd: c, max: max === 'none' ? null : Number(max) }));
    const h = [c, ...history.filter((x) => x !== c)].slice(0, 20);
    setHistory(h);
    localStorage.setItem('wasabi-run-history', JSON.stringify(h));
  };

  const rc = result?.rc;
  return (
    <Flex direction="column" gap="3">
      <Flex gap="2" align="center" wrap="wrap">
        <TextField.Root className="wv-grow" placeholder='An AmigaDOS command, e.g. Version FULL or List SYS:C'
          value={cmd} onChange={(e) => setCmd(e.target.value)}
          onKeyDown={(e) => { if (e.key === 'Enter') run(); }} />
        <Select.Root value={max} onValueChange={setMax}>
          <Select.Trigger aria-label="Time limit" />
          <Select.Content>
            <Select.Item value="10">stop after 10 s</Select.Item>
            <Select.Item value="30">stop after 30 s</Select.Item>
            <Select.Item value="120">stop after 2 min</Select.Item>
            <Select.Item value="600">stop after 10 min</Select.Item>
          </Select.Content>
        </Select.Root>
        {running ? (
          <Button color="red" variant="soft" onClick={() => ws.current?.send(JSON.stringify({ t: 'stop' }))}>
            <StopIcon /> Stop (Ctrl-C)
          </Button>
        ) : (
          <Button onClick={run} disabled={!cmd.trim()}><PlayIcon /> Run</Button>
        )}
      </Flex>
      <Text size="1" color="gray">
        Runs in a Shell on the Amiga, as if typed there. Commands are not checked: a Delete in
        C: or LIBS: does exactly what it says.
      </Text>
      {history.length > 0 && (
        <Flex gap="1" wrap="wrap">
          {history.slice(0, 8).map((h) => (
            <Button key={h} size="1" variant="ghost" color="gray" onClick={() => setCmd(h)}>{h}</Button>
          ))}
        </Flex>
      )}
      <Card size="1">
        <Flex justify="between" align="center" mb="2">
          <Text size="2" color="gray">Output</Text>
          {running && <Flex gap="2" align="center"><Spinner /><Text size="1" color="gray">running…</Text></Flex>}
          {rc !== undefined && (
            <Badge color={rc === 0 ? 'green' : rc < 10 ? 'amber' : 'red'}>
              returned {rc}{rc === 0 ? ' (OK)' : rc === 5 ? ' (WARN)' : rc === 10 ? ' (ERROR)' : rc >= 20 ? ' (FAIL)' : ''}
            </Badge>
          )}
          {result?.msg && <Badge color="red">{result.msg}</Badge>}
        </Flex>
        <Box className="wv-log">
          <Code variant="ghost" className="wv-pre">{out || (running ? '' : 'Nothing run yet.')}</Code>
        </Box>
      </Card>
    </Flex>
  );
}

/* --- Screens ------------------------------------------------------------ */

type Win = { title: string; left: number; top: number; width: number; height: number; task: string; active: boolean; backdrop: boolean };
type Scr = { title: string; width: number; height: number; depth: number; front: boolean; windows: Win[] };

function ScreensTab() {
  const [screens, setScreens] = useState<Scr[]>([]);
  const [tick, setTick] = useState(0);
  const [error, setError] = useState('');
  useEffect(() => {
    let stop = false;
    api<{ screens: Scr[] }>('api/screens')
      .then((r) => { if (!stop) { setScreens(r.screens); setError(''); } })
      .catch((e: Error) => { if (!stop) setError(e.message); });
    return () => { stop = true; };
  }, [tick]);
  return (
    <Flex direction="column" gap="3">
      <Flex gap="2" align="center">
        <Text size="2" color="gray">Every screen, front first, with its windows.</Text>
        <Box flexGrow="1" />
        <IconButton size="1" variant="soft" aria-label="Refresh" onClick={() => setTick((n) => n + 1)}>
          <ReloadIcon />
        </IconButton>
      </Flex>
      {error && <Text size="2" color="red">{error}</Text>}
      {screens.map((s) => (
        <Card key={s.title + s.width} size="2">
          <Flex justify="between" align="center" mb="2" gap="3">
            <Box>
              <Heading size="3">{s.title || '(untitled screen)'}</Heading>
              <Text size="1" color="gray">{s.width}×{s.height}, {s.depth} bit</Text>
            </Box>
            {s.front ? <Badge color="green">in front</Badge> : (
              <Button size="1" variant="soft" onClick={() => api('api/screen/front', { title: s.title })
                .then(() => setTick((n) => n + 1)).catch((e: Error) => setError(e.message))}>
                Bring to front
              </Button>
            )}
          </Flex>
          <Table.Root size="1">
            <Table.Body>
              {s.windows.map((w, i) => (
                <Table.Row key={i}>
                  <Table.Cell><Text size="2">{w.title || '(no title)'}</Text></Table.Cell>
                  <Table.Cell><Text size="1" color="gray">{w.task}</Text></Table.Cell>
                  <Table.Cell className="wv-nowrap"><Text size="1" color="gray">at {w.left},{w.top} · {w.width}×{w.height}</Text></Table.Cell>
                  <Table.Cell>
                    {w.active && <Badge color="blue">active</Badge>}
                    {w.backdrop && <Badge color="gray">backdrop</Badge>}
                  </Table.Cell>
                </Table.Row>
              ))}
            </Table.Body>
          </Table.Root>
        </Card>
      ))}
    </Flex>
  );
}

/* --- the page ------------------------------------------------------------ */

export function DeveloperPage() {
  return (
    <Box className="wv-page-body">
      <Heading size="6" mb="1">Developer</Heading>
      <Text as="p" size="2" color="gray" mb="4">
        The Amiga's debug output and DOS calls as they happen, its tasks, a command line, its screens.
      </Text>
      <Tabs.Root defaultValue="logs">
        <Tabs.List>
          <Tabs.Trigger value="logs">Logs</Tabs.Trigger>
          <Tabs.Trigger value="tasks">Tasks</Tabs.Trigger>
          <Tabs.Trigger value="run">Run</Tabs.Trigger>
          <Tabs.Trigger value="screens">Screens</Tabs.Trigger>
        </Tabs.List>
        <Box pt="4">
          {/* Logs stays alive on other tabs: leaving it would stop the
              streams and lose what came in meanwhile. */}
          <Tabs.Content value="logs" forceMount className="wv-keep-tab"><LogsTab /></Tabs.Content>
          <Tabs.Content value="tasks"><TasksTab /></Tabs.Content>
          <Tabs.Content value="run"><RunTab /></Tabs.Content>
          <Tabs.Content value="screens"><ScreensTab /></Tabs.Content>
        </Box>
      </Tabs.Root>
    </Box>
  );
}
