import { useEffect, useState } from 'react';
import {
  Badge,
  Box,
  Button,
  Flex,
  Heading,
  Text,
} from '@radix-ui/themes';
import {
  CameraIcon,
  CodeIcon,
  DashboardIcon,
  DesktopIcon,
  FileTextIcon,
} from '@radix-ui/react-icons';
import { api, type AuthState, type Health, type Me } from './api';
import { LoginPage } from './pages/Login';
import { LogoutButton, RebootButton } from './ui/Machine';
import { OverviewPage } from './pages/Overview';
import { ScreenPage } from './pages/Screen';
import { FilesPage } from './pages/Files';
import { ShotsPage } from './pages/Shots';
import { DeveloperPage } from './pages/Developer';

const PAGES = [
  { id: 'overview', label: 'Overview', icon: <DashboardIcon /> },
  { id: 'screen', label: 'Screen', icon: <DesktopIcon /> },
  { id: 'files', label: 'Files', icon: <FileTextIcon /> },
  { id: 'shots', label: 'Screenshots', icon: <CameraIcon /> },
  { id: 'dev', label: 'Developer', icon: <CodeIcon /> },
] as const;
type PageId = (typeof PAGES)[number]['id'];

/* Pages live in the URL's #: a reload or the back button keeps them. */
function go(id: PageId) {
  window.location.hash = id;
}

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
  const [auth, setAuth] = useState<AuthState | null>(null);
  const [me, setMe] = useState<Me | null>(null);
  const [authTick, setAuthTick] = useState(0);

  // A server with a login (Wasabi phone): ask whether we are in, and
  // ask again whenever a call comes back "please log in".
  useEffect(() => {
    let stop = false;
    api<AuthState>('api/auth/state')
      .then((a) => {
        if (stop) return;
        setAuth(a);
        if (!a.required || a.logged_in) {
          api<Me>('api/me').then((m) => { if (!stop) setMe(m); }).catch(() => {});
        }
      })
      .catch(() => { if (!stop) setAuth({ required: false, setup: false, logged_in: true }); });
    return () => { stop = true; };
  }, [authTick]);
  useEffect(() => {
    const again = () => setAuthTick((n) => n + 1);
    window.addEventListener('wasabi-login', again);
    return () => window.removeEventListener('wasabi-login', again);
  }, []);
  const locked = !!auth && auth.required && !auth.logged_in;

  // "This window is open": the bridge ends when the last one goes
  // (unless it is a server). Kept open whatever page is showing.
  useEffect(() => {
    if (locked || !auth) return;
    const u = new URL('ws/hello', window.location.href);
    u.protocol = u.protocol === 'https:' ? 'wss:' : 'ws:';
    let sock: WebSocket | null = null;
    let timer = 0;
    let stop = false;
    const open = () => {
      sock = new WebSocket(u);
      sock.onclose = () => { if (!stop) timer = window.setTimeout(open, 2000); };
    };
    open();
    return () => { stop = true; window.clearTimeout(timer); sock?.close(); };
  }, [locked, auth]);

  useEffect(() => {
    const on = () => setPage(pageFromHash());
    window.addEventListener('hashchange', on);
    return () => window.removeEventListener('hashchange', on);
  }, []);

  // A light pulse for the sidebar's connection badge.
  useEffect(() => {
    if (viewOnly || locked || !auth) return;
    let stop = false;
    const ping = () => api<Health>('api/health')
      .then((h) => { if (!stop) { setOnline(true); setBanner(h.banner ?? ''); } })
      .catch(() => { if (!stop) setOnline(false); });
    void ping();
    const t = window.setInterval(ping, 10000);
    return () => { stop = true; window.clearInterval(t); };
  }, [viewOnly, locked, auth]);

  if (!auth) return null;
  if (locked) return <LoginPage setup={auth.setup} onDone={() => setAuthTick((n) => n + 1)} />;
  if (viewOnly) return <ScreenPage bare />;

  return (
    <Flex className="wv-shell" direction={{ initial: 'column', md: 'row' }}>
      <Flex direction="column" className="wv-sidebar" p="3" gap="1"
        display={{ initial: 'none', md: 'flex' }}>
        <Box px="2" pt="1" pb="4">
          <Heading size="5">Wasabi</Heading>
          <Text size="1" color="gray">Amiga remote control</Text>
        </Box>
        {PAGES.map((p) => (
          <Button key={p.id} className="wv-nav-item" size="2"
            variant={page === p.id ? 'soft' : 'ghost'}
            color={page === p.id ? undefined : 'gray'}
            aria-current={page === p.id ? 'page' : undefined}
            onClick={() => go(p.id)}>
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
          {auth.required && <LogoutButton onDone={() => setAuthTick((n) => n + 1)} />}
        </Flex>
      </Flex>
      <Box className="wv-content">
        {page === 'overview' && (
          <OverviewPage login={auth.required} onLogout={() => setAuthTick((n) => n + 1)} />
        )}
        {page === 'screen' && <ScreenPage />}
        {page === 'files' && <FilesPage me={me} />}
        {page === 'shots' && <ShotsPage />}
        {page === 'dev' && <DeveloperPage />}
      </Box>
      <Flex className="wv-bottom-nav" display={{ initial: 'flex', md: 'none' }}>
        {PAGES.map((p) => (
          <Button key={p.id} variant="ghost" size="1" radius="none"
            color={page === p.id ? undefined : 'gray'}
            aria-current={page === p.id ? 'page' : undefined}
            onClick={() => go(p.id)}>
            {p.icon}
            <Text size="1">{p.label}</Text>
          </Button>
        ))}
      </Flex>
    </Flex>
  );
}
