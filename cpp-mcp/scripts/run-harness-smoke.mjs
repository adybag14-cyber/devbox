import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {mkdtemp,mkdir,readFile,writeFile,rm} from 'node:fs/promises';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {StreamableHTTPClientTransport} from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
import {stopStateFixture} from './state-fixture.mjs';
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const binary=process.env.DEVBOX_CPP_BINARY||path.join(repo,'.cpp-build/windows/cpp-mcp/Release/devbox-mcp.exe');
const probe=process.env.DEVBOX_ISOLATION_PROBE||path.join(path.dirname(binary),'devbox-isolation-probe'+(process.platform==='win32'?'.exe':''));
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const binaryHash=hash(await readFile(binary));
const root=await mkdtemp(path.join(os.tmpdir(),'devbox-run-sdk-'));
const workspace=path.join(root,'workspace');await mkdir(workspace);
const sentinel=path.join(root,'outside-sentinel');await writeFile(sentinel,'SYNTHETIC-PRIVATE-SENTINEL');
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const freePort=()=>new Promise((resolve,reject)=>{const s=net.createServer();s.once('error',reject);s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>resolve(p));});});
const port=await freePort();
let generations=0, sawToolOutput=false;
const model=http.createServer(async(req,res)=>{
  try{
    let body='';for await(const chunk of req){body+=chunk;assert(body.length<1024*1024);}
    assert.equal(req.url,'/v1/responses');assert.equal(req.method,'POST');
    const input=JSON.parse(body);assert.equal(input.model,'recorded-fixture');
    const outputs=input.input.filter(item=>item.type==='function_call_output');
    const output=outputs.length?[{type:'message',role:'assistant',content:[{type:'output_text',text:'The approved isolated worker completed.'}]}]:[
      {type:'function_call',id:'fc_fixture',call_id:'call_fixture',name:'program',arguments:JSON.stringify({requested_program:'isolated-native-probe',args:process.platform==='win32'?[sentinel,String(model.address().port)]:[sentinel]})}
    ];
    if(outputs.length){const receipt=JSON.parse(outputs.at(-1).output);assert.equal(receipt.result.exit_code,0);sawToolOutput=true;}
    generations++;
    res.writeHead(200,{'content-type':'application/json'});
    res.end(JSON.stringify({id:`resp_${generations}`,model:'recorded-fixture',status:'completed',output,usage:{input_tokens:20,output_tokens:10}}));
  }catch(error){res.writeHead(500,{'content-type':'application/json'});res.end(JSON.stringify({error:String(error)}));}
});
await new Promise(resolve=>model.listen(0,'127.0.0.1',resolve));
const env={...process.env,DEVBOX_PROJECT_ROOT:root,HOST:'127.0.0.1',PORT:String(port),MCP_AUTH_MODE:'none',PUBLIC_BASE_URL:'',
  DEVBOX_RUNTIME_MODE:'host',ENABLE_HOST_EXEC:'true',DEVBOX_AUTO_START:'false',HOST_WORKSPACE_PATH:workspace,
  DEVBOX_WORKSPACE_PATH:workspace,HOST_DEFAULT_WORKDIR:workspace,NODE_EXE:process.execPath,
  MCP_STATE_BACKEND:'sqlite',MCP_STATE_ROOT:path.join(root,'run','state'),MCP_JOBS_ROOT:path.join(root,'jobs'),
  MCP_EXEC_SLOT_ROOT:path.join(root,'slots'),MCP_PERFORMANCE_STATE_PATH:path.join(root,'run','performance.json'),DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE:'0'};
