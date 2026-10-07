import { useEffect, useState } from 'react';
import {
  AlertDialog,
  Badge,
  Box,
  Button,
  Dialog,
  Flex,
  Select,
  Spinner,
  Table,
  Text,
  TextField,
} from '@radix-ui/themes';
import { MagnifyingGlassIcon, PlusIcon } from '@radix-ui/react-icons';
import { api, MACHINE, pickMachine, type Found, type MachineRow } from '../api';

/*
 * More than one Amiga: the A1200, FS-UAE on this PC, a friend's machine.
 * Picking one reloads the app for it - every page, stream and screen is
 * that machine's. The list is the bridge's (machines.json).
 */
export function MachinePicker() {
  const [rows, setRows] = useState<MachineRow[]>([]);
  const [manage, setManage] = useState(false);
  const [tick, setTick] = useState(0);
  useEffect(() => {
    let stop = false;
    api<{ machines: MachineRow[] }>('api/machines')
      .then((r) => { if (!stop) setRows(r.machines); }).catch(() => {});
    return () => { stop = true; };
  }, [tick]);
  useEffect(() => {
    const t = window.setInterval(() => setTick((n) => n + 1), 15000);
    return () => window.clearInterval(t);
  }, []);
  const current = rows.find((m) => m.id === MACHINE) ?? rows[0];
  if (!current) return null;
  return (
    <>
      <Select.Root value={current.id} onValueChange={(v) => {
        if (v === '__manage') setManage(true);
        else if (v !== current.id) pickMachine(v);
      }}>
        <Select.Trigger variant="soft" aria-label="Machine" />
        <Select.Content>
          {rows.map((m) => (
            <Select.Item key={m.id} value={m.id}>
              {m.online ? '● ' : '○ '}{m.name}
            </Select.Item>
          ))}
          <Select.Separator />
          <Select.Item value="__manage">Manage machines…</Select.Item>
        </Select.Content>
      </Select.Root>
      {manage && <ManageMachines rows={rows} onClose={() => { setManage(false); setTick((n) => n + 1); }} />}
    </>
  );
}

function ManageMachines({ rows: initial, onClose }: { rows: MachineRow[]; onClose: () => void }) {
  const [rows, setRows] = useState(initial);
  const [name, setName] = useState('');
  const [addr, setAddr] = useState('');
  const [key, setKey] = useState('');
  const [found, setFound] = useState<Found[] | null>(null);
  const [looking, setLooking] = useState(false);
  const [error, setError] = useState('');
  const [remove, setRemove] = useState<MachineRow | null>(null);

  const change = (body: object) => api<{ machines: MachineRow[] }>('api/machines', body)
    .then((r) => { setRows(r.machines); setError(''); })
    .catch((e: Error) => setError(e.message));
  const add = (host: string, port: number, n: string, k = '') => change({ action: 'add', host, port, name: n, key: k })
    .then(() => { setName(''); setAddr(''); setKey(''); setFound(null); });

  return (
    <Dialog.Root open onOpenChange={(o) => { if (!o) onClose(); }}>
      <Dialog.Content maxWidth="var(--wv-dialog-wide)">
        <Dialog.Title>Machines</Dialog.Title>
        <Dialog.Description size="2" color="gray" mb="3">
          Every Amiga this app can reach. "Found automatically" is wherever Wasabi finds
          the first one on the network, even if its address changes.
        </Dialog.Description>
        <Table.Root size="1">
          <Table.Body>
            {rows.map((m) => (
              <Table.Row key={m.id}>
                <Table.Cell>
                  <TextField.Root size="1" defaultValue={m.name} aria-label={`Name of ${m.name}`}
                    onBlur={(e) => { if (e.target.value !== m.name) void change({ action: 'rename', id: m.id, name: e.target.value }); }} />
                </Table.Cell>
                <Table.Cell>
                  <Text size="1" color="gray">{m.auto ? `found automatically (${m.host})` : `${m.host}:${m.port}`}</Text>
                </Table.Cell>
                <Table.Cell>
                  <Badge color={m.online ? 'green' : 'gray'}>{m.online ? m.banner || 'online' : 'not answering'}</Badge>
                </Table.Cell>
                <Table.Cell justify="end">
                  <Button size="1" variant="soft" color="red" disabled={rows.length < 2}
                    onClick={() => setRemove(m)}>Remove</Button>
                </Table.Cell>
              </Table.Row>
            ))}
          </Table.Body>
        </Table.Root>

        <Flex gap="2" mt="4" align="center">
          <Button size="1" variant="soft" disabled={looking} onClick={() => {
            setLooking(true);
            api<{ found: Found[] }>('api/machines/discover')
              .then((r) => setFound(r.found)).catch((e: Error) => setError(e.message))
              .finally(() => setLooking(false));
          }}>
            {looking ? <Spinner /> : <MagnifyingGlassIcon />} Look for Amigas on the network
          </Button>
        </Flex>
        {found && (
          <Box mt="2">
            {!found.length && <Text size="2" color="gray">None answered.</Text>}
            {found.map((f) => (
              <Flex key={`${f.host}:${f.port}`} gap="3" align="center" mt="1">
                <Text size="2">{f.name}</Text>
                <Text size="1" color="gray">{f.host}:{f.port} · {f.banner}</Text>
                <Box flexGrow="1" />
                {f.known ? <Badge color="gray">in the list</Badge> : (
                  <Button size="1" onClick={() => void add(f.host, f.port, f.name)}><PlusIcon /> Add</Button>
                )}
              </Flex>
            ))}
          </Box>
        )}

        <Text as="div" size="2" weight="medium" mt="4" mb="2">Add one by address</Text>
        <Flex gap="2" wrap="wrap">
          <TextField.Root size="1" placeholder="Name (FS-UAE)" value={name} onChange={(e) => setName(e.target.value)} />
          <TextField.Root size="1" placeholder="Address (127.0.0.1 or 127.0.0.1:1234)" value={addr}
            onChange={(e) => setAddr(e.target.value)} className="wv-grow" />
          <TextField.Root size="1" type="password" placeholder="Key, if not the same" value={key}
            onChange={(e) => setKey(e.target.value)} />
          <Button size="1" disabled={!addr.trim()} onClick={() => {
            const [h, p] = addr.trim().split(':');
            void add(h, Number(p) || 1234, name || h, key);
          }}><PlusIcon /> Add</Button>
        </Flex>
        {error && <Text as="p" size="2" color="red" mt="2">{error}</Text>}
        <Flex justify="end" mt="4">
          <Dialog.Close><Button variant="soft" color="gray">Done</Button></Dialog.Close>
        </Flex>

        <AlertDialog.Root open={!!remove} onOpenChange={(o) => { if (!o) setRemove(null); }}>
          <AlertDialog.Content maxWidth="var(--wv-dialog-width)">
            <AlertDialog.Title>Remove {remove?.name}?</AlertDialog.Title>
            <AlertDialog.Description size="2">
              Only from this list - nothing on the machine changes.
            </AlertDialog.Description>
            <Flex gap="3" mt="4" justify="end">
              <AlertDialog.Cancel><Button variant="soft" color="gray">Cancel</Button></AlertDialog.Cancel>
              <AlertDialog.Action>
                <Button color="red" onClick={() => { const m = remove; setRemove(null); if (m) void change({ action: 'remove', id: m.id }); }}>
                  Remove
                </Button>
              </AlertDialog.Action>
            </Flex>
          </AlertDialog.Content>
        </AlertDialog.Root>
      </Dialog.Content>
    </Dialog.Root>
  );
}
