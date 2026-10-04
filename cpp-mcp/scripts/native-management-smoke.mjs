import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, mkdir, readFile, writeFile, rm} from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {runCheckedProcess} from '../../src/mcp-implementation.js';

const binary = path.resolve(process.env.DEVBOX_CPP_BINARY || process.argv[2] || '');
const badCandidate=process.env.DEVBOX_MANAGEMENT_BAD_CANDIDATE||path.join(path.dirname(binary),'devbox-management-bad-candidate'+(process.platform==='win32'?'.exe':''));
const fixture = await mkdtemp(path.join(os.tmpdir(), 'devbox-native-management-'));
const root = path.join(fixture, 'native root with spaces');
const emptyPath = path.join(fixture, 'empty-path'); await mkdir(emptyPath);
const port = await new Promise((resolve, reject) => {
  const server = net.createServer(); server.once('error', reject);
  server.listen(0, '127.0.0.1', () => { const value = server.address().port; server.close(() => resolve(value)); });
});
const env = Object.fromEntries(Object.entries(process.env).filter(([key]) =>
  ['SYSTEMROOT', 'WINDIR', 'SYSTEMDRIVE', 'TEMP', 'TMP', 'TMPDIR', 'HOME', 'USERPROFILE', 'LANG', 'LC_ALL'].includes(key.toUpperCase())));
env.PATH = emptyPath;
const invoke = async (command, ...args) => {
  const result = await runCheckedProcess(binary, ['manage', command, '--root', root, ...args],
    {env, cwd: fixture, timeoutMs: 120000, maxCaptureChars: 128*1024, label: `Native management ${command}`});
  return JSON.parse(result.stdout);
};
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
let foreground, exit;
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
  const startedAt = performance.now();
  foreground = spawn(binary,['manage','run','--root',root,'--timeout-ms','20000'], {env,cwd:fixture,windowsHide:true,stdio:['ignore','pipe','pipe']});
  let output=''; for(const stream of [foreground.stdout,foreground.stderr]) stream.on('data',value=>output=(output+value).slice(-16000));
  exit = new Promise((resolve,reject)=>{foreground.once('exit',(code,signal)=>resolve({code,signal}));foreground.once('error',reject);});
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
  // Kill only the ChildProcess that this fixture launched, then recover through
  // stored birth/executable identities without duplicating an orphan frontend.
  foreground.kill('SIGKILL'); await exit; foreground=null;
  const recoveredOwner=await invoke('start');assert.equal(recoveredOwner.healthy,true);
  assert.equal(await readFile(marker,'utf8'),'x');
  await invoke('stop');
  const stoppedBy=performance.now()+10000;
  while((await invoke('status')).running&&performance.now()<stoppedBy)await delay(100);
  assert.equal((await invoke('status')).running,false);
  // Detached lifecycle is also native and retains exact owner identity.
  const detached=await invoke('start'); assert.equal(detached.healthy,true);
  await invoke('stop');
  const deadline=performance.now()+10000;
  while((await invoke('status')).running && performance.now()<deadline) await delay(100);
  assert.equal((await invoke('status')).running,false);
  console.log(JSON.stringify({ok:true,platform:process.platform,startupMs,nodeOnRuntimePath:false,checks:[
    'empty-PATH native initialization/startup','live MCP file effect','duplicate suppression','restart/state preservation',
    'busy request and durable-job drain refusal','frontend crash recovery without replay','supervisor crash recovery',
    'independent owned companion lifecycle','service definition without activation','missing proof rejection',
    'failed candidate startup rollback','foreground and detached stop']}));
} finally {
  try { if((await invoke('status')).running) await invoke('stop'); } catch(error) { console.error(String(error)); }
  if(foreground && foreground.exitCode===null) {
    // Only this fixture's owned child, never a name/group lookup.
    foreground.kill('SIGTERM'); await Promise.race([exit,delay(15000)]);
    if(foreground.exitCode===null) throw new Error(`Owned fixture did not stop; evidence retained at ${fixture}`);
  }
  const last=await invoke('status').catch(()=>({running:false}));
  if(last.running) throw new Error(`Native owner still running; evidence retained at ${fixture}`);
  await rm(fixture,{recursive:true,force:true});
}
