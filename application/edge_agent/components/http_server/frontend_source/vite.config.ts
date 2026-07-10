import { defineConfig } from 'vite';
import solidPlugin from 'vite-plugin-solid';
import tailwindcss from '@tailwindcss/vite';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { compression } from 'vite-plugin-compression2';

export default defineConfig({
  plugins: [
    solidPlugin(),
    tailwindcss(),
    viteSingleFile({ removeViteModuleLoader: true }),
    compression({ algorithms: ['gzip'] }),
  ],
  server: {
    port: 3000,
    proxy: {
      '/api': 'http://esp-claw.local/',
      '/files': 'http://esp-claw.local/',
    },
  },
  build: {
    target: 'es2020',
    outDir: 'dist',
    emptyOutDir: true,
    cssCodeSplit: false,
    assetsInlineLimit: 100_000_000,
    chunkSizeWarningLimit: 4_000,
    rollupOptions: {
      output: {
        codeSplitting: false,
      },
    },
  },
});
