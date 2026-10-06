import { useEffect, useState } from 'react';
import {
  AlertDialog,
  Badge,
  Box,
  Button,
  Flex,
  Heading,
  Text,
} from '@radix-ui/themes';
import {
  CameraIcon,
  DashboardIcon,
  DesktopIcon,
  FileTextIcon,
  ReloadIcon,
} from '@radix-ui/react-icons';
import { api, type Health } from './api';
import { OverviewPage } from './pages/Overview';
import { ScreenPage } from './pages/Screen';
import { FilesPage } from './pages/Files';
import { ShotsPage } from './pages/Shots';

const PAGES = [
  { id: 'overview', label: 'Overview', icon: <DashboardIcon /> },
  { id: 'screen', label: 'Screen', icon: <DesktopIcon /> },
  { id: 'files', label: 'Files', icon: <FileTextIcon /> },
  { id: 'shots', label: 'Screenshots', icon: <CameraIcon /> },
] as const;
type PageId = (typeof PAGES)[number]['id'];

function pageFromHash(): PageId {
  const h = window.location.hash.slice(1);
  return (PAGES.find((p) => p.id === h)?.id ?? 'overview') as PageId;
}

/*
 * `wasabi view` opens ?mode=view: the live screen and nothing else.
 * `wasabi desktop` opens the app: a sidebar and four pages. The page is
 * in the URL's #, so a reload stays where it was.
 */
export function App() {
  const viewOnly = new URLSearchParams(window.location.search).get('mode') === 'view';
  const [page, setPage] = useState<PageId>(pageFromHash);
  const [banner, setBanner] = useState('');
  const [online, setOnline] = useState<boolean | null>(null);

  useEffect(() => {
    const on = () => setPage(pageFromHash());
    window.addEventListener('hashchange', on);
    return () => window.removeEventListener('hashchange', on);
  }, []);

  // A light pulse for the sidebar's connection badge.
  useEffect(() => {
    if (viewOnly) return;
    let stop = false;
    const ping = () => api<Health>('api/health')
      .then((h) => { if (!stop) { setOnline(true); setBanner(h.banner ?? ''); } })
      .catch(() => { if (!stop) setOnline(false); });
    void ping();
    const t = window.setInterval(ping, 10000);
    return () => { stop = true; window.clearInterval(t); };
  }, [viewOnly]);

  if (viewOnly) return <ScreenPage bare />;

  return (
    <Flex className="wv-shell">
      <Flex direction="column" className="wv-sidebar" p="3" gap="1">
        <Box px="2" pt="1" pb="4">
          <Heading size="5">Wasabi</Heading>
          <Text size="1" color="gray">Amiga remote control</Text>
        </Box>
        {PAGES.map((p) => (
          <Button key={p.id} className="wv-nav-item" size="2"
            variant={page === p.id ? 'soft' : 'ghost'}
            color={page === p.id ? undefined : 'gray'}
            aria-current={page === p.id ? 'page' : undefined}
            onClick={() => { window.location.hash = p.id; }}>
            {p.icon} {p.label}
          </Button>
        ))}
        <Box flexGrow="1" />
        <Flex direction="column" gap="2" px="2" pb="1">
          <Badge color={online ? 'green' : online === false ? 'red' : 'gray'}>
            {online ? 'connected' : online === false ? 'not connected' : 'connecting…'}
          </Badge>
          <Text size="1" color="gray">{banner}</Text>
          <RebootButton />
        </Flex>
      </Flex>
      <Box className="wv-content">
        {page === 'overview' && <OverviewPage />}
        {page === 'screen' && <ScreenPage />}
        {page === 'files' && <FilesPage />}
        {page === 'shots' && <ShotsPage />}
      </Box>
    </Flex>
  );
}

function RebootButton() {
  const [msg, setMsg] = useState('');
  return (
    <AlertDialog.Root>
      <AlertDialog.Trigger>
        <Button size="1" variant="soft" color="red"><ReloadIcon /> Reboot Amiga</Button>
      </AlertDialog.Trigger>
      <AlertDialog.Content maxWidth="var(--wv-dialog-width)">
        <AlertDialog.Title>Reboot the Amiga?</AlertDialog.Title>
        <AlertDialog.Description size="2">
          Whatever is open on the Amiga is lost. It takes about half a minute to come back.
          {msg && <Text as="div" color="red" mt="2">{msg}</Text>}
        </AlertDialog.Description>
        <Flex gap="3" mt="4" justify="end">
          <AlertDialog.Cancel><Button variant="soft" color="gray">Cancel</Button></AlertDialog.Cancel>
          <AlertDialog.Action>
            <Button color="red" onClick={() => {
              api('api/reboot', {}).catch((e: Error) => setMsg(e.message));
            }}>Reboot</Button>
          </AlertDialog.Action>
        </Flex>
      </AlertDialog.Content>
    </AlertDialog.Root>
  );
}
