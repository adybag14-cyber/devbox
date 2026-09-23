// Run only against a reviewed/verified binary. This creates a new synthetic
// project and never reads production runtime settings or attaches to a service.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {mkdir,writeFile,readFile,access} from 'node:fs/promises';
import path from 'node:path';
import net from 'node:net';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {StreamableHTTPClientTransport} from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
import {stopStateFixture} from './state-fixture.mjs';

const [binaryArgument,outputArgument,baselineArgument]=process.argv.slice(2);
assert(binaryArgument&&outputArgument,'Pass absolute verified binary and NEW canary output directory');
const binary=path.resolve(binaryArgument),root=path.resolve(outputArgument);
await mkdir(root);await mkdir(path.join(root,'workspace'));
const workspace=path.join(root,'workspace');
const digest=createHash('sha256').update(await readFile(binary)).digest('hex');
const baseline=baselineArgument?path.resolve(baselineArgument):null;
const baselineDigest=baseline?createHash('sha256').update(await readFile(baseline)).digest('hex'):null;
const clean={};for(const [key,value]of Object.entries(process.env))if(['PATH','SYSTEMROOT','WINDIR','SYSTEMDRIVE','TEMP','TMP','TMPDIR','HOME','USERPROFILE','LANG','LOCALAPPDATA','APPDATA','PATHEXT'].includes(key.toUpperCase()))clean[key]=value;
const port=await new Promise((resolve,reject)=>{const s=net.createServer();s.once('error',reject);s.listen(0,'127.0.0.1',()=>{const value=s.address().port;s.close(()=>resolve(value));});});
const base=`http://127.0.0.1:${port}`;
const env={...clean,DEVBOX_PROJECT_ROOT:root,HOST:'127.0.0.1',PORT:String(port),MCP_AUTH_MODE:'none',PUBLIC_BASE_URL:'',
  DEVBOX_RUNTIME_MODE:'host',ENABLE_HOST_EXEC:'true',DEVBOX_AUTO_START:'false',ENABLE_GATEWAY_BRIDGE:'false',
  HOST_WORKSPACE_PATH:workspace,HOST_DEFAULT_WORKDIR:workspace,DEVBOX_WORKSPACE_PATH:workspace,NODE_EXE:process.execPath,
  MCP_JOBS_ROOT:path.join(root,'jobs'),MCP_EXEC_SLOT_ROOT:path.join(root,'slots'),MCP_STATE_ROOT:path.join(root,'run/state'),
  MCP_PERFORMANCE_STATE_PATH:path.join(root,'run/performance.json'),MCP_STATE_BACKEND:'legacy',DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE:'1'};
const run=(args,overrides={})=>runCheckedProcess(binary,args,{env:{...env,...overrides},cwd:root,timeoutMs:20000,label:'Owned deployment canary operator command'});
const identity=JSON.parse((await run(['--build-info'])).stdout);
assert.equal(identity.binarySha256,digest);assert(identity.stateSchemaVersion>=2&&identity.contractVersion>=9);
let child,exited,client,job;let output='';const owners=[];let complete=false;
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function stop(){await client?.close().catch(()=>{});client=undefined;
  if(child&&child.exitCode===null&&child.signalCode===null){child.kill();await Promise.race([exited,delay(10000)]);}
  if(child)assert(child.exitCode!==null||child.signalCode!==null,'Exact owned frontend stopped');child=undefined;}