let child,exited,client,output='',runId,taskId,complete=false;
const tasks=process.env.DEVBOX_TEST_TASKS==='1';
let rpcId=0;
async function rpc(method,params={},optIn=true){
  const meta={'io.modelcontextprotocol/protocolVersion':'2026-07-28','io.modelcontextprotocol/clientCapabilities':optIn?{extensions:{'io.modelcontextprotocol/tasks':{}}}:{}};
  const headers={'content-type':'application/json',accept:'application/json','mcp-protocol-version':'2026-07-28','mcp-method':method};
  if(method==='tools/call')headers['mcp-name']=params.name;
  if(method==='resources/read')headers['mcp-name']=params.uri;
  const response=await fetch(`http://127.0.0.1:${port}/mcp`,{method:'POST',headers,body:JSON.stringify({jsonrpc:'2.0',id:++rpcId,method,params:{...params,_meta:meta}}),signal:AbortSignal.timeout(15000)});
  return response.json();
}
async function start(){
  child=spawn(binary,[],{cwd:repo,env,windowsHide:true,stdio:['ignore','pipe','pipe']});
  const owned=child;output='';for(const stream of [child.stdout,child.stderr])stream.on('data',v=>{output=(output+v).slice(-12000);});
  exited=new Promise((resolve,reject)=>{owned.once('exit',resolve);owned.once('error',reject);});
  const until=Date.now()+20000;
  for(;;){assert.equal(owned.exitCode,null,output);assert(Date.now()<until,output);
    try{const r=await fetch(`http://127.0.0.1:${port}`,{signal:AbortSignal.timeout(500)});if(r.ok){assert.equal((await r.json()).build.binarySha256,binaryHash);break;}}
    catch(error){if(error.code==='ERR_ASSERTION')throw error;}await delay(50);
  }
  client=new Client({name:'native-run-harness-fixture',version:'1'});
  await client.connect(new StreamableHTTPClientTransport(new URL(`http://127.0.0.1:${port}/mcp`)));
}
async function stopFrontend(){
  await client?.close().catch(()=>{});client=undefined;
  if(child&&child.exitCode===null&&child.signalCode===null){child.kill();await Promise.race([exited,delay(10000)]);}
  if(child)assert(child.exitCode!==null||child.signalCode!==null,'owned frontend stopped');
}
async function invoke(args){const reply=await client.callTool({name:'devbox_agent_run',arguments:args});assert.equal(reply.isError,false,JSON.stringify(reply));return reply.structuredContent.data;}
async function waitStatus(status){const until=Date.now()+20000;for(;;){const value=await invoke({action:'get',run_id:runId});if(value.status===status)return value;
  assert(!['failed','uncertain','cancelled'].includes(value.status),JSON.stringify(value));assert(Date.now()<until,JSON.stringify(value));await delay(50);}}
