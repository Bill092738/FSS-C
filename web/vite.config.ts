/// <reference types="vitest/config" />
import tailwindcss from '@tailwindcss/vite'
import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// The C backend (server/build/fss) listens on :8080 by default.
const backend = process.env.FSS_BACKEND ?? 'http://localhost:8080'

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    // changeOrigin stays false: the backend rejects writes whose Origin host
    // differs from the Host header (roadmap 10.5), so the browser's
    // Host: localhost:5173 must reach it unchanged.
    proxy: {
      '/api': { target: backend, changeOrigin: false },
      '/ws': { target: backend.replace(/^http/, 'ws'), ws: true, changeOrigin: false },
    },
  },
  build: {
    // MapLibre alone is ~1 MB minified and already in its own lazy chunk.
    chunkSizeWarningLimit: 1100,
  },
  test: {
    environment: 'jsdom',
  },
})
