import react from '@vitejs/plugin-react';
import { configDefaults, defineConfig } from 'vitest/config';

const artifactsRoot = process.env.INPX_WEB_READER_WEB_ARTIFACTS_ROOT ?? '../../out/web/inpx-web-reader';

export default defineConfig({
  plugins: [react()],
  cacheDir: `${artifactsRoot}/vite-cache`,
  build: {
    outDir: `${artifactsRoot}/dist`,
    emptyOutDir: true
  },
  test: {
    environment: 'jsdom',
    environmentOptions: {
      jsdom: {
        url: 'http://127.0.0.1/'
      }
    },
    setupFiles: './src/test/setup.ts',
    css: true,
    exclude: [...configDefaults.exclude, 'tests/**'],
    coverage: {
      provider: 'v8',
      reporter: ['text', 'html', 'json-summary'],
      reportsDirectory: `${artifactsRoot}/coverage/web`,
      include: ['src/**/*.{ts,tsx}'],
      exclude: [...configDefaults.exclude, 'tests/**', 'src/test/**']
    }
  }
});
