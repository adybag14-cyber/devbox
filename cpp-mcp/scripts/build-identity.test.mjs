import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {copyFileSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, unlinkSync, writeFileSync} from 'node:fs';
import {setTimeout as sleep} from 'node:timers/promises';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

const generator=fileURLToPath(new URL('../cmake/generate_identity.cmake',import.meta.url));
test('compiled package identity follows exact clean tags and invalidates cached metadata',()=>{
  const parent=realpathSync(os.tmpdir());
  const scratch=mkdtempSync(path.join(parent,'devbox-build-identity-'));
  const source=path.join(scratch,'source with spaces'), output=path.join(scratch,'generated');
  const version=path.join(source,'cpp-bootstrap','VERSION');
  mkdirSync(path.dirname(version),{recursive:true});
  const run=(exe,args,success=true)=>{
    const r=spawnSync(exe,args,{cwd:source,encoding:'utf8',timeout:20000,windowsHide:true,
      env:{...process.env,GIT_CONFIG_NOSYSTEM:'1',GIT_CONFIG_GLOBAL:process.platform==='win32'?'NUL':'/dev/null'}});
    if(success)assert.equal(r.status,0,r.stderr||r.stdout||String(r.error));
    else assert.notEqual(r.status,0);
    return r;
  };
  const git=(...args)=>run('git',['-c','user.name=Devbox fixture','-c','user.email=fixture@invalid.test',...args]);
  const generate=(success=true)=>run('cmake',[`-DSOURCE_ROOT=${source}`,`-DOUTPUT_DIR=${output}`,
    '-DCOMPILER=fixture','-DVERSION=0.3.0','-P',generator],success);
  const fields=()=>{
    const header=readFileSync(path.join(output,'build_identity.hpp'),'utf8');
    return Object.fromEntries([...header.matchAll(/const char\* (\w+) = "([^"]*)";/g)].map(m=>[m[1],m[2]]));
  };
  try {
    writeFileSync(version,'0.9.15\n');git('init','--quiet');git('add','.');git('commit','--quiet','-m','fixture');
    generate();const before=fields();
    assert.equal(before.package_version,'0.9.15');assert.equal(before.version,'0.3.0');assert.equal(before.dirty,'false');assert.equal(before.release_tag,'');
    git('tag','-a','bootstrap-v0.9.15','-m','fixture');git('checkout','--detach','--quiet');
    generate();const tagged=fields();
    assert.equal(tagged.ref,'HEAD');assert.equal(tagged.release_tag,'bootstrap-v0.9.15');assert.notEqual(tagged.fingerprint,before.fingerprint);
    generate();assert.deepEqual(fields(),tagged,'Unchanged input must retain cached build identity');
    const extra=path.join(source,'untracked.txt');writeFileSync(extra,'dirty fixture');generate();
    assert.equal(fields().dirty,'true');assert.equal(fields().release_tag,'');
    unlinkSync(extra);writeFileSync(version,'0.9.16\n');generate();
    assert.equal(fields().package_version,'0.9.16');assert.equal(fields().release_tag,'');
    git('add','.');git('commit','--quiet','-m','next version');generate();
    assert.equal(fields().dirty,'false');assert.equal(fields().release_tag,'','An older release tag must not label a newer clean commit');
    writeFileSync(version,'0.9.16"; invalid\n');generate(false);
  } finally {
    assert.equal(path.dirname(realpathSync(scratch)),parent);
    assert.equal(realpathSync(scratch),scratch);
    rmSync(scratch,{recursive:true,force:true});
  }
});

test('version-only edits regenerate installer and TUI compile definitions',async()=>{
  const parent=realpathSync(os.tmpdir());
  const scratch=mkdtempSync(path.join(parent,'devbox-version-config-'));
  const source=path.join(scratch,'source'),build=path.join(scratch,'build');
  const repo=fileURLToPath(new URL('../../',import.meta.url));
  const run=args=>{
    const result=spawnSync('cmake',args,{cwd:scratch,encoding:'utf8',timeout:60000,windowsHide:true});
    assert.equal(result.status,0,result.stderr||result.stdout||String(result.error));
  };
  try {
    for(const component of ['cpp-bootstrap','setup-tui']){
      mkdirSync(path.join(source,component,'src'),{recursive:true});
      copyFileSync(path.join(repo,component,'CMakeLists.txt'),path.join(source,component,'CMakeLists.txt'));
    }
    for(const file of ['setup.cpp','main.cpp','bundle.cpp','bundle_pack_main.cpp','offline_main.cpp'])
      writeFileSync(path.join(source,'cpp-bootstrap/src',file),'// Unbuilt source placeholder for the real CMake target definitions.\n');
    writeFileSync(path.join(source,'setup-tui/src/main.cpp'),'// Unbuilt source placeholder.\n');
    const version=path.join(source,'cpp-bootstrap/VERSION');
    writeFileSync(version,'0.9.15\n');
    writeFileSync(path.join(source,'CMakeLists.txt'),[
      'cmake_minimum_required(VERSION 3.24)',
      'project(version_fixture LANGUAGES CXX)',
      'add_library(devbox_core INTERFACE)',
      'set(DEVBOX_BUILD_TESTS OFF)',
      'add_subdirectory(cpp-bootstrap)',
      'add_subdirectory(setup-tui)',
      'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/versions-$<CONFIG>.txt" CONTENT "$<TARGET_PROPERTY:devbox_setup,COMPILE_DEFINITIONS>\\n$<TARGET_PROPERTY:devbox-tui,COMPILE_DEFINITIONS>")',
      'add_custom_target(check_versions)',
    ].join('\n'));
    run(['-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Release']);
    const definitions=()=>readFileSync(path.join(build,'versions-Release.txt'),'utf8');
    assert.match(definitions(),/DEVBOX_SETUP_VERSION="0\.9\.15"/);
    assert.match(definitions(),/DEVBOX_TUI_VERSION="0\.9\.15"/);
    await sleep(1100); // Make the edit newer even with coarse timestamp precision.
    writeFileSync(version,'0.9.16\n');
    run(['--build',build,'--config','Release','--target','check_versions']);
    assert.match(definitions(),/DEVBOX_SETUP_VERSION="0\.9\.16"/);
    assert.match(definitions(),/DEVBOX_TUI_VERSION="0\.9\.16"/);
  } finally {
    assert.equal(path.dirname(realpathSync(scratch)),parent);
    assert.equal(realpathSync(scratch),scratch);
    rmSync(scratch,{recursive:true,force:true});
  }
});