async function start(selected=binary){
  const expected=selected===baseline?baselineDigest:digest;
  output='';child=spawn(selected,[],{env,cwd:root,windowsHide:true,stdio:['ignore','pipe','pipe']});
  const owner={pid:child.pid,executable:selected,binarySha256:expected,startedAt:new Date().toISOString(),backend:env.MCP_STATE_BACKEND};owners.push(owner);
  exited=new Promise((resolve,reject)=>{child.once('exit',(code,signal)=>{owner.terminal={code,signal};resolve();});child.once('error',reject);});
  for(const stream of[child.stdout,child.stderr])stream.on('data',chunk=>{output=(output+chunk.toString()).slice(-16000);});
  const deadline=Date.now()+20000;for(;;){assert(Date.now()<deadline,output);assert.equal(child.exitCode,null,output);
    try{const r=await fetch(base,{signal:AbortSignal.timeout(500)});if(r.ok){assert.equal((await r.json()).build.binarySha256,expected);break;}}catch(e){if(e.code==='ERR_ASSERTION')throw e;}await delay(50);}
  client=new Client({name:'owned-deployment-canary',version:'1'},{capabilities:{}});await client.connect(new StreamableHTTPClientTransport(new URL(`${base}/mcp`)));
}
async function call(name,arguments_){const result=await client.callTool({name,arguments:arguments_});assert.equal(result.isError,false,JSON.stringify(result));return result.structuredContent.data;}
async function admission(action){const value=JSON.parse((await run(['--admission',action])).stdout);const deadline=Date.now()+5000;
  for(;;){const observed=(await(await fetch(base)).json()).admission;if(observed.generation===value.requested.generation)return observed;
    assert(Date.now()<deadline,'Admission generation not acknowledged');await delay(50);}}
