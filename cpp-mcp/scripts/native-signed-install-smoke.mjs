import assert from 'node:assert/strict';
import {access,mkdtemp,rename,rm,readFile,writeFile} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
const assets=path.resolve('.cpp-build/release-assets');
const receipt=path.join(assets,'qualification-receipt.json');
const provenance=path.join(assets,'provenance.sigstore.json');
const proof=JSON.parse(await readFile(receipt,'utf8'));
const binary=path.join(assets,'devbox-mcp-linux-x86_64');
const fixture=await mkdtemp(path.join(os.tmpdir(),'devbox-native-signed-'));
try {
  const root=path.join(fixture,'signed');
  const result=await runCheckedProcess(binary,['manage','init','--root',root,'--receipt',receipt,'--provenance',provenance,
    '--source',proof.sourceSha,'--target','linux-x86_64'],{timeoutMs:120000,maxCaptureChars:65536,label:'Native exact-artifact signed initialization'});
  const initialized=JSON.parse(result.stdout);assert.equal(initialized.policy,'signed-qualified');
  const before=await readFile(path.join(root,'run/native/config.json'),'utf8');
  await assert.rejects(runCheckedProcess(binary,['manage','promote','--root',root,'--binary',binary,'--allow-local-build'],
    {timeoutMs:15000,label:'Signed policy downgrade refusal'}),/DOWNGRADE_REFUSED/);
  assert.equal(await readFile(path.join(root,'run/native/config.json'),'utf8'),before);
  await rename(path.join(root,'run/native'),path.join(fixture,'retained-manager'));
  const marker=path.join(root,'workspace/keep.txt');await writeFile(marker,'existing-workspace');
  const output=path.join(fixture,'prepared');
  const preparation=await runCheckedProcess(binary,['manage','prepare','--root',root,'--output',output,
    '--receipt',receipt,'--provenance',provenance,'--source',proof.sourceSha,'--target','linux-x86_64'],
    {timeoutMs:120000,maxCaptureChars:65536,label:'Native signed preparation for an existing root'});
  const prepared=JSON.parse(preparation.stdout);
  assert.equal(prepared.prepared,true);assert.equal(prepared.initialized,false);
  assert.equal(prepared.policy,'signed-qualified');assert.equal(prepared.sha256,initialized.sha256);
  await assert.rejects(access(path.join(root,'run/native')),{code:'ENOENT'});
  assert.equal(await readFile(marker,'utf8'),'existing-workspace');
  const config=JSON.parse(await readFile(path.join(output,'run/native/config.json'),'utf8'));
  assert.equal(config.root,root);assert.equal(config.current.file,JSON.parse(before).current.file);
  await assert.rejects(runCheckedProcess(binary,['manage','start','--root',output],
    {timeoutMs:5000,label:'Signed preparation cannot activate from the staging root'}),/CONFIG_INVALID/);
  const report={ok:true,source:proof.sourceSha,sha256:initialized.sha256,policy:initialized.policy,
    checks:['actual GitHub attestations','source/workflow/hosted-runner binding','authenticated binary and receipt',
      'policy downgrade rejection','signed preparation preserves the existing root','prepared root cannot start before activation']};
  await writeFile('.cpp-build/native-signed-install-result.json',JSON.stringify(report,null,2));console.log(JSON.stringify(report));
} finally {await rm(fixture,{recursive:true,force:true});}
