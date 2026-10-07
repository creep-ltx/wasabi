import { useState } from 'react';
import { Box, Button, Callout, Card, Flex, Heading, Text, TextField } from '@radix-ui/themes';
import { CrossCircledIcon, LockClosedIcon } from '@radix-ui/react-icons';
import { api } from '../api';

/*
 * Wasabi phone's door. The first visit chooses the password (the person
 * who has just set the server up); after that it asks for it. A login
 * lasts 30 days on that device.
 */
export function LoginPage({ setup, onDone }: { setup: boolean; onDone: () => void }) {
  const [pw, setPw] = useState('');
  const [again, setAgain] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);

  const go = async () => {
    setError('');
    if (setup && pw !== again) {
      setError('The two passwords are not the same');
      return;
    }
    setBusy(true);
    try {
      await api(setup ? 'api/auth/setup' : 'api/auth/login', { password: pw });
      onDone();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Flex className="wv-login" align="center" justify="center" p="4">
      <Card size="4" className="wv-login-card">
        <Flex direction="column" gap="3">
          <Flex align="center" gap="2">
            <LockClosedIcon />
            <Heading size="5">Wasabi</Heading>
          </Flex>
          <Text size="2" color="gray">
            {setup
              ? 'Choose a password for Wasabi. You will need it on each phone or computer, once a month.'
              : 'Log in to reach the Amiga.'}
          </Text>
          <TextField.Root type="password" placeholder="Password" value={pw} autoFocus
            autoComplete={setup ? 'new-password' : 'current-password'}
            onChange={(e) => setPw(e.target.value)}
            onKeyDown={(e) => { if (e.key === 'Enter' && !setup) void go(); }} />
          {setup && (
            <TextField.Root type="password" placeholder="The same again" value={again}
              autoComplete="new-password" onChange={(e) => setAgain(e.target.value)}
              onKeyDown={(e) => { if (e.key === 'Enter') void go(); }} />
          )}
          {error && (
            <Callout.Root color="red" size="1">
              <Callout.Icon><CrossCircledIcon /></Callout.Icon>
              <Callout.Text>{error}</Callout.Text>
            </Callout.Root>
          )}
          <Box>
            <Button size="3" onClick={() => void go()} disabled={busy || !pw}>
              {setup ? 'Set the password' : 'Log in'}
            </Button>
          </Box>
        </Flex>
      </Card>
    </Flex>
  );
}