async function absent(file){await assert.rejects(access(file),{code:'ENOENT'});}
try {
  let baselineSubmission,baselineJob;
  const baselineEffect=path.join(workspace,'baseline-effect');
  if(baseline) {
    await start(baseline);
    const script=path.join(workspace,'baseline-once.mjs');
    await writeFile(script,"import{appendFileSync}from'node:fs';appendFileSync(process.argv[2],'x');console.log('baseline effect');\n");
    baselineSubmission={task_id:'baseline_task',operation_id:'preserve_across_upgrade',program:'node',args:[script,baselineEffect],timeout_seconds:30};
    baselineJob=await call('devbox_job_submit',baselineSubmission);
    assert.equal((await call('devbox_job_status',{job_id:baselineJob.id,wait_seconds:10})).status,'succeeded');
    assert.equal(await readFile(baselineEffect,'utf8'),'x');
    await stop();
  }
  await start();
  if(baseline){const replay=await call('devbox_job_submit',baselineSubmission);assert.equal(replay.id,baselineJob.id);assert.equal(replay.replayed,true);assert.equal(await readFile(baselineEffect,'utf8'),'x');}
  const canary='SYNTHETIC-CREDENTIAL-REDACT-FROM-DEFAULT-EXPORT';
  const checkpoint=await call('devbox_task_put',{task_id:'migration_checkpoint',expected_revision:0,state:{phase:'before migration',canary}});
  assert.equal(checkpoint.record.revision,1);
  const script=path.join(workspace,'once.mjs'),gate=path.join(workspace,'release'),effect=path.join(workspace,'effect'),ready=path.join(workspace,'ready');
  await writeFile(script,"import{writeFileSync,existsSync,appendFileSync}from'node:fs';writeFileSync(process.argv[4],'ready');const end=Date.now()+90000;while(!existsSync(process.argv[2])){if(Date.now()>end)throw Error('fixture gate deadline');await new Promise(r=>setTimeout(r,25));}appendFileSync(process.argv[3],'x');console.log('one effect');\n");
  const submission={task_id:'canary_task',operation_id:'exactly_once',program:'node',args:[script,gate,effect,ready],timeout_seconds:120};
  job=await call('devbox_job_submit',submission);
  const readyDeadline=Date.now()+15000;for(;;){try{await access(ready);break;}catch{}assert(Date.now()<readyDeadline,'Durable fixture runner did not start');await delay(25);}
  assert.equal((await admission('drain')).mode,'draining');
  const blocked=await client.callTool({name:'devbox_write_file_atomic',arguments:{path:path.join(workspace,'blocked'),content_base64:'eA==',expected_file_sha256:'missing'}});
  assert.equal(blocked.isError,true);assert(JSON.stringify(blocked).includes('SERVER_DRAINING'));await absent(path.join(workspace,'blocked'));
  await assert.rejects(run(['--migrate-state']),/lock|FRONTEND/iu);
  await stop();
  await assert.rejects(run(['--migrate-state']),/STATE_MIGRATION_ACTIVE_JOB/u);
  await start();assert.equal((await(await fetch(base)).json()).admission.mode,'draining','Drain survives frontend replacement');
  await writeFile(gate,'release');
  assert.equal((await call('devbox_job_status',{job_id:job.id,wait_seconds:10})).status,'succeeded');
  assert.equal(await readFile(effect,'utf8'),'x','Acknowledged runner survived frontend replacement exactly once');
  await stop();
  const migration=JSON.parse((await run(['--migrate-state'])).stdout);assert.equal(migration.migrated,true);assert.equal(migration.jobs,baseline?2:1);assert.equal(migration.operations,baseline?2:1);assert.equal(migration.tasks,1);
  assert.equal(JSON.parse((await run(['--migrate-state'])).stdout).replayed,true);
  env.MCP_STATE_BACKEND='sqlite';await start();await admission('resume');
  const replay=await call('devbox_job_submit',submission);assert.equal(replay.id,job.id);assert.equal(replay.replayed,true);assert.equal(await readFile(effect,'utf8'),'x');
  if(baseline){const old=await call('devbox_job_submit',baselineSubmission);assert.equal(old.id,baselineJob.id);assert.equal(old.replayed,true);assert.equal(await readFile(baselineEffect,'utf8'),'x');}
  assert.equal((await call('devbox_task_get',{task_id:'migration_checkpoint'})).record.state.canary,canary);
  const publicReport=path.join(root,'safe-export.json');await run(['--state-export',publicReport]);assert(!(await readFile(publicReport,'utf8')).includes(canary));
  const snapshot=path.join(root,'private-snapshot');await run(['--private-state-snapshot',snapshot,'--include-private-state']);
  assert((await readFile(path.join(snapshot,'metadata.sqlite3'))).includes(Buffer.from(canary)));
  const restored=path.join(root,'restored-inspection');await run(['--restore-state-snapshot',snapshot,restored]);
  assert.equal(JSON.parse(await readFile(path.join(restored,'recovery-fenced.json'),'utf8')).recovery_fenced,true);
  await admission('drain');await stop();
  // An unsafe legacy backend cannot be used as a rollback after authority cutover.
  await assert.rejects(run(['--parity-report'],{MCP_STATE_BACKEND:'legacy'}),/STATE_LEGACY_BACKEND_FENCED/u);
  // A startup failure must leave the acknowledged state intact. Restore the
  // same known-good exact artifact, preserving SQLite authority and receipts.
  await assert.rejects(run([],{HOST:'invalid address'}));
  await start();await admission('resume');
  const restoredJob=await call('devbox_job_submit',submission);assert.equal(restoredJob.id,job.id);assert.equal(restoredJob.replayed,true);assert.equal(await readFile(effect,'utf8'),'x');
  const result={schema:1,ok:true,build:identity,binarySha256:digest,baselineSha256:baselineDigest,
    receiptReplayAcrossBinaryUpgrade:!!baseline,admissionDrain:true,frontendRestart:true,durableEffects:baseline?2:1,
    migration,receiptReplayAfterMigration:true,unsafeLegacyRollbackBlocked:true,privateSnapshotFenced:true,
    knownGoodArtifactRestoredAfterStartupFailure:true,rollbackScope:'same compatible exact artifact; no state downgrade',productionRequests:0};
  await writeFile(path.join(root,'canary-result.json'),JSON.stringify(result,null,2)+'\n');complete=true;console.log(JSON.stringify(result));
}finally{
  if(!complete&&client&&job){await client.callTool({name:'devbox_job_cancel',arguments:{job_id:job.id}}).catch(()=>{});await delay(500);}
  await stop();await stopStateFixture(binary,root);
  await writeFile(path.join(root,'owned-frontends.json'),JSON.stringify(owners,null,2)+'\n');
  if(!complete)console.error(`Retained incomplete owned canary: ${root}\n${output}`);
}
