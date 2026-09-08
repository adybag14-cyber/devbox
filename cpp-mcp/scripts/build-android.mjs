import assert from 'node:assert/strict';
import { runCheckedProcess } from '../../src/mcp-implementation.js';
import { access, readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const options = Object.fromEntries(process.argv.slice(2).map((arg, i, args) =>
  arg.startsWith('--') ? [arg.slice(2), args[i + 1]] : null).filter(Boolean));
const abis = { 'arm64-v8a': 'arm64', 'armeabi-v7a': 'arm', x86_64: 'x64', x86: 'x86' };
const abi = options.abi || 'arm64-v8a';
assert(abis[abi], 'Choose one of arm64-v8a, armeabi-v7a, x86_64 or x86');
// CMake persists this value as a quoted string in CMakeSystem.cmake. Native
// Windows backslashes would turn C:\Users into an invalid CMake escape.
const ndk = path.resolve(options.ndk || process.env.ANDROID_NDK_HOME || '').replaceAll('\\', '/');
const vcpkg = path.resolve(options.vcpkg || process.env.VCPKG_ROOT || path.join(repo, '.cpp-build/vcpkg'));
await access(path.join(ndk, 'build/cmake/android.toolchain.cmake'));
await access(path.join(vcpkg, 'scripts/buildsystems/vcpkg.cmake'));
assert.match(await readFile(path.join(ndk, 'source.properties'), 'utf8'), /Pkg\.Revision\s*=\s*29\.0\.14206865\s/,
  'Android certification requires the pinned NDK 29.0.14206865');
const build = path.resolve(options.build || path.join(repo, '.cpp-build/android', abi));
const env = { ...process.env, ANDROID_NDK_HOME: ndk, VCPKG_MAX_CONCURRENCY: '4' };
const cmake = options.cmake || process.env.CMAKE_EXE || 'cmake';
async function run(args) {
  await runCheckedProcess(cmake, args, { cwd: repo, env, stdio: 'inherit',
    timeoutMs: 90 * 60 * 1000, label: `Android ${abi} CMake ${args[0]}` });
}
await run(['-S', repo, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
  '-DDEVBOX_BUILD_TESTS=OFF', '-DDEVBOX_BUILD_TUI=ON',
  `-DCMAKE_TOOLCHAIN_FILE=${path.join(vcpkg, 'scripts/buildsystems/vcpkg.cmake')}`,
  `-DVCPKG_OVERLAY_TRIPLETS=${path.join(repo, 'cpp-mcp/cmake/triplets')}`,
  `-DVCPKG_TARGET_TRIPLET=${abis[abi]}-android-api21`,
  `-DVCPKG_INSTALLED_DIR=${path.join(repo, '.cpp-build/android/vcpkg_installed')}`,
  '-DCMAKE_SYSTEM_NAME=Android', '-DCMAKE_SYSTEM_VERSION=21', `-DCMAKE_ANDROID_ARCH_ABI=${abi}`,
  `-DCMAKE_ANDROID_NDK=${ndk}`, `-DANDROID_ABI=${abi}`, '-DANDROID_PLATFORM=android-21',
  '-DANDROID_STL=c++_static']);
await run(['--build', build, '--parallel', '4']);
console.log(JSON.stringify({ ok: true, abi, api: 21, ndk, build }));
