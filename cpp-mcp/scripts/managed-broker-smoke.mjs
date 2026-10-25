import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,mkdir,readFile,writeFile,rm} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {runCheckedProcess} from '../../src/mcp-implementation.js';

assert.equal(process.platform,'win32');
const binary=path.resolve(process.env.DEVBOX_CPP_BINARY||process.argv[2]||'');
const fixture=await mkdtemp(path.join(os.tmpdir(),'devbox-managed-broker-'));
const root=path.join(fixture,'native root'),empty=path.join(fixture,'empty-path');
await mkdir(empty);
const env=Object.fromEntries(Object.entries(process.env).filter(([key])=>
  ['SYSTEMROOT','WINDIR','SYSTEMDRIVE','TEMP','TMP','USERPROFILE','HOME','LOCALAPPDATA','APPDATA'].includes(key.toUpperCase())));
env.PATH=empty;
const run=(file,args,timeoutMs=10000)=>runCheckedProcess(file,args,
  {env,cwd:fixture,timeoutMs,maxCaptureChars:256*1024,label:'Native managed desktop broker'});
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
let child,exited,output='';
try {
  const pipe='Devbox-Managed-Test-'+path.basename(fixture);
  const environment=path.join(fixture,'config.env');
  await writeFile(environment,'DEVBOX_COMPUTER_USE_PIPE='+pipe+'\n');
  await run(binary,['manage','init','--root',root,'--binary',binary,'--allow-local-build','--env-file',environment],30000);
  const config=JSON.parse(await readFile(path.join(root,'run/native/config.json'),'utf8'));
  const selected=config.current.file,hash=config.current.sha256,source=config.current.source;
  const args=['--managed-computer-use-broker',root,hash,source,pipe];
  child=spawn(selected,args,{env,cwd:fixture,windowsHide:true,stdio:['ignore','pipe','pipe']});
  for(const stream of [child.stdout,child.stderr])stream.on('data',value=>output=(output+value).slice(-4096));
  exited=new Promise((resolve,reject)=>{child.once('exit',resolve);child.once('error',reject);});exited.catch(()=>{});
  const status=path.join(root,'run/native/desktop-worker.json');
  let record;
  const until=performance.now()+10000;
  while(performance.now()<until) {
    assert.equal(child.exitCode,null,output);
    try{record=JSON.parse(await readFile(status,'utf8'));if(record.state==='running')break;}catch(error){if(error.code!=='ENOENT')throw error;}
    await delay(50);
  }
  assert.equal(record?.pid,child.pid,'The native executable itself owns the broker; no shell wrapper');
  assert.equal(record.implementation,'native C++23');assert.equal(record.sha256,hash);
  assert.equal(record.source,source);assert.equal(record.executable,selected);assert(record.sessionId>0);
  assert.match(record.instance,/^\d+$/u);assert.match(record.createdAtUtc,/\.\d{7}Z$/u);
  // Read-only broker transport: enumerate windows but retain no window titles or input data.
  JSON.parse((await run(selected,['--computer-use-probe',pipe])).stdout);
  const before=await readFile(status,'utf8');
  await assert.rejects(run(selected,args,6000));
  assert.equal(await readFile(status,'utf8'),before,'Duplicate launch preserves the running owner record');
  for(const bad of [
    ['--managed-computer-use-broker',root,'0'.repeat(64),source,pipe],
    ['--managed-computer-use-broker',root,hash,'0'.repeat(40),pipe],
    ['--managed-computer-use-broker',root,hash,source,pipe+'-other']])
    await assert.rejects(run(selected,bad),/MANAGED_COMPUTER_SELECTION_MISMATCH/u);
  await assert.rejects(run(binary,args),/MANAGED_COMPUTER_EXECUTABLE_MISMATCH/u);
  assert.equal(await readFile(status,'utf8'),before);
  assert.equal(child.exitCode,null,'Rejected launch must preserve the owned broker');
  console.log(JSON.stringify({ok:true,checks:['empty PATH','direct native broker ownership','read-only pipe probe',
    'duplicate refusal preserves owner','source/hash/pipe/self-path pinning'],desktopInput:false}));
} finally {
  if(child&&child.exitCode===null)child.kill('SIGTERM'); // Retained ChildProcess handle, only this fixture.
  if(exited) {
    let timer;
    try{await Promise.race([exited,new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('Owned broker did not exit; preserve '+fixture)),5000);})]);}
    finally{clearTimeout(timer);}
  }
  assert(path.isAbsolute(fixture)&&path.basename(fixture).startsWith('devbox-managed-broker-'));
  await rm(fixture,{recursive:true,force:true});
}
