import { useEffect, useState } from 'react';
import {
  AlertDialog,
  Box,
  Button,
  Callout,
  Card,
  Checkbox,
  Dialog,
  Flex,
  Heading,
  IconButton,
  Spinner,
  Table,
  Text,
  TextField,
  Tooltip,
} from '@radix-ui/themes';
import {
  ArrowLeftIcon,
  ArrowRightIcon,
  ArrowUpIcon,
  CrossCircledIcon,
  FileIcon,
  PlusIcon,
  ReloadIcon,
  TrashIcon,
} from '@radix-ui/react-icons';
import { api, bytes, NeedsForce, type Entry } from '../api';

type Side = 'pc' | 'amiga';
type Listing = { path: string; entries: Entry[]; parent?: string };

function amigaJoin(dir: string, name: string) {
  if (!dir) return name.endsWith(':') ? name : `${name}:`;
  return dir.endsWith(':') || dir.endsWith('/') ? dir + name : `${dir}/${name}`;
}

function amigaParent(p: string): string {
  if (!p || p.endsWith(':')) return '';               // volume -> the volume list
  const i = Math.max(p.lastIndexOf('/'), p.lastIndexOf(':'));
  return p.slice(0, i + (p[i] === ':' ? 1 : 0));
}

function FolderGlyph() {
  return <Text color="amber" size="2">▸</Text>;
}

/* One side of the file manager. Click a folder to open it; tick files
 * to copy them across. */
function Pane({ side, path, setPath, listing, selected, setSelected, busy, onReload,
  onMkdir, onDelete }: {
  side: Side;
  path: string;
  setPath: (p: string) => void;
  listing: Listing | null;
  selected: Set<string>;
  setSelected: (s: Set<string>) => void;
  busy: boolean;
  onReload: () => void;
  onMkdir: (name: string) => void;
  onDelete?: () => void;
}) {
  const [typed, setTyped] = useState(path);
  const [typedFor, setTypedFor] = useState(path);
  const [newName, setNewName] = useState('');
  if (typedFor !== path) {        // a new folder: show its path again
    setTypedFor(path);
    setTyped(path);
  }
  const entries = listing?.entries ?? [];
  const up = side === 'pc' ? (listing?.parent ?? path) : amigaParent(path);
  const open = (e: Entry) => {
    if (side === 'amiga') setPath(path ? amigaJoin(path, e.name) : `${e.name}:`);
    else setPath(`${path.replace(/\/$/, '')}/${e.name}`);
  };
  const toggle = (name: string) => {
    const s = new Set(selected);
    if (s.has(name)) s.delete(name); else s.add(name);
    setSelected(s);
  };
  return (
    <Card size="2">
      <Flex align="center" justify="between" mb="2">
        <Heading size="3">{side === 'pc' ? 'This PC' : 'Amiga'}</Heading>
        <Flex gap="1" align="center">
          {busy && <Spinner />}
          <Tooltip content="Up one level">
            <IconButton variant="ghost" color="gray" aria-label="Up one level"
              onClick={() => setPath(up)} disabled={side === 'amiga' && !path}>
              <ArrowUpIcon />
            </IconButton>
          </Tooltip>
          <Tooltip content="Refresh">
            <IconButton variant="ghost" color="gray" aria-label="Refresh" onClick={onReload}>
              <ReloadIcon />
            </IconButton>
          </Tooltip>
          <Dialog.Root onOpenChange={() => setNewName('')}>
            <Tooltip content="New folder">
              <Dialog.Trigger>
                <IconButton variant="ghost" color="gray" aria-label="New folder"
                  disabled={side === 'amiga' && !path}>
                  <PlusIcon />
                </IconButton>
              </Dialog.Trigger>
            </Tooltip>
            <Dialog.Content maxWidth="var(--wv-dialog-width)">
              <Dialog.Title>New folder</Dialog.Title>
              <Dialog.Description size="2" color="gray" mb="3">In {path || 'the root'}</Dialog.Description>
              <TextField.Root autoFocus value={newName} placeholder="Name"
                onChange={(e) => setNewName(e.target.value)} />
              <Flex gap="3" mt="4" justify="end">
                <Dialog.Close><Button variant="soft" color="gray">Cancel</Button></Dialog.Close>
                <Dialog.Close>
                  <Button disabled={!newName} onClick={() => onMkdir(newName)}>Create</Button>
                </Dialog.Close>
              </Flex>
            </Dialog.Content>
          </Dialog.Root>
          {onDelete && (
            <Tooltip content="Delete the ticked items">
              <IconButton variant="ghost" color="red" aria-label="Delete"
                disabled={!selected.size} onClick={onDelete}>
                <TrashIcon />
              </IconButton>
            </Tooltip>
          )}
        </Flex>
      </Flex>
      <TextField.Root size="1" mb="2" value={typed}
        placeholder={side === 'amiga' ? 'Volumes - type a path like Work:' : '~'}
        onChange={(e) => setTyped(e.target.value)}
        onKeyDown={(e) => { if (e.key === 'Enter') setPath(typed); }} />
      <Box className="wv-pane-list">
        <Table.Root size="1">
          <Table.Body>
            {entries.map((e) => (
              <Table.Row key={e.name} className={e.dir ? 'wv-row-dir' : undefined}
                onDoubleClick={() => e.dir && open(e)}>
                <Table.Cell>
                  <Checkbox checked={selected.has(e.name)} aria-label={`Select ${e.name}`}
                    onCheckedChange={() => toggle(e.name)} />
                </Table.Cell>
                <Table.Cell className="wv-grow" onClick={() => (e.dir ? open(e) : toggle(e.name))}>
                  <Flex gap="2" align="center">
                    {e.dir ? <FolderGlyph /> : <FileIcon />}
                    <Text size="2">{e.name}</Text>
                  </Flex>
                </Table.Cell>
                <Table.Cell justify="end" className="wv-nowrap">
                  <Text size="1" color="gray">{bytes(e.size)}</Text>
                </Table.Cell>
                <Table.Cell className="wv-nowrap">
                  {/* volumes have no date: AmigaDOS says day 0, 1978 */}
                  <Text size="1" color="gray">{e.date.startsWith('1978-01-01') ? '' : e.date}</Text>
                </Table.Cell>
              </Table.Row>
            ))}
            {listing && !entries.length && (
              <Table.Row><Table.Cell colSpan={4}><Text size="2" color="gray">Empty</Text></Table.Cell></Table.Row>
            )}
          </Table.Body>
        </Table.Root>
      </Box>
    </Card>
  );
}

