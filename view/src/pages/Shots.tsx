import { useEffect, useState } from 'react';
import {
  AlertDialog,
  Box,
  Button,
  Callout,
  Card,
  Dialog,
  Flex,
  Heading,
  Inset,
  Link,
  Spinner,
  Text,
} from '@radix-ui/themes';
import { CameraIcon, CrossCircledIcon, TrashIcon } from '@radix-ui/react-icons';
import { api, bytes } from '../api';

type Shot = { name: string; size: number; date: string };

/* Screenshots: full, exact grabs (24-bit PNG, like `wasabi grab`),
 * kept on this PC in ~/Pictures/Wasabi. */
export function ShotsPage() {
  const [shots, setShots] = useState<Shot[]>([]);
  const [folder, setFolder] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [open, setOpen] = useState<Shot | null>(null);
  const [del, setDel] = useState<Shot | null>(null);

  const [reload, setReload] = useState(0);
  const load = () => setReload((n) => n + 1);
  useEffect(() => {
    let stop = false;
    api<{ folder: string; shots: Shot[] }>('api/shots')
      .then((r) => { if (!stop) { setShots(r.shots); setFolder(r.folder); } })
      .catch((e: Error) => { if (!stop) setError(e.message); });
    return () => { stop = true; };
  }, [reload]);

  const take = async () => {
    setBusy(true);
    setError('');
    try {
      await api('api/grab', {});
      load();
    } catch (e) { setError((e as Error).message); }
    finally { setBusy(false); }
  };

  return (
    <Box className="wv-page-body">
      <Flex align="center" justify="between" mb="1">
        <Heading size="6">Screenshots</Heading>
        <Button onClick={() => void take()} disabled={busy}>
          {busy ? <Spinner /> : <CameraIcon />} Take screenshot
        </Button>
      </Flex>
      <Text as="p" size="2" color="gray" mb="4">
        The Amiga's front screen, full colour, saved in {folder || 'Pictures/Wasabi'}.
      </Text>
      {error && (
        <Callout.Root color="red" mb="3">
          <Callout.Icon><CrossCircledIcon /></Callout.Icon>
          <Callout.Text>{error}</Callout.Text>
        </Callout.Root>
      )}
      {!shots.length && !busy && <Text size="2" color="gray">No screenshots yet.</Text>}
      <Box className="wv-shots">
        {shots.map((s) => (
          <Card key={s.name} size="1" className="wv-shot-card" role="button" tabIndex={0}
            aria-label={`Open ${s.name}`} onClick={() => setOpen(s)}
            onKeyDown={(e) => { if (e.key === 'Enter' || e.key === ' ') setOpen(s); }}>
            <Inset clip="padding-box" side="top" pb="current">
              <img className="wv-thumb" src={`shots/${encodeURIComponent(s.name)}`} alt={s.name} loading="lazy" />
            </Inset>
            <Text as="div" size="2">{s.date}</Text>
            <Text as="div" size="1" color="gray">{bytes(s.size)}</Text>
          </Card>
        ))}
      </Box>

      <Dialog.Root open={!!open} onOpenChange={(o) => { if (!o) setOpen(null); }}>
        <Dialog.Content maxWidth="var(--wv-content-max)">
          <Dialog.Title>{open?.name}</Dialog.Title>
          <Dialog.Description size="2" color="gray">{open?.date} · {bytes(open?.size ?? 0)}</Dialog.Description>
          {open && <img className="wv-shot-big" src={`shots/${encodeURIComponent(open.name)}`} alt={open.name} />}
          <Flex gap="3" justify="end">
            {open && (
              <Button variant="soft" asChild>
                <Link href={`shots/${encodeURIComponent(open.name)}`} download={open.name}>Download</Link>
              </Button>
            )}
            <Button variant="soft" color="red" onClick={() => { setDel(open); setOpen(null); }}>
              <TrashIcon /> Delete
            </Button>
            <Dialog.Close><Button>Close</Button></Dialog.Close>
          </Flex>
        </Dialog.Content>
      </Dialog.Root>

      <AlertDialog.Root open={!!del} onOpenChange={(o) => { if (!o) setDel(null); }}>
        <AlertDialog.Content maxWidth="var(--wv-dialog-width)">
          <AlertDialog.Title>Delete this screenshot?</AlertDialog.Title>
          <AlertDialog.Description size="2">{del?.name} will be removed from this PC.</AlertDialog.Description>
          <Flex gap="3" mt="4" justify="end">
            <AlertDialog.Cancel><Button variant="soft" color="gray">Cancel</Button></AlertDialog.Cancel>
            <AlertDialog.Action>
              <Button color="red" onClick={() => {
                const d = del;
                setDel(null);
                if (d) api('api/shots/delete', { name: d.name }).then(load)
                  .catch((e: Error) => setError(e.message));
              }}>Delete</Button>
            </AlertDialog.Action>
          </Flex>
        </AlertDialog.Content>
      </AlertDialog.Root>
    </Box>
  );
}
