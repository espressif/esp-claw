import { defineConfig } from 'vite'

export default defineConfig({
  base: './',
  server: {
    proxy: {
      '/lab/raw': { target: 'http://localhost:5173', rewrite: (path) => path.replace(/^\/lab/, '') },
    },
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
  },
})