try{
  await start();
  const init=await client.callTool({name:'devbox_task_put',arguments:{task_id:'fixture_init',expected_revision:0,state:{fixture:true}}});assert.equal(init.isError,false);
  await writeFile(path.join(root,'run','state','providers.json'),JSON.stringify({profiles:[{id:'mock',base_url:`http://127.0.0.1:${model.address().port}/v1`,protocol:'responses',model:'recorded-fixture',local:true,tools:true,context_tokens:32768,output_tokens:4096,max_run_cost_micro_usd:0}]}),{mode:0o600});
  const providerList=await invoke({action:'providers'});assert.equal(providerList.providers[0].id,'mock');
  const create={action:'create',request_id:'sdk_run',provider_id:'mock',goal:'Run the operator-approved isolated native probe and report only its actual result.'};
  const created=await invoke(create);runId=created.run_id;
  if(tasks){
    const discover=await rpc('server/discover');assert(discover.result.capabilities.extensions['io.modelcontextprotocol/tasks']);
    const task=await rpc('tools/call',{name:'devbox_agent_run',arguments:create});assert.equal(task.result.resultType,'task',JSON.stringify(task));taskId=task.result.taskId;
    assert.equal((await rpc('tasks/get',{taskId},false)).error.code,-32021);
    assert.equal((await rpc('tasks/get',{taskId:'task-missing'})).error.code,-32602);
  }
  const pending=await waitStatus('awaiting_approval');assert.equal(generations,1);assert.equal(pending.tool_calls,0);
  const replayed=await invoke(create);assert.equal(replayed.run_id,runId,'create retry keeps identity');
  assert.equal(Object.hasOwn(replayed,'driver'),false,'approval-wait replay starts no driver');
  const paused=await invoke({action:'pause',run_id:runId});assert.equal(paused.status,'paused');
  assert.deepEqual(paused.pending_approval,pending.pending_approval,'pause retains the exact grant request');
  await stopFrontend();await start();
  assert.equal((await invoke({action:'get',run_id:runId})).status,'paused','pause survives frontend restart');
  const resumed=await invoke({action:'resume',run_id:runId});assert.equal(resumed.status,'awaiting_approval');
  assert.equal(Object.hasOwn(resumed,'driver'),false,'resuming an approval wait starts no driver');
  assert.equal(generations,1,'pause/resume does not spend another generation');
  const retained=await invoke({action:'get',run_id:runId});assert.equal(retained.status,'awaiting_approval');
  assert.deepEqual(retained.pending_approval,pending.pending_approval,'restart/resume preserves operation identity');
  if(tasks){
    const poll=await rpc('tasks/get',{taskId});assert.equal(poll.result.status,'input_required');assert(poll.result.inputRequests[retained.pending_approval.operation_id]);
    const ignored=await rpc('tasks/update',{taskId,inputResponses:{not_issued:{action:'accept',content:{grant_id:'fake'}}}});assert.equal(ignored.result.resultType,'complete');
  }
  const grant={principal:retained.principal_id,run:runId,operation:retained.pending_approval.operation_id,tool:'program',
    arguments:retained.pending_approval.arguments,workspace:retained.workspace,executable:probe,executable_sha256:hash(await readFile(probe)),egress_origins:[],expires_at_ms:Date.now()+60000};
  const grantFile=path.join(root,'operator-grant.json');await writeFile(grantFile,JSON.stringify(grant),{mode:0o600});
  const issued=await runCheckedProcess(binary,['--grant-issue',grantFile],{cwd:repo,env,timeoutMs:10000,label:'Issue exact fixture grant'});
  const grantId=JSON.parse(issued.stdout).grant_id;
  if(tasks){
    const update={taskId,inputResponses:{[retained.pending_approval.operation_id]:{action:'accept',content:{grant_id:grantId}}}};
    assert.equal((await rpc('tasks/update',update)).result.resultType,'complete');
    assert.equal((await rpc('tasks/update',update)).result.resultType,'complete','duplicate fulfilled input ignored');
  }else await invoke({action:'approve',run_id:runId,grant_id:grantId});
  const finished=await waitStatus('completed');
  assert.equal(finished.tool_calls,1);assert.equal(generations,2);assert(sawToolOutput);
  assert.equal(await readFile(path.join(finished.workspace,'result.txt'),'utf8'),'isolated-workspace-output');
  const events=await invoke({action:'events',run_id:runId});assert(events.events.some(e=>e.type==='operator_approved'));
  assert.equal((await invoke(create)).run_id,runId);assert.equal(generations,2);
  const payload=await invoke({action:'artifact',run_id:runId,sha256:finished.answer_artifact.sha256});assert(payload.untrusted_content);
  if(tasks){
    const final=await rpc('tasks/get',{taskId});assert.equal(final.result.status,'completed');assert.equal(final.result.result.structuredContent.data.run_id,runId);
    assert.equal((await rpc('tasks/cancel',{taskId})).result.resultType,'complete');
    assert.deepEqual((await rpc('tasks/get',{taskId})).result,final.result,'terminal task state is immutable');
    const listed=await rpc('resources/list');assert(listed.result.resources.some(r=>r.uri==='devbox://instructions/backend-v1'));
    const resource=await rpc('resources/read',{uri:`devbox://runs/${runId}/artifacts/${finished.answer_artifact.sha256}`});assert(JSON.parse(resource.result.contents[0].text).untrusted_content);
    const jobScript=path.join(workspace,'task-job.mjs');
    await writeFile(jobScript,"import{appendFileSync}from'node:fs';appendFileSync(process.argv[2],'x');\n");
    const job=await rpc('tools/call',{name:'devbox_job_submit',arguments:{task_id:'task_fixture',operation_id:'once',program:'node',args:[jobScript,path.join(workspace,'task-effect')]}});
    assert.equal(job.result.resultType,'task');
    const until=Date.now()+15000;let jobTask;
    do{jobTask=(await rpc('tasks/get',{taskId:job.result.taskId})).result;if(jobTask.status==='completed')break;assert(Date.now()<until,JSON.stringify(jobTask));await delay(100);}while(true);
    assert.equal(jobTask.result.isError,false,JSON.stringify(jobTask));
    assert.equal(await readFile(path.join(workspace,'task-effect'),'utf8'),'x');
  }
  complete=true;
  console.log(JSON.stringify({ok:true,binarySha256:binaryHash,runId,modelRequests:generations,isolatedEffects:1,frontendRestart:true,operatorGrant:true,completedReceiptReplay:true,paidRequests:0,tasksExtension:tasks},null,2));
}finally{
  if(client&&runId&&!complete){await invoke({action:'cancel',run_id:runId}).catch(()=>{});await delay(1500);}
  await stopFrontend();
  await stopStateFixture(binary,root);
  await new Promise(resolve=>model.close(resolve));
  if(complete)await rm(root,{recursive:true,force:true,maxRetries:20,retryDelay:200});
  else console.error(`Retained failed fixture: ${root}`);
}
