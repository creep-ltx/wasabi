import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { Theme } from '@radix-ui/themes';
import '@radix-ui/themes/styles.css';
import './tokens.css';
import './app.css';
import { App } from './App';

const root = document.getElementById('root');
if (root) {
  createRoot(root).render(
    <StrictMode>
      <Theme appearance="dark" hasBackground={false} radius="none" scaling="90%">
        <App />
      </Theme>
    </StrictMode>,
  );
}
