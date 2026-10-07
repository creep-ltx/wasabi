import { useState } from 'react';
import { AlertDialog, Button, Flex, Text } from '@radix-ui/themes';
import { ReloadIcon } from '@radix-ui/react-icons';
import { api } from '../api';

export function RebootButton() {
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

export function LogoutButton({ onDone }: { onDone: () => void }) {
  return (
    <Button size="1" variant="ghost" color="gray"
      onClick={() => { api('api/auth/logout', {}).finally(onDone); }}>
      Log out
    </Button>
  );
}
