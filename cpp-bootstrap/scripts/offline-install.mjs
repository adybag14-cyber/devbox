import assert from 'node:assert/strict';
import {cp,mkdir,mkdtemp,open,readFile,realpath,rm,writeFile,access} from 'node:fs/promises';
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
const root=path.join(fixture,'portable spaces \u03a9'),empty=path.join(fixture,'empty-path');
await mkdir(empty);
const env=Object.fromEntries(Object.entries(process.env).filter(([key])=>['SYSTEMROOT','WINDIR','SYSTEMDRIVE','TEMP','TMP','TMPDIR','HOME','USERPROFILE'].includes(key.toUpperCase())));
env.PATH=empty;
env.LOCALAPPDATA=path.join(fixture,'user-profile');env.APPDATA=env.LOCALAPPDATA;
env.HOME=env.LOCALAPPDATA;env.USERPROFILE=env.LOCALAPPDATA;
await mkdir(env.LOCALAPPDATA);
const userRoot=process.platform==='win32'?path.join(env.LOCALAPPDATA,'Programs','Devbox')
  :process.platform==='darwin'?path.join(env.HOME,'Library','Application Support','Devbox')
  :path.join(env.HOME,'.local','share','devbox');
const run=(file,args,timeoutMs=60000)=>runCheckedProcess(file,args,{cwd:fixture,env,timeoutMs,maxCaptureChars:65536,label:'Offline native installation qualification'});
const port=await new Promise((resolve,reject)=>{const s=net.createServer();s.once('error',reject);s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>resolve(p));});});
const binary=path.join(root,'bin','devbox-mcp'+(process.platform==='win32'?'.exe':''));
async function waitStopped(directory=root) {
  const command=path.join(directory,'bin',path.basename(binary));
  const until=Date.now()+15000;
  do {
    if(!JSON.parse((await run(command,['manage','status','--root',directory])).stdout).running)return;
    await new Promise(resolve=>setTimeout(resolve,100));
  } while(Date.now()<until);
  throw new Error('Owned offline fixture did not stop; preserve its directory');
}
try {
  assert.deepEqual(JSON.parse((await run(installer,['--inspect'])).stdout),metadata.payload);
  await run(installer,['--portable',root,'--port',String(port)]);
  const configFile=path.join(root,'run/native/config.json');
  const config=JSON.parse(await readFile(configFile,'utf8'));
  assert.equal(config.policy,'bundled-qualified');
  assert.equal(config.current.source,metadata.source);
  assert.equal(config.current.target,metadata.target);
  assert.equal(config.environment.PORT,String(port));
  const initialStatus=JSON.parse((await run(binary,['manage','status','--root',root])).stdout);
  assert.equal(initialStatus.running,true,'Default start must survive the installer process exit');
  assert.equal(initialStatus.healthy,true);
  const optional=metadata.payload.files.find(item=>/^devbox-rg(?:\.exe)?$/u.test(item.name));
  if(optional) {
    const tool=path.join(root,'bin',optional.name);
    assert.equal(createHash('sha256').update(await readFile(tool)).digest('hex'),optional.sha256);
    assert.match((await run(tool,['--version'])).stdout,/features:-pcre2/u);
    await access(path.join(root,'share','ripgrep-NOTICES.txt'));
  }
  for(const name of ['node','node.exe','python','python.exe','python3','python3.exe','docker','docker.exe'])
    await assert.rejects(access(path.join(root,'bin',name)),{code:'ENOENT'});
  await assert.rejects(run(installer,['--portable',root,'--no-start']),/NEW_USER_DIRECTORY_REQUIRED/u);
  assert.deepEqual(JSON.parse(await readFile(configFile,'utf8')),config);
  await assert.rejects(run(binary,['manage','promote','--root',root,'--binary',binary,'--allow-local-build']),/DOWNGRADE_REFUSED/u);
  assert.deepEqual(JSON.parse(await readFile(configFile,'utf8')),config);
  for(const file of ['THIRD_PARTY_NOTICES.txt','dependency-inventory.json','sbom.spdx.json','build-manifest.json','OFFLINE.md','CORE_SERVER.md'])
    await access(path.join(root,'share',file));
  const started=JSON.parse((await run(binary,['manage','start','--root',root])).stdout);
  assert.equal(started.healthy,true);
  const live=await fetch(`http://127.0.0.1:${port}/`,{signal:AbortSignal.timeout(5000)});
  assert.equal(live.status,200);
  assert.equal((await live.json()).build.binarySha256,config.current.sha256);
  await run(binary,['manage','stop','--root',root]);
  await waitStopped();
  // Reproduce the incidental metadata that blocked the old checkout-based
  // launcher, with no Git or other developer tools available to the runtime.
  const incidental=path.join(root,'undefined','native-artifact-fixture','download-source-private.json');
  const incidentalContent=JSON.stringify({artifact_id:'fixture-only',download_url:'https://example.invalid/not-fetched'});
  await mkdir(path.dirname(incidental),{recursive:true});
  await writeFile(incidental,incidentalContent);
  const resumed=JSON.parse((await run(binary,['manage','start','--root',root])).stdout);
  assert.equal(resumed.healthy,true,'unrelated metadata must not prevent a fresh supervisor start');
  assert.notEqual(resumed.child.generation,started.child.generation);
  const restarted=JSON.parse((await run(binary,['manage','restart','--root',root])).stdout);
  assert.equal(restarted.healthy,true);
  assert.equal(await readFile(incidental,'utf8'),incidentalContent);
  await run(binary,['manage','stop','--root',root]);
  await waitStopped();
  // Ignoring unrelated files must not weaken verification of the executable
  // selected for launch. Mutate only the stopped fixture's private runtime.
  // The installed CLI can be a hard link to that runtime, so use an unchanged
  // verified controller copy and close all write handles before invoking it.
  const controller=path.join(fixture,'verified-controller'+(process.platform==='win32'?'.exe':''));
  await cp(binary,controller);
  assert.equal(createHash('sha256').update(await readFile(controller)).digest('hex'),config.current.sha256);
  const runtimeFile=await open(config.current.file,'r+');
  const stoppedConfig=await readFile(configFile,'utf8');
  const originalByte=Buffer.alloc(1);
  const runtimeOffset=(await runtimeFile.stat()).size-1;
  try {
    await runtimeFile.read(originalByte,0,1,runtimeOffset);
    await runtimeFile.write(Buffer.from([originalByte[0]^1]),0,1,runtimeOffset);
  } finally { await runtimeFile.close(); }
  try {
    await assert.rejects(run(controller,['manage','start','--root',root]),/NATIVE_STORED_CANDIDATE_IDENTITY_MISMATCH/u);
    assert.equal(await readFile(configFile,'utf8'),stoppedConfig,'invalid selected runtime does not change restart intent');
    assert.equal(JSON.parse((await run(controller,['manage','status','--root',root])).stdout).running,false);
  } finally {
    const restore=await open(config.current.file,'r+');
    try { await restore.write(originalByte,0,1,runtimeOffset); }
    finally { await restore.close(); }
  }
  const restored=JSON.parse((await run(binary,['manage','start','--root',root])).stdout);
  assert.equal(restored.healthy,true,'the original verified executable remains usable after restoring fixture bytes');
  await run(binary,['manage','stop','--root',root]);
  await waitStopped();
  const settings=path.join(fixture,'operator.env');
  await writeFile(settings,`HOST=127.0.0.1\nPORT=${port}\nMCP_AUTH_MODE=none\nMCP_EXEC_MAX_CONCURRENT=2\n`,{mode:0o600});
  await run(installer,['--user','--no-start','--env-file',settings]);
  const userConfig=JSON.parse(await readFile(path.join(userRoot,'run/native/config.json'),'utf8'));
  assert.equal(userConfig.root,await realpath(userRoot));
  assert.equal(userConfig.policy,'bundled-qualified');
  assert.equal(userConfig.current.source,metadata.source);
  assert.equal(userConfig.environment.PORT,String(port));
  assert.equal(userConfig.environment.MCP_EXEC_MAX_CONCURRENT,'2');
  assert.equal(JSON.parse((await run(path.join(userRoot,'bin',path.basename(binary)),
    ['manage','status','--root',userRoot])).stdout).running,false);
  const invalidCompanions=path.join(fixture,'invalid-companions.json');
  await writeFile(invalidCompanions,JSON.stringify([{name:'invalid',program:'relative-command',args:[]}]));
  const invalidRoot=path.join(fixture,'invalid-companion-install');
  await assert.rejects(run(installer,['--portable',invalidRoot,'--no-start','--companions',invalidCompanions]),
    /Companion program must be absolute/u);
  await assert.rejects(access(invalidRoot));
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
      'portable user directory with spaces and Unicode','default user-only directory',
      'operator environment preserved','invalid companion rejected before publishing',
      'default startup survives installer exit','no-start remains stopped',
      'existing installation preserved','local health and exact binary',
      'unrelated download metadata survives fresh startup and restart',
      'modified installed runtime rejected before launch','restored original runtime starts',
      'native stop','no development-policy downgrade','tamper rejected before extraction']};
  for(const item of metadata.files) {
    const bytes=await readFile(path.join(directory,item.file));
    assert.equal(bytes.length,item.bytes);
    assert.equal(createHash('sha256').update(bytes).digest('hex'),item.sha256);
  }
  await writeFile(path.join(directory,'offline-test-result.json'),JSON.stringify(receipt,null,2)+'\n');
  console.log(JSON.stringify(receipt));
} finally {
  for(const directory of [root,userRoot]) {
    const initialized=await access(path.join(directory,'run/native/config.json')).then(()=>true,error=>{
      if(error.code==='ENOENT')return false;throw error;
    });
    if(initialized) {
      const command=path.join(directory,'bin',path.basename(binary));
      const status=JSON.parse((await run(command,['manage','status','--root',directory])).stdout);
      if(status.running)await run(command,['manage','stop','--root',directory]);
      await waitStopped(directory);
    }
  }
  await rm(fixture,{recursive:true,force:true});
}
