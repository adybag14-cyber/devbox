import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const root = new URL('../', import.meta.url);
const read = file => readFile(new URL(file, root), 'utf8');
const [cmake, setup, tui, versionText, vcpkgText, android, workflow, packageText] = await Promise.all([
  read('CMakeLists.txt'), read('cpp-bootstrap/src/setup.cpp'), read('setup-tui/CMakeLists.txt'),
  read('cpp-bootstrap/VERSION'), read('vcpkg.json'), read('cpp-mcp/scripts/build-android.mjs'),
  read('.github/workflows/cpp-runtime.yml'), read('package.json'),
]);
const version = versionText.trim();
assert.match(version, /^\d+\.\d+\.\d+$/);
assert.match(cmake, /cmake_minimum_required\(VERSION 3\.24\)/);
assert.match(cmake, /set\(CMAKE_CXX_STANDARD 23\)/);
assert.match(cmake, /set\(CMAKE_CXX_STANDARD_REQUIRED ON\)/);
assert.match(cmake, /set\(CMAKE_CXX_EXTENSIONS OFF\)/);
assert.match(cmake, /project\(devbox_cpp VERSION 0\.3\.0 /);
const baseline = JSON.parse(vcpkgText)['builtin-baseline'];
assert.match(baseline, /^[a-f0-9]{40}$/);
assert.equal(setup.match(/vcpkg_revision = "([a-f0-9]+)"/)[1], baseline);
assert.equal(tui.match(/project\(devbox_tui VERSION ([0-9.]+)/)[1], version);
assert(workflow.includes('node-version: 24'));
assert(workflow.includes(baseline));
assert(workflow.includes('ndk;29.0.14206865'));
assert(android.includes('29\\.0\\.14206865'));
assert(android.includes('-DANDROID_PLATFORM=android-21'));
assert.equal(JSON.parse(packageText).engines.node, '>=18');
console.log(JSON.stringify({ ok: true, implementation: 'cpp', cxx: 23, cmake: '3.24',
  runtimeVersion: '0.3.0', setupVersion: version, nodeMinimum: 18, nodeCertified: 24,
  dependencyBaseline: baseline, ndk: '29.0.14206865', androidApi: 21 }));
