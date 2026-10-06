import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// `wasabi view` serves dist/ itself; during development the dev server
// passes the WebSocket and the settings API through to it.
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    chunkSizeWarningLimit: 800,
  },
  server: {
    proxy: {
      '/ws': { target: 'ws://localhost:8070', ws: true },
      '/api': { target: 'http://localhost:8070' },
    },
  },
});
