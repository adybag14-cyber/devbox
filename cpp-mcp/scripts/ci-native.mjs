import assert from 'node:assert/strict';
import { access, appendFile, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runCheckedProcess } from '../../src/mcp-implementation.js';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const vcpkg = process.env.VCPKG_ROOT;
assert(vcpkg && path.isAbsolute(vcpkg), 'Pass the pinned vcpkg checkout in VCPKG_ROOT');
await access(path.join(vcpkg, 'scripts/buildsystems/vcpkg.cmake'));
const build = path.join(repo, '.cpp-build/ci');
const packageRoot = path.join(repo, '.cpp-build/package');
const sanitizer = process.env.DEVBOX_CI_SANITIZERS === '1';
const configuration = sanitizer ? 'RelWithDebInfo' : 'Release';
const architecture = process.arch === 'arm64' ? 'arm64' : 'x64';
const triplet = `${architecture}-${process.platform === 'win32' ? 'windows-static' : process.platform === 'darwin' ? 'osx' : 'linux'}`;
const env = { ...process.env, VCPKG_MAX_CONCURRENCY: '4' };
async function run(file, args) {
  await runCheckedProcess(file, args, { cwd: repo, env, stdio: 'inherit', timeoutMs: 90 * 60 * 1000, label: 'C++ native certification' });
}
const args = ['-S', repo, '-B', build, `-DCMAKE_BUILD_TYPE=${configuration}`,
  '-DDEVBOX_BUILD_TESTS=ON', '-DDEVBOX_BUILD_TUI=ON',
  `-DCMAKE_TOOLCHAIN_FILE=${path.join(vcpkg, 'scripts/buildsystems/vcpkg.cmake')}`,
  `-DVCPKG_TARGET_TRIPLET=${triplet}`, `-DVCPKG_INSTALLED_DIR=${path.join(repo, '.cpp-build/vcpkg_installed')}`];
if (sanitizer) args.push('-DDEVBOX_SANITIZERS=ON', '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG');
await run('cmake', args);
await run('cmake', ['--build', build, '--config', configuration, '--parallel', '4']);
await run('ctest', ['--test-dir', build, '-C', configuration, '--output-on-failure']);
await run('cmake', ['--install', build, '--config', configuration, '--prefix', packageRoot]);
const extension = process.platform === 'win32' ? '.exe' : '';
const exported = {
  DEVBOX_MCP_TEST_BINARY: path.join(packageRoot, 'bin/devbox-mcp' + extension),
  DEVBOX_CPP_BINARY: path.join(packageRoot, 'bin/devbox-mcp' + extension),
  DEVBOX_SETUP_TEST_BINARY: path.join(packageRoot, 'bin/devbox-setup' + extension),
  DEVBOX_TUI_TEST_BINARY: path.join(packageRoot, 'bin/devbox-tui' + extension),
};
for (const binary of Object.values(exported)) await access(binary);
if (process.env.GITHUB_ENV) await appendFile(process.env.GITHUB_ENV,
  Object.entries(exported).map(([key, value]) => `${key}=${value}\n`).join(''));
await writeFile(path.join(repo, '.cpp-build/ci-native-result.json'), JSON.stringify({
  ok: true, configuration, triplet, ...exported,
  dependencyBaseline: JSON.parse(await readFile(path.join(repo, 'vcpkg.json'), 'utf8'))['builtin-baseline'],
}, null, 2));
