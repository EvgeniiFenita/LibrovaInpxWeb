import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(scriptDirectory, '..', '..', '..');
const runWebUiScript = path.join(repoRoot, 'scripts', 'RunWebUi.py');

const candidates = [];
if (process.env.PYTHON) {
  candidates.push([process.env.PYTHON]);
}
candidates.push(['python'], ['python3']);

function canRun(candidate) {
  const result = spawnSync(candidate[0], [...candidate.slice(1), '--version'], { stdio: 'ignore' });
  return result.status === 0;
}

const pythonCommand = candidates.find(canRun);
if (!pythonCommand) {
  console.error('Python was not found. Install Python 3 or set the PYTHON environment variable.');
  process.exit(1);
}

const result = spawnSync(
  pythonCommand[0],
  [...pythonCommand.slice(1), runWebUiScript, ...process.argv.slice(2)],
  {
    cwd: repoRoot,
    env: process.env,
    stdio: 'inherit'
  }
);

if (result.error) {
  console.error(result.error.message);
  process.exit(1);
}

process.exit(result.status ?? 1);
