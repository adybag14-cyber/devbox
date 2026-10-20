import assert from 'node:assert/strict';
import {cp,mkdir,mkdtemp,open,readFile,rm,writeFile,access} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import {runCheckedProcess} from '../../src/mcp-implementation.js';

const packageRoot=path.resolve(process.argv[2]||'.cpp-build/package');
const directory=path.join(packageRoot,'offline');
const metadata=JSON.parse(await readFile(path.join(directory,'offline-manifest.json'),'utf8'));
const installer=path.join(directory,metadata.installer);
const fixture=await mkdtemp(path.join(os.tmpdir(),'devbox-offline-install-'));
const root=path.join(fixture,'portable'),empty=path.join(fixture,'empty-path');
await mkdir(empty);
const env=Object.fromEntries(Object.entries(process.env).filter(([key])=>['SYSTEMROOT','WINDIR','SYSTEMDRIVE','TEMP','TMP','TMPDIR','HOME','USERPROFILE'].includes(key.toUpperCase())));
env.PATH=empty;
env.LOCALAPPDATA=path.join(fixture,'user-profile');env.APPDATA=env.LOCALAPPDATA;
await mkdir(env.LOCALAPPDATA);
const run=(file,args,timeoutMs=60000)=>runCheckedProcess(file,args,{cwd:fixture,env,timeoutMs,maxCaptureChars:65536,label:'Offline native installation qualification'});
const port=await new Promise((resolve,reject)=>{const s=net.createServer();s.once('error',reject);s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>resolve(p));});});
const binary=path.join(root,'bin','devbox-mcp'+(process.platform==='win32'?'.exe':''));
async function waitStopped() {
  const until=Date.now()+15000;
  do {
    if(!JSON.parse((await run(binary,['manage','status','--root',root])).stdout).running)return;
    await new Promise(resolve=>setTimeout(resolve,100));
  } while(Date.now()<until);
  throw new Error('Owned offline fixture did not stop; preserve its directory');
}
let initialized=false;
try {
  assert.deepEqual(JSON.parse((await run(installer,['--inspect'])).stdout),metadata.payload);
  await run(installer,['--portable',root,'--port',String(port),'--no-start']);
  initialized=true;
  const configFile=path.join(root,'run/native/config.json');
  const config=JSON.parse(await readFile(configFile,'utf8'));
  assert.equal(config.policy,'bundled-qualified');
  assert.equal(config.current.source,metadata.source);
  assert.equal(config.current.target,metadata.target);
  assert.equal(config.environment.PORT,String(port));
  assert.equal(JSON.parse((await run(binary,['manage','status','--root',root])).stdout).running,false);
  await assert.rejects(run(installer,['--portable',root,'--no-start']),/NEW_USER_DIRECTORY_REQUIRED/u);
  assert.deepEqual(JSON.parse(await readFile(configFile,'utf8')),config);
  await assert.rejects(run(binary,['manage','promote','--root',root,'--binary',binary,'--allow-local-build']),/DOWNGRADE_REFUSED/u);
  assert.deepEqual(JSON.parse(await readFile(configFile,'utf8')),config);
  for(const file of ['THIRD_PARTY_NOTICES.txt','dependency-inventory.json','sbom.spdx.json','build-manifest.json','OFFLINE.md'])
    await access(path.join(root,'share',file));
  const started=JSON.parse((await run(binary,['manage','start','--root',root])).stdout);
  assert.equal(started.healthy,true);
  const live=await fetch(`http://127.0.0.1:${port}/`,{signal:AbortSignal.timeout(5000)});
  assert.equal(live.status,200);
  assert.equal((await live.json()).build.binarySha256,config.current.sha256);
  await run(binary,['manage','stop','--root',root]);
  await waitStopped();
  const corrupted=path.join(fixture,'corrupted');
  await cp(directory,corrupted,{recursive:true});
  const damaged=process.platform==='darwin'
    ?path.join(corrupted,'Devbox.app/Contents/Resources/devbox.payload')
    :path.join(corrupted,metadata.installer);
  const file=await open(damaged,'r+');
  try {
    const size=(await file.stat()).size;
    const offset=process.platform==='darwin'?0:size-24-metadata.payload.payloadBytes;
    const byte=Buffer.alloc(1);const at=offset+Math.floor(metadata.payload.payloadBytes/2);
    await file.read(byte,0,1,at);byte[0]^=1;await file.write(byte,0,1,at);
  } finally {await file.close();}
  const rejected=path.join(fixture,'must-not-exist');
  await assert.rejects(run(path.join(corrupted,metadata.installer),['--portable',rejected,'--no-start']),/PAYLOAD_DIGEST_MISMATCH/u);
  await assert.rejects(access(rejected));
  const receipt={ok:true,source:metadata.source,target:metadata.target,policy:config.policy,
    files:metadata.files,
    checks:['empty developer-tool PATH','compiled payload identity','native source and contract verification',
      'portable user directory','existing installation preserved','local health and exact binary',
      'native stop','no development-policy downgrade','tamper rejected before extraction']};
  for(const item of metadata.files) {
    const bytes=await readFile(path.join(directory,item.file));
    assert.equal(bytes.length,item.bytes);
    assert.equal(createHash('sha256').update(bytes).digest('hex'),item.sha256);
  }
  await writeFile(path.join(directory,'offline-test-result.json'),JSON.stringify(receipt,null,2)+'\n');
  console.log(JSON.stringify(receipt));
} finally {
  if(initialized) {
    const status=JSON.parse((await run(binary,['manage','status','--root',root])).stdout);
    if(status.running)await run(binary,['manage','stop','--root',root]);
    await waitStopped();
  }
  await rm(fixture,{recursive:true,force:true});
}