export function FilesPage() {
  const [pcPath, setPcPath] = useState('~');
  const [amPath, setAmPath] = useState('');
  const [pc, setPc] = useState<Listing | null>(null);
  const [am, setAm] = useState<Listing | null>(null);
  const [pcSel, setPcSel] = useState<Set<string>>(new Set());
  const [amSel, setAmSel] = useState<Set<string>>(new Set());
  const [busy, setBusy] = useState<Side | null>(null);
  const [error, setError] = useState('');
  const [done, setDone] = useState('');
  const [confirm, setConfirm] = useState<{ text: string; go: () => void } | null>(null);

  // Listings reload when the path changes, or when `reload` is bumped
  // after a change. setState only in the callbacks, never in the effect.
  const [pcReload, setPcReload] = useState(0);
  const [amReload, setAmReload] = useState(0);
  useEffect(() => {
    let stop = false;
    api<Listing>(`api/local/ls?path=${encodeURIComponent(pcPath)}`)
      .then((r) => {
        if (stop) return;
        setPc(r);
        setPcSel(new Set());
        if (r.path !== pcPath) setPcPath(r.path);
      })
      .catch((e: Error) => { if (!stop) setError(e.message); });
    return () => { stop = true; };
  }, [pcPath, pcReload]);
  useEffect(() => {
    let stop = false;
    api<Listing>(`api/amiga/ls?path=${encodeURIComponent(amPath)}`)
      .then((r) => { if (!stop) { setAm(r); setAmSel(new Set()); } })
      .catch((e: Error) => { if (!stop) setError(e.message); });
    return () => { stop = true; };
  }, [amPath, amReload]);
  const loadPc = () => setPcReload((n) => n + 1);
  const loadAm = () => setAmReload((n) => n + 1);

  /* Run a change; a system place answers 428 - ask, then run it forced. */
  const run = async (side: Side, what: string, fn: (force: boolean) => Promise<unknown>,
    after: () => void) => {
    setError('');
    setDone('');
    setBusy(side);
    try {
      await fn(false);
      setDone(what);
      after();
    } catch (e) {
      if (e instanceof NeedsForce) {
        setConfirm({
          text: `${e.message}. Do it anyway?`,
          go: () => {
            setBusy(side);
            fn(true).then(() => { setDone(what); after(); })
              .catch((e2: Error) => setError(e2.message))
              .finally(() => setBusy(null));
          },
        });
      } else {
        setError((e as Error).message);
      }
    } finally {
      setBusy(null);
    }
  };

  const toAmiga = () => run('amiga', `Copied ${pcSel.size} item(s) to ${amPath}`,
    (force) => api('api/copy/to-amiga', {
      paths: [...pcSel].map((n) => `${pc?.path}/${n}`), dir: amPath, force,
    }), () => loadAm());
  const toPc = () => run('pc', `Copied ${amSel.size} item(s) to ${pc?.path}`,
    () => api('api/copy/to-pc', {
      items: (am?.entries ?? []).filter((e) => amSel.has(e.name))
        .map((e) => ({ path: amigaJoin(amPath, e.name), dir: e.dir })),
      dir: pc?.path,
    }), () => loadPc());

  return (
    <Box className="wv-page-body">
      <Heading size="6" mb="1">Files</Heading>
      <Text as="p" size="2" color="gray" mb="4">
        Tick files or folders, then copy them across. Click a folder to open it.
      </Text>
      {error && (
        <Callout.Root color="red" mb="3">
          <Callout.Icon><CrossCircledIcon /></Callout.Icon>
          <Callout.Text>{error}</Callout.Text>
        </Callout.Root>
      )}
      {done && <Text as="p" size="2" color="green" mb="3">{done}</Text>}
      <Box className="wv-panes">
        <Pane side="pc" path={pc?.path ?? pcPath} setPath={setPcPath} listing={pc}
          selected={pcSel} setSelected={setPcSel} busy={busy === 'pc'}
          onReload={() => loadPc()}
          onMkdir={(name) => run('pc', `Made ${name}`,
            () => api('api/local/mkdir', { path: `${pc?.path}/${name}` }),
            () => loadPc())} />
        <Flex direction="column" gap="3" pt="9">
          <Tooltip content="Copy the ticked PC items to the Amiga">
            <Button disabled={!pcSel.size || !amPath || !!busy} onClick={() => void toAmiga()}>
              Copy <ArrowRightIcon />
            </Button>
          </Tooltip>
          <Tooltip content="Copy the ticked Amiga items to this PC">
            <Button disabled={!amSel.size || !amPath || !!busy} onClick={() => void toPc()}>
              <ArrowLeftIcon /> Copy
            </Button>
          </Tooltip>
        </Flex>
        <Pane side="amiga" path={amPath} setPath={setAmPath} listing={am}
          selected={amSel} setSelected={setAmSel} busy={busy === 'amiga'}
          onReload={() => loadAm()}
          onMkdir={(name) => run('amiga', `Made ${name}`,
            () => api('api/amiga/mkdir', { path: amigaJoin(amPath, name) }),
            () => loadAm())}
          onDelete={() => setConfirm({
            text: `Delete ${amSel.size} item(s) on the Amiga, folders with everything in them? This cannot be undone.`,
            go: () => void run('amiga', `Deleted ${amSel.size} item(s)`, async (force) => {
              for (const e of (am?.entries ?? []).filter((x) => amSel.has(x.name))) {
                await api('api/amiga/delete', { path: amigaJoin(amPath, e.name), force, dir: e.dir });
              }
            }, () => loadAm()),
          })} />
      </Box>
      <AlertDialog.Root open={!!confirm} onOpenChange={(o) => { if (!o) setConfirm(null); }}>
        <AlertDialog.Content maxWidth="var(--wv-dialog-width)">
          <AlertDialog.Title>Are you sure?</AlertDialog.Title>
          <AlertDialog.Description size="2">{confirm?.text}</AlertDialog.Description>
          <Flex gap="3" mt="4" justify="end">
            <AlertDialog.Cancel><Button variant="soft" color="gray">Cancel</Button></AlertDialog.Cancel>
            <AlertDialog.Action>
              <Button color="red" onClick={() => { const c = confirm; setConfirm(null); c?.go(); }}>
                Yes, go ahead
              </Button>
            </AlertDialog.Action>
          </Flex>
        </AlertDialog.Content>
      </AlertDialog.Root>
    </Box>
  );
}
