import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, readFile, writeFile, rm } from 'node:fs/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';

const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const root=await mkdtemp(path.join(os.tmpdir(),'devbox-agent-reliability-'));
const binary=process.env.DEVBOX_MCP_TEST_BINARY||path.join(repo,'rust-mcp/target/debug',process.platform==='win32'?'devbox-mcp.exe':'devbox-mcp');
const port=await new Promise((resolve,reject)=>{const s=net.createServer();s.once('error',reject);s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>resolve(p));});});
const url=new URL(`http://127.0.0.1:${port}/`);
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const sha=bytes=>createHash('sha256').update(bytes).digest('hex');
const b64=text=>Buffer.from(text).toString('base64');
const outcomes=[]; const ownedJobs=new Set();
let server,client,exited,logs='';
const env={...process.env,DEVBOX_PROJECT_ROOT:root,HOST:'127.0.0.1',PORT:String(port),MCP_AUTH_MODE:'none',PUBLIC_BASE_URL:'',DEVBOX_RUNTIME_MODE:'host',ENABLE_HOST_EXEC:'true',NODE_EXE:process.execPath,HOST_WORKSPACE_PATH:root,HOST_DEFAULT_WORKDIR:root,DEVBOX_WORKSPACE_PATH:root,MCP_JOBS_ROOT:path.join(root,'jobs'),MCP_EXEC_SLOT_ROOT:path.join(root,'slots'),MCP_JOB_MAX_ACTIVE_RUNNERS:'2',MCP_JOB_MAX_RUNNERS_PER_TASK:'1',MCP_EXEC_HEAVY_CAPACITY:'5',MCP_JOB_LOG_MAX_BYTES:'4096',MCP_JOB_LOG_ROTATIONS:'2'};
async function start(){
  server=spawn(binary,[],{cwd:repo,env,stdio:['ignore','pipe','pipe'],windowsHide:true});
  exited=new Promise((resolve,reject)=>{server.once('error',reject);server.once('exit',resolve);});
  for(const stream of [server.stdout,server.stderr]){stream.setEncoding('utf8');stream.on('data',chunk=>{logs=(logs+chunk).slice(-32000);});}
  const deadline=Date.now()+20000;let ready=false;
  while(Date.now()<deadline){if(server.exitCode!==null||server.signalCode!==null)throw new Error(`Server exited: ${logs}`);try{const r=await fetch(new URL('readyz',url),{signal:AbortSignal.timeout(500)});if(r.ok){ready=true;break;}}catch{}await sleep(100);}
  assert(ready,`server readiness: ${logs}`);
  client=new Client({name:'native-agent-reliability',version:'2'});
  await client.connect(new StreamableHTTPClientTransport(url));
}
async function stop({abrupt=false}={}){
  await client?.close().catch(()=>{});client=null;
  if(server&&server.exitCode===null&&server.signalCode===null){
    server.kill(abrupt?'SIGKILL':'SIGTERM');
    await Promise.race([exited,sleep(abrupt?3000:12000)]);
    if(server.exitCode===null&&server.signalCode===null){
      console.error(`Force-cleaning owned test server PID ${server.pid} after its shutdown deadline`);
      server.kill('SIGKILL');await Promise.race([exited,sleep(3000)]);
    }
    assert(server.exitCode!==null||server.signalCode!==null,'owned test server stopped');
  }
}
async function call(name,args={},failure=false){const r=await client.callTool({name,arguments:args});if(failure){assert.equal(r.isError,true,`${name} unexpectedly succeeded`);return r;}assert.equal(r.isError??false,false,`${name}: ${JSON.stringify(r.structuredContent)}`);assert.equal(r.structuredContent?.ok,true);return r.structuredContent.data;}
async function check(name,body){const start=Date.now();await body();outcomes.push({name,ok:true,durationMs:Date.now()-start});}
async function done(id){let value;const deadline=Date.now()+15000;do{value=await call('devbox_job_status',{job_id:id,wait_seconds:2,terminal_only:true});if(['succeeded','failed','cancelled','timed_out','interrupted'].includes(value.status))return value;}while(Date.now()<deadline);throw new Error(`Job did not reach terminal state: ${JSON.stringify(value)}`);}
async function running(id){const deadline=Date.now()+10000;while(Date.now()<deadline){const r=await call('devbox_job_status',{job_id:id});if(r.status==='running'&&r.childPid)return r;await sleep(100);}throw new Error('job did not start');}
let duplicated,duplicateRequest;
try{
  await start();
  await check('45 discoverable tools, capability hash and heavy capacity five',async()=>{
    const tools=(await client.listTools()).tools;assert.equal(tools.length,45);
    const caps=await call('devbox_capabilities');assert.equal(caps.contract_version,2);assert.equal(caps.limits.heavy_capacity,5);assert.equal(caps.limits.active_runners,2);assert.match(caps.schema_sha256,/^[a-f0-9]{64}$/);
    assert.deepEqual([...caps.tools].sort(),tools.map(t=>t.name).sort());
    const schema=await call('devbox_capabilities',{tool_name:'devbox_job_submit'});assert.match(JSON.stringify(schema.inputSchema),/io-heavy/);
  });
  await check('atomic overwrite, conflict, append and replay',async()=>{
    const file=path.join(root,'checkpoint.bin');
    const first=await call('devbox_write_file_atomic',{path:file,content_base64:b64('first'),expected_file_sha256:'missing'});
    assert.equal(first.current.sha256,sha('first'));
    assert.equal((await call('devbox_write_file_atomic',{path:file,content_base64:b64('first'),expected_file_sha256:'missing'})).replayed,true);
    await call('devbox_write_file_atomic',{path:file,content_base64:b64('wrong'),expected_file_sha256:'missing'},true);
    const append={path:file,content_base64:b64(' + second'),expected_file_sha256:sha('first'),append:true,expected_offset_bytes:5};
    await call('devbox_write_file_atomic',append);assert.equal((await call('devbox_write_file_atomic',append)).replayed,true);
    assert.equal(await readFile(file,'utf8'),'first + second');
    assert.equal((await call('devbox_file_state',{path:file})).sha256,sha('first + second'));
  });
  await check('concurrent duplicate submission executes once and survives retention',async()=>{
    const counter=path.join(root,'counter');
    duplicateRequest={task_id:'once',operation_id:'build',program:'node',args:['-e',"const fs=require('fs');const p=process.argv[1];fs.writeFileSync(p,String(Number(fs.existsSync(p)?fs.readFileSync(p,'utf8'):0)+1));console.log('x'.repeat(16000));console.log('DONE');",counter],working_dir:root};
    const [a,b]=await Promise.all([call('devbox_job_submit',duplicateRequest),call('devbox_job_submit',duplicateRequest)]);
    assert.equal(a.id,b.id);assert.notEqual(a.replayed,b.replayed);duplicated=a.id;ownedJobs.add(a.id);
    assert.equal((await done(a.id)).status,'succeeded');assert.equal(await readFile(counter,'utf8'),'1');
    await call('devbox_job_submit',{...duplicateRequest,args:['--version']},true);
    const log=await call('devbox_job_logs',{job_id:a.id,max_chars:2000});assert.match(log.stdout,/DONE/);assert.equal(log.logs.truncated,true);
    await rm(path.join(root,'jobs',a.id),{recursive:true,force:true});
    const expired=await call('devbox_job_submit',duplicateRequest);assert.equal(expired.replayed,true);assert.equal(expired.job.status,'result_expired');assert.equal(await readFile(counter,'utf8'),'1');ownedJobs.delete(a.id);
  });
  const sleeper=(task,operation)=>({task_id:task,operation_id:operation,program:'node',args:['-e','setInterval(()=>{},1000)'],working_dir:root,timeout_seconds:120});
  let a,b;
  await check('global and per-task runner admission plus pagination',async()=>{
    a=await call('devbox_job_submit',sleeper('task-a','work'));ownedJobs.add(a.id);
    await call('devbox_job_submit',sleeper('task-a','other'),true);
    b=await call('devbox_job_submit',sleeper('task-b','work'));ownedJobs.add(b.id);
    await call('devbox_job_submit',sleeper('task-c','work'),true);
    const replay=await call('devbox_job_submit',sleeper('task-a','work'));assert.equal(replay.id,a.id);assert.equal(replay.replayed,true);
    const page=await call('devbox_job_list',{limit:1});assert.equal(page.jobs.length,1);assert(page.next_cursor);
    const second=await call('devbox_job_list',{limit:1,cursor:page.next_cursor});assert.equal(second.jobs.length,1);assert.notEqual(page.jobs[0].id,second.jobs[0].id);
    const filtered=await call('devbox_job_list',{task_id:'task-a'});assert.equal(filtered.jobs.length,1);assert.equal(filtered.jobs[0].id,a.id);
    assert(filtered.jobs.every(job=>Object.keys(job).every(key=>['id','status','createdAtUtc','startedAtUtc','completedAtUtc','exitCode','runnerAlive','agent'].includes(key))),'discovery omits detailed logs and process diagnostics');
  });
  await check('task CAS and reconnect recover work without resubmission',async()=>{
    await running(a.id);await running(b.id);
    const state={phase:'running',job_ids:[a.id,b.id],next_action:'poll existing jobs'};
    const request={task_id:'workflow',expected_revision:0,state};
    assert.equal((await call('devbox_task_put',request)).record.revision,1);
    assert.equal((await call('devbox_task_put',request)).replayed,true);
    await call('devbox_task_put',{...request,state:{phase:'stale'}},true);
    await stop({abrupt:true});await start();
    assert.deepEqual((await call('devbox_task_get',{task_id:'workflow'})).record.state,state);
    assert.equal((await call('devbox_job_submit',sleeper('task-a','work'))).replayed,true);
    assert.equal((await call('devbox_task_list')).tasks[0].task_id,'workflow');
  });
  await check('cancellation confirms both runner and child stopped',async()=>{
    for(const job of [a,b]){const before=await running(job.id);const acknowledgement=await call('devbox_job_cancel',{job_id:job.id});assert(['cancel_requested','cancelled'].includes(acknowledgement.status));const result=await done(job.id);assert.equal(result.status,'cancelled');assert.equal(result.runnerAlive,false);try{process.kill(before.childPid,0);assert.fail('child survived cancellation');}catch(error){if(error.code!=='ESRCH')throw error;}ownedJobs.delete(job.id);}
  });
  await check('nested task checkpoints retain bounded compact output',async()=>{
    let state=Array(500).fill('checkpoint');for(let i=0;i<60;i++)state={next:state};
    const r=await client.callTool({name:'devbox_task_put',arguments:{task_id:'nested',expected_revision:0,state}});
    assert.equal(r.isError??false,false);assert.deepEqual(r.structuredContent.data.record.state,state);
    const text=r.content.filter(c=>c.type==='text').map(c=>c.text).join('');
    assert(text.length<JSON.stringify(r.structuredContent.data).length+300,'text rendering must not expand nested state through indentation');
  });
  if(process.platform==='win32')await check('native cold capture works through successive Rust server starts',async()=>{
    for(let attempt=0;attempt<3;attempt++){if(attempt){await stop();await start();}const r=await client.callTool({name:'host_capture_display',arguments:{quality:50,timeout_seconds:15}});assert.equal(r.isError??false,false,JSON.stringify(r.structuredContent));assert(r.content.some(c=>c.type==='image'&&c.data.length>200));}
  });
  console.log(JSON.stringify({ok:true,checks:outcomes,root},null,2));
}catch(error){console.error(JSON.stringify({ok:false,error:error.stack,checks:outcomes,serverLogs:logs,root},null,2));process.exitCode=1;}
finally{
  if(client)for(const id of ownedJobs){await call('devbox_job_cancel',{job_id:id}).catch(()=>{});}
  await stop();
  if(process.env.DEVBOX_AGENT_EVIDENCE_DIR){await writeFile(path.join(process.env.DEVBOX_AGENT_EVIDENCE_DIR,'agent-smoke.json'),JSON.stringify({ok:!process.exitCode,checks:outcomes,root},null,2));}
  else await rm(root,{recursive:true,force:true,maxRetries:5,retryDelay:100});
}
