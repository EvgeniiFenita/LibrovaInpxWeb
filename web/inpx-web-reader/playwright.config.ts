import { defineConfig, devices } from '@playwright/test';

const artifactsRoot = process.env.INPX_WEB_READER_WEB_ARTIFACTS_ROOT ?? '../../out/web/inpx-web-reader';
const realServerBaseUrl = process.env.INPX_WEB_READER_WEB_E2E_BASE_URL;

export default defineConfig({
  testDir: './tests',
  timeout: 30000,
  outputDir: `${artifactsRoot}/test-results`,
  use: {
    baseURL: realServerBaseUrl ?? 'http://127.0.0.1:4175',
    trace: 'retain-on-failure'
  },
  webServer: realServerBaseUrl ? undefined : {
    command: 'npx --no-install vite --host 127.0.0.1 --port 4175',
    url: 'http://127.0.0.1:4175',
    reuseExistingServer: !process.env.CI,
    timeout: 120000
  },
  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] }
    }
  ]
});
