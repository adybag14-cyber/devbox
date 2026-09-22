import { access } from 'node:fs/promises';
import path from 'node:path';
import { runCheckedProcess } from '../../src/mcp-implementation.js';
export async function stopStateFixture(binary, root) {
  const state = path.join(root, 'run', 'state');
  try { await access(path.join(state, 'coordinator.json')); } catch (error) {
    if (error.code === 'ENOENT') return;
    throw error;
  }
  await runCheckedProcess(binary, ['--stop-state-coordinator', state], {
    cwd: root, timeoutMs: 10000, label: 'Stop owned fixture state coordinator',
  });
}
