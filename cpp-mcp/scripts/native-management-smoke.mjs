import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, mkdir, open, readFile, writeFile, rm} from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {runCheckedProcess} from '../../src/mcp-implementation.js';

const binary = path.resolve(process.env.DEVBOX_CPP_BINARY || process.argv[2] || '');
const badCandidate=process.env.DEVBOX_MANAGEMENT_BAD_CANDIDATE||path.join(path.dirname(binary),'devbox-management-bad-candidate'+(process.platform==='win32'?'.exe':''));
const fixture = await mkdtemp(path.join(os.tmpdir(), 'devbox-native-management-long-path-regression-'));
const root = path.join(fixture, 'native root with spaces');
const emptyPath = path.join(fixture, 'empty-path'); await mkdir(emptyPath);
const port = await new Promise((resolve, reject) => {
  const server = net.createServer(); server.once('error', reject);
  server.listen(0, '127.0.0.1', () => { const value = server.address().port; server.close(() => resolve(value)); });
});
const env = Object.fromEntries(Object.entries(process.env).filter(([key]) =>
  ['SYSTEMROOT', 'WINDIR', 'SYSTEMDRIVE', 'TEMP', 'TMP', 'TMPDIR', 'HOME', 'USERPROFILE', 'LANG', 'LC_ALL'].includes(key.toUpperCase())));
