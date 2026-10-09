import assert from 'node:assert/strict';
import { appendFile, mkdtemp, writeFile } from 'node:fs/promises';
import path from 'node:path';

assert.equal(process.platform, 'linux');
assert.equal(process.env.GITHUB_ACTIONS, 'true');
assert.equal(process.env.RUNNER_ENVIRONMENT, 'github-hosted');
assert.equal(process.env.RUNNER_OS, 'Linux');
assert(process.env.RUNNER_TEMP && path.isAbsolute(process.env.RUNNER_TEMP));
assert(process.env.GITHUB_ENV && path.isAbsolute(process.env.GITHUB_ENV));

// Public qualification images need no credentials. Hosted runners may carry
// Docker Hub authentication that its mirror cannot accept. Give this job a new
// anonymous client configuration; never edit or reveal the existing one.
const directory = await mkdtemp(path.join(process.env.RUNNER_TEMP, 'devbox-public-docker-'));
assert(!/[\r\n]/.test(directory));
await writeFile(path.join(directory, 'config.json'), '{"auths":{}}\n', { mode: 0o600, flag: 'wx' });
await appendFile(process.env.GITHUB_ENV, `DOCKER_CONFIG=${directory}\n`);
console.log(JSON.stringify({ publicRegistryCredentials: 'anonymous', scope: 'current-job',
  existingCredentialFileModified: false }));
