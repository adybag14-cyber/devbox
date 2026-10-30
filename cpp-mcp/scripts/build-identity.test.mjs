import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, unlinkSync, writeFileSync} from 'node:fs';
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