env.PATH = emptyPath;
if(process.platform==='win32') {
  env.LOCALAPPDATA=path.join(fixture,'account-profile');await mkdir(env.LOCALAPPDATA);
  env.APPDATA=env.LOCALAPPDATA;
}
const invoke = async (command, ...args) => {
  const result = await runCheckedProcess(binary, ['manage', command, '--root', root, ...args],
    {env, cwd: fixture, timeoutMs: 120000, maxCaptureChars: 128*1024, label: `Native management ${command}`});
  return JSON.parse(result.stdout);
};
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const awaitWithin=async(promise,ms,message)=>{
  let timer;
  try {
    return await Promise.race([promise,new Promise((_,reject)=>{
      timer=setTimeout(()=>reject(new Error(message)),ms);
    })]);
  } finally { clearTimeout(timer); }
};
let foreground, exit, output='';
const launchForeground=()=>{
  output='';
  foreground=spawn(binary,['manage','run','--root',root,'--timeout-ms','20000'],
    {env,cwd:fixture,windowsHide:true,stdio:['ignore','pipe','pipe']});
  for(const stream of [foreground.stdout,foreground.stderr])
    stream.on('data',value=>output=(output+value).slice(-16000));
  exit=new Promise((resolve,reject)=>{
    foreground.once('exit',(code,signal)=>resolve({code,signal}));
    foreground.once('error',reject);
  });
  exit.catch(()=>{});
};
let counter = 0;
const rpc = async (name, args = {}) => {
  const response = await fetch(`http://127.0.0.1:${port}/mcp`, {method:'POST', headers:{'content-type':'application/json', accept:'application/json, text/event-stream'},
    body:JSON.stringify({jsonrpc:'2.0', id:++counter, method:'tools/call', params:{name, arguments:args}}), signal:AbortSignal.timeout(30000)});
  assert.equal(response.status,200,await response.clone().text());
  const text=await response.text();
  const value=response.headers.get('content-type')?.includes('text/event-stream')
    ? text.split(/\r?\n/u).filter(line=>line.startsWith('data:')).map(line=>JSON.parse(line.slice(5))).find(value=>value.result||value.error)
    : JSON.parse(text);
  assert(value,`Missing MCP response: ${text}`); assert(!value.error,JSON.stringify(value));
  assert.equal(value.result.isError,false,JSON.stringify(value)); return value.result.structuredContent.data;
};
try {
  const environment = path.join(fixture,'config.env');
  await writeFile(environment, `ENABLE_HOST_EXEC=true\nDEVBOX_AUTO_START=false\nHOST_PROGRAM_ALLOWLIST_REPLACE=true\nHOST_PROGRAM_ALLOWLIST=node\nNODE_EXE=${process.execPath}\n`);
  const companionFile=path.join(fixture,'companions.json'),companionEffect=path.join(fixture,'companion-starts');
  await writeFile(companionFile,JSON.stringify([{name:'owned-fixture',program:process.execPath,
    args:['-e',"require('fs').appendFileSync(process.argv[1],'x');setInterval(()=>{},1000)",companionEffect]}]));
  const initialized = await invoke('init', '--binary', binary, '--allow-local-build', '--port', String(port), '--env-file', environment,'--companions',companionFile);
  assert.equal(initialized.policy,'local-development');
  // An incidental download receipt blocked the former Git-checkout launcher
  // after a reboot. Native supervision verifies its pinned runtime instead.
  const incidentalFile=path.join(root,'undefined','native-artifact-fixture','download-source-private.json');
  const incidentalContent=JSON.stringify({artifact_id:'fixture-only',download_url:'https://example.invalid/not-fetched'});
  await mkdir(path.dirname(incidentalFile),{recursive:true});
  await writeFile(incidentalFile,incidentalContent);
  const startedAt = performance.now();
  launchForeground();
  const by = performance.now()+20000;
  let first;
  while(performance.now()<by) {
    assert.equal(foreground.exitCode,null,output);
    first = await invoke('status'); if(first.healthy)break; await delay(100);
  }
  assert.equal(first.healthy,true,JSON.stringify(first));
  const startupMs = performance.now()-startedAt;
  assert.equal(first.owner.pid,foreground.pid);
  const companionBy=performance.now()+10000;
  while(!(first.companions?.[0]?.healthy) && performance.now()<companionBy) {await delay(100);first=await invoke('status');}
  assert.equal(first.companions[0].healthy,true);const companionPid=first.companions[0].process.pid;
  const duplicate = await invoke('start'); assert.equal(duplicate.owner.pid,foreground.pid);
  const file = path.join(root,'workspace','roundtrip.txt');
  await rpc('devbox_write_file_atomic',{path:file,content_base64:Buffer.from('native-core-effect').toString('base64'),expected_file_sha256:'missing'});
  assert.equal(await readFile(file,'utf8'),'native-core-effect');
  await invoke('restart');
  const restarted=await invoke('status'); assert.notEqual(restarted.child.generation,first.child.generation);
  assert.equal(restarted.healthy,true); assert.equal(await readFile(file,'utf8'),'native-core-effect');
  assert.equal(await readFile(incidentalFile,'utf8'),incidentalContent,'incidental metadata does not block startup or restart');
  assert.equal(restarted.companions[0].process.pid,companionPid,'frontend restart preserves healthy companion');
  const longWait=rpc('devbox_wait',{seconds:10}); longWait.catch(()=>{}); await delay(500);
  await assert.rejects(invoke('stop','--timeout-ms','2000'), /DRAIN_BUSY_OR_UNCONFIRMED/);
  await longWait;
  assert.equal((await invoke('status')).owner.pid,foreground.pid,'busy refusal preserves owned server');
  await rpc('devbox_task_put',{task_id:'native_lifecycle',expected_revision:0,state:{phase:'test'}});
  const marker=path.join(root,'workspace','once.txt');
  const job=await rpc('devbox_job_submit',{task_id:'native_lifecycle',operation_id:'once',program:'node',
    args:['-e',"setTimeout(()=>require('fs').appendFileSync(process.argv[1],'x'),12000)",marker]});
  await delay(300);
  await assert.rejects(invoke('stop','--timeout-ms','2000'), /DRAIN_BUSY_OR_UNCONFIRMED/);
  let completed;const jobBy=performance.now()+30000;
  do {completed=await rpc('devbox_job_status',{job_id:job.id,wait_seconds:10});}
  while(['queued','running'].includes(completed.status)&&performance.now()<jobBy);
  assert.equal(completed.status,'succeeded',JSON.stringify(completed));
  assert.equal(await readFile(marker,'utf8'),'x','durable effect completed exactly once');
  const liveStatus=await rpc('devbox_status');assert.equal(liveStatus.guardian.implementation,'cpp');
  assert.equal(liveStatus.guardian.stale,false,'independent native heartbeat is visible to MCP clients');
  const beforeCrash=await invoke('status');
  const observed=await (await fetch(`http://127.0.0.1:${port}/`)).json();
  assert.equal(observed.build.deploymentGeneration,beforeCrash.child.generation);
  assert.equal(observed.build.binarySha256,initialized.sha256);
  process.kill(beforeCrash.child.pid,'SIGKILL'); // Exact child of this fixture, identity just verified.
  const recoveredBy=performance.now()+20000;let recovered;
  do {await delay(150);recovered=await invoke('status');}
  while((!recovered.healthy||recovered.child?.generation===beforeCrash.child.generation)&&performance.now()<recoveredBy);
  assert.equal(recovered.healthy,true);assert.notEqual(recovered.child.generation,beforeCrash.child.generation);
  assert.equal(recovered.companions[0].process.pid,companionPid);
  assert.equal(await readFile(marker,'utf8'),'x','frontend crash does not repeat completed effects');
  const configPath=path.join(root,'run/native/config.json');
  const config=JSON.parse(await readFile(configPath,'utf8'));
  assert.equal(config.current.sha256,initialized.sha256);
  const service=path.join(fixture,process.platform==='win32'?'devbox.xml':'devbox.service');
  const serviceKind=process.platform==='win32'?'windows':process.platform==='darwin'?'launchd':'systemd';
  await invoke('service-file','--service',serviceKind,'--output',service);
  const definition=await readFile(service,'utf8');
  assert(definition.includes(serviceKind==='launchd'?'<string>manage</string>':'manage run --root'));
  if(serviceKind==='systemd')assert(!definition.includes('ExecStop='),
    'systemd shutdown must signal the supervisor, not persist an operator stop');
  if(process.platform==='darwin') await runCheckedProcess('/usr/bin/plutil',['-lint',service],{timeoutMs:10000,label:'Native LaunchAgent XML validation'});
  assert(!definition.includes('node '));
  await writeFile(service,'unrelated existing service definition');
  await assert.rejects(invoke('service-file','--service',serviceKind,'--output',service));
  assert.equal(await readFile(service,'utf8'),'unrelated existing service definition');
  const stateBefore = JSON.parse(await readFile(configPath,'utf8'));
  await assert.rejects(invoke('promote','--binary',binary),/Signed installation requires/);
  assert.deepEqual(JSON.parse(await readFile(configPath,'utf8')),stateBefore,'invalid proof changes no active selection');
  await assert.rejects(invoke('promote','--binary',badCandidate,'--allow-local-build'),/rolledBack/);
  const rolledBack=await invoke('status');assert.equal(rolledBack.healthy,true);
  assert.equal(rolledBack.candidateSha256,initialized.sha256,'failed startup restores prior immutable binary');
  assert.equal(await readFile(marker,'utf8'),'x');
  if(process.platform!=='win32') {
    // Signal only the ChildProcess owned by this fixture. A service-manager
    // termination must allow the next foreground start without manual repair.
    assert.equal(rolledBack.owner.pid,foreground.pid);
    foreground.kill('SIGTERM');
    const terminated=await awaitWithin(exit,25000,'Owned signal shutdown did not finish');
    assert.equal(terminated.code,0,output);foreground=null;
    assert.equal(JSON.parse(await readFile(configPath,'utf8')).desired,true,
      'service-manager shutdown preserves restart intent');
    launchForeground();
    const resumedBy=performance.now()+20000;let resumed;
    do {
      assert.equal(foreground.exitCode,null,output);
      resumed=await invoke('status');
      if(resumed.healthy&&resumed.owner.pid===foreground.pid)break;
      await delay(100);
    }while(performance.now()<resumedBy);
    assert.equal(resumed.healthy,true);assert.equal(resumed.owner.pid,foreground.pid);
    assert.equal(await readFile(marker,'utf8'),'x','service restart does not replay completed effects');
    assert.equal(await readFile(incidentalFile,'utf8'),incidentalContent,'service restart preserves unrelated metadata');
  }
  // Kill only the ChildProcess that this fixture launched, then recover through
  // stored birth/executable identities without duplicating an orphan frontend.
  foreground.kill('SIGKILL'); await exit; foreground=null;
  const recoveredOwner=await invoke('start');assert.equal(recoveredOwner.healthy,true);
  assert.equal(await readFile(marker,'utf8'),'x');
  assert.equal(await readFile(incidentalFile,'utf8'),incidentalContent,'supervisor recovery preserves unrelated metadata');
  await invoke('stop');
  const stoppedBy=performance.now()+10000;
  while((await invoke('status')).running&&performance.now()<stoppedBy)await delay(100);
  assert.equal((await invoke('status')).running,false);
  assert.equal(JSON.parse(await readFile(configPath,'utf8')).desired,false,
    'explicit operator stop remains persistent');
  await runCheckedProcess(binary,['manage','run','--root',root],
    {env,cwd:fixture,timeoutMs:5000,label:'Foreground run after persistent operator stop'});
  assert.equal((await invoke('status')).running,false,'foreground service start respects an operator stop');
  const stoppedConfig=await readFile(configPath,'utf8');
  const runtimeFile=await open(config.current.file,'r+');
  const originalByte=Buffer.alloc(1),runtimeOffset=(await runtimeFile.stat()).size-1;
  try {
    await runtimeFile.read(originalByte,0,1,runtimeOffset);
    await runtimeFile.write(Buffer.from([originalByte[0]^1]),0,1,runtimeOffset);
  } finally { await runtimeFile.close(); }
  try {
    await assert.rejects(invoke('start'),/NATIVE_STORED_CANDIDATE_IDENTITY_MISMATCH/);
    assert.equal(await readFile(configPath,'utf8'),stoppedConfig,'invalid selected runtime does not change restart intent');
    assert.equal((await invoke('status')).running,false,'invalid selected runtime does not launch a supervisor');
  } finally {
    const restore=await open(config.current.file,'r+');
    try { await restore.write(originalByte,0,1,runtimeOffset); }
    finally { await restore.close(); }
  }
  // Detached lifecycle is also native and retains exact owner identity.
  const detached=await invoke('start'); assert.equal(detached.healthy,true);
  await invoke('stop');
  const deadline=performance.now()+10000;
  while((await invoke('status')).running && performance.now()<deadline) await delay(100);
  assert.equal((await invoke('status')).running,false);
  console.log(JSON.stringify({ok:true,platform:process.platform,startupMs,nodeOnRuntimePath:false,checks:[
    'empty-PATH native initialization/startup','unrelated download metadata survives startup and recovery',
    'live MCP file effect','duplicate suppression','restart/state preservation',
    'busy request and durable-job drain refusal','frontend crash recovery without replay','supervisor crash recovery',
    'independent owned companion lifecycle','service definition without activation','missing proof rejection',
    'failed candidate startup rollback','foreground and detached stop',
    'modified selected runtime rejected without changing restart intent',
    ...(process.platform==='win32'?[]:['service signal shutdown and foreground restart without replay']),
    'persistent explicit operator stop']}));
} finally {
  try { if((await invoke('status')).running) await invoke('stop'); } catch(error) { console.error(String(error)); }
  if(foreground && foreground.exitCode===null) {
    // Only this fixture's owned child, never a name/group lookup.
    foreground.kill('SIGTERM');
    await awaitWithin(exit,15000,`Owned fixture did not stop; evidence retained at ${fixture}`);
    if(foreground.exitCode===null) throw new Error(`Owned fixture did not stop; evidence retained at ${fixture}`);
  }
  const last=await invoke('status').catch(()=>({running:false}));
  if(last.running) throw new Error(`Native owner still running; evidence retained at ${fixture}`);
  await rm(fixture,{recursive:true,force:true});
}
