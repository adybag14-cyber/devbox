import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {access, mkdtemp, mkdir, open, readFile, readlink, rename, writeFile, rm} from 'node:fs/promises';
import {createHash} from 'node:crypto';
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
const moveStoppedFixtureDirectory=async(from,to)=>{
  for(const file of [from,to]){const relative=path.relative(fixture,file);assert(relative&&!relative.startsWith('..'+path.sep)&&relative!=='..'&&!path.isAbsolute(relative));}
  const deadline=performance.now()+5000;
  for(;;){
    try{await rename(from,to);return;}
    catch(error){
      if(process.platform!=='win32'||!['EPERM','EBUSY'].includes(error.code)||performance.now()>=deadline)throw error;
      // Windows can retain an image/directory handle briefly after process exit.
      // Never retry a fixture move while its native owner is still running.
      assert.equal((await invoke('status')).running,false);
      await delay(100);
    }
  }
};
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
  if(process.platform==='linux') {
    // Stall only this fixture's identity-checked frontend. Its supervisor must
    // preserve unconfirmed work, then report recovery without replacing it.
    const beforeStall=await invoke('status');
    const observed=await (await fetch(`http://127.0.0.1:${port}/`)).json();
    assert.equal(observed.build.deploymentGeneration,beforeStall.child.generation);
    assert.equal(observed.build.binarySha256,initialized.sha256);
    const pid=beforeStall.child.pid;
    const birth=async()=>{const stat=await readFile(`/proc/${pid}/stat`,'utf8');return stat.slice(stat.lastIndexOf(')')+2).split(' ')[19];};
    const instance=await birth(), image=await readlink(`/proc/${pid}/exe`);
    const sameChild=async()=>{assert.equal(await birth(),instance);assert.equal(await readlink(`/proc/${pid}/exe`),image);};
    let suspended=false;
    try {
      await sameChild();process.kill(pid,'SIGSTOP');suspended=true;
      const stalledBy=performance.now()+45000;let stalled;
      do {await delay(200);stalled=await invoke('status');}
      while((stalled.phase!=='unhealthy'||!stalled.error?.includes('NATIVE_DRAIN_BUSY_OR_UNCONFIRMED'))&&performance.now()<stalledBy);
      assert.equal(stalled.phase,'unhealthy',JSON.stringify(stalled));
      assert.match(stalled.error,/NATIVE_DRAIN_BUSY_OR_UNCONFIRMED/);
      assert.equal(stalled.child.generation,beforeStall.child.generation);
      assert.equal(stalled.lastHealthFailure.reason,'probe_exception');
      assert.equal(stalled.lastHealthFailure.phase,'metadata');
      assert.equal(stalled.lastHealthFailure.child.generation,beforeStall.child.generation);
      assert(stalled.lastHealthFailure.consecutiveFailures>=1);
      assert.equal(stalled.restarts,beforeStall.restarts,'an unconfirmed health drain is not a restart');
      const heartbeat=JSON.parse(await readFile(path.join(root,'run/native/heartbeat.json'),'utf8'));
      assert(Date.parse(heartbeat.updatedAt)>Date.now()-15000,'heartbeat continues during a failed health drain');
    } finally {
      if(suspended){await sameChild();process.kill(pid,'SIGCONT');}
    }
    const recoveryBy=performance.now()+15000;let healthyAgain;
    do {await delay(150);healthyAgain=await invoke('status');}
    while((!healthyAgain.healthy||healthyAgain.phase!=='ready')&&performance.now()<recoveryBy);
    assert.equal(healthyAgain.healthy,true,JSON.stringify(healthyAgain));
    assert.equal(healthyAgain.phase,'ready','a recovered probe clears the stale unhealthy phase');
    assert(!Object.hasOwn(healthyAgain,'error'),'a recovered probe clears the stale health error');
    assert.deepEqual(healthyAgain.child,beforeStall.child,'health recovery does not replace a preserved frontend');
    assert.deepEqual(healthyAgain.lastRequest,beforeStall.lastRequest,'probe recovery preserves operator history');
    assert.equal(healthyAgain.lastHealthFailure.child.generation,beforeStall.child.generation,
      'recovery retains the failed probe evidence');

    // Make only this idle fixture's scheduler path unavailable. Unlike SIGSTOP,
    // its HTTP loop can still acknowledge drain, so a health repair is permitted.
    const beforeHealthRepair=await invoke('status');
    assert.equal(beforeHealthRepair.healthy,true);
    assert.deepEqual(beforeHealthRepair.child,beforeStall.child);
    const slots=path.join(root,'run','slots'),heldSlots=path.join(root,'run','slots-held-by-fixture');
    for(const file of [slots,heldSlots]) {
      const relative=path.relative(fixture,file);
      assert(relative&&!relative.startsWith('..'+path.sep)&&relative!=='..'&&!path.isAbsolute(relative));
    }
    await assert.rejects(access(heldSlots),{code:'ENOENT'});
    let moved=false,substituted=false,repair;
    try {
      await sameChild();process.kill(pid,'SIGSTOP');suspended=true;
      await rename(slots,heldSlots);moved=true;
      await writeFile(slots,'owned readiness fixture',{flag:'wx'});substituted=true;
      await sameChild();process.kill(pid,'SIGCONT');suspended=false;
      const repairBy=performance.now()+45000;
      do {await delay(150);repair=await invoke('status');}
      while(repair.lastHealthRecovery?.previousChild?.generation!==beforeHealthRepair.child.generation&&performance.now()<repairBy);
      assert.equal(repair.lastHealthRecovery?.previousChild?.generation,beforeHealthRepair.child.generation);
      assert.equal(repair.lastHealthRecovery.reason,'consecutive_readiness_failures');
      assert.equal(repair.lastHealthRecovery.probe.phase,'readiness');
      assert.equal(repair.lastHealthRecovery.probe.httpStatus,503);
      assert(repair.restarts>=beforeHealthRepair.restarts+1,'confirmed health replacement increments restart accounting');
    } finally {
      if(suspended){await sameChild();process.kill(pid,'SIGCONT');suspended=false;}
      if(substituted){assert.equal(await readFile(slots,'utf8'),'owned readiness fixture');await rm(slots);}
      if(moved)await rename(heldSlots,slots);
    }
    const repairedBy=performance.now()+25000;let repaired;
    do {await delay(150);repaired=await invoke('status');}
    while(!repaired.healthy&&performance.now()<repairedBy);
    assert.equal(repaired.healthy,true,JSON.stringify(repaired));
    assert.notEqual(repaired.child.generation,beforeHealthRepair.child.generation);
    assert.equal(repaired.companions[0].process.pid,companionPid,'health recovery preserves the healthy companion');
  }
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
  if(process.platform==='win32'||process.platform==='linux') {
    // A private development copy with an inert overlay gives this fixture a
    // second valid image identity without changing any production artifact.
    const alternate=path.join(fixture,'stable-selected-runtime'+(process.platform==='win32'?'.exe':''));
    await writeFile(alternate,Buffer.concat([await readFile(binary),Buffer.from('\nDEVBOX LOCAL MANAGEMENT FIXTURE\n')]),{mode:0o755});
    const alternateHash=createHash('sha256').update(await readFile(alternate)).digest('hex');
    await invoke('promote','--binary',alternate,'--allow-local-build');
    const committed=await invoke('status'),selection=JSON.parse(await readFile(configPath,'utf8'));
    assert.equal(committed.healthy,true);assert.equal(committed.candidateSha256,alternateHash);
    assert.equal(selection.previous.sha256,initialized.sha256);
    await assert.rejects(access(path.join(root,'run/native/handoff.json')),'promotion must already be committed');
    const observed=await (await fetch(`http://127.0.0.1:${port}/`)).json();
    assert.equal(observed.build.binarySha256,alternateHash);
    assert.equal(observed.build.deploymentGeneration,committed.child.generation);
    const selectedFile=selection.current.file,heldFile=selectedFile+'.fixture-held';
    assert.equal(path.dirname(selectedFile),path.join(root,'run/native/releases',alternateHash));
    // Move only this fixture's image within its own directory. The existing
    // mapped frontend remains alive, but its next pathname lookup must fail.
    await rename(selectedFile,heldFile);
    try {
      process.kill(committed.child.pid,'SIGKILL'); // Exact private fixture child verified above.
      const failedBy=performance.now()+12000;let failed;
      do {await delay(100);failed=await invoke('status');}
      while((!failed.error||failed.restarts<committed.restarts+2)&&performance.now()<failedBy);
      assert(failed.error,'the injected startup failure must be observed');
      assert.equal(JSON.parse(await readFile(configPath,'utf8')).current.sha256,alternateHash,
        'ordinary restart failure must not select a historical version');
      assert.equal(failed.candidateSha256,alternateHash);
    } finally {await rename(heldFile,selectedFile);}
    const healthyBy=performance.now()+25000;let healthy;
    do {await delay(100);healthy=await invoke('status');}while(!healthy.healthy&&performance.now()<healthyBy);
    assert.equal(healthy.healthy,true);assert.equal(healthy.candidateSha256,alternateHash);
    assert.equal(await readFile(marker,'utf8'),'x','restart recovery does not repeat durable effects');
    await invoke('rollback');assert.equal((await invoke('status')).candidateSha256,initialized.sha256);
  }
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
  // Model a stopped legacy installation while retaining its real durable job,
  // workspace and incidental files. Preparation must not touch any of them.
  await moveStoppedFixtureDirectory(path.join(root,'run/native'),path.join(fixture,'retained-old-native'));
  const stateDatabase=path.join(root,'run/state/metadata.sqlite3');
  const stateDigest=()=>readFile(stateDatabase).then(bytes=>createHash('sha256').update(bytes).digest('hex'));
  const beforePreparation=await stateDigest();
  const preparation=path.join(fixture,'prepared native manager');
  const prepared=await invoke('prepare','--output',preparation,'--binary',binary,'--allow-local-build',
    '--port',String(port),'--env-file',environment,'--companions',companionFile);
  assert.equal(prepared.prepared,true);assert.equal(prepared.initialized,false);
  assert.equal(prepared.activationRequired,true);
  await assert.rejects(access(path.join(root,'run/native')),{code:'ENOENT'});
  assert.equal(await stateDigest(),beforePreparation,'preparation leaves the existing database unchanged');
  assert.equal(await readFile(incidentalFile,'utf8'),incidentalContent);
  const preparedConfig=JSON.parse(await readFile(path.join(preparation,'run/native/config.json'),'utf8'));
  assert.equal(preparedConfig.root,initialized.root);
  assert.equal(preparedConfig.current.file,config.current.file,'prepared runtime binds the final path');
  await assert.rejects(runCheckedProcess(binary,['manage','start','--root',preparation],
    {env,cwd:fixture,timeoutMs:5000,label:'Prepared configuration cannot start from its staging directory'}),
    /NATIVE_MANAGEMENT_CONFIG_INVALID/);
  assert.equal(await stateDigest(),beforePreparation);
  await moveStoppedFixtureDirectory(path.join(preparation,'run/native'),path.join(root,'run/native'));
  const activated=await invoke('start');assert.equal(activated.healthy,true);
  assert.equal((await rpc('devbox_job_status',{job_id:job.id})).status,'succeeded');
  assert.equal(await readFile(marker,'utf8'),'x','prepared activation does not replay durable effects');
  assert.equal(await readFile(file,'utf8'),'native-core-effect');
  assert.equal(await readFile(incidentalFile,'utf8'),incidentalContent);
  await assert.rejects(invoke('prepare','--output',path.join(fixture,'second-preparation'),
    '--binary',binary,'--allow-local-build'),/NATIVE_EXISTING_MANAGEMENT/);
  await invoke('stop');
  const migratedStopBy=performance.now()+10000;
  while((await invoke('status')).running&&performance.now()<migratedStopBy)await delay(100);
  assert.equal((await invoke('status')).running,false);
  console.log(JSON.stringify({ok:true,platform:process.platform,startupMs,nodeOnRuntimePath:false,checks:[
    'empty-PATH native initialization/startup','unrelated download metadata survives startup and recovery',
    'live MCP file effect','duplicate suppression','restart/state preservation',
    'busy request and durable-job drain refusal','frontend crash recovery without replay','supervisor crash recovery',
    'independent owned companion lifecycle','service definition without activation','missing proof rejection',
    ...(process.platform==='linux'?['transient health recovery clears stale status without replacing the frontend']:[]),
    'failed candidate startup rollback','foreground and detached stop',
    ...(process.platform==='win32'||process.platform==='linux'?['committed selection survives ordinary restart failure without effect replay']:[]),
    'modified selected runtime rejected without changing restart intent',
    'preparation does not alter existing production paths or database',
    'staged manager cannot run before activation','prepared activation preserves durable effects without replay',
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
