// Opt-in live network validation. Fixtures and ordinary CI remain deterministic.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,mkdir,readFile,writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import {fileURLToPath} from 'node:url';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {StreamableHTTPClientTransport} from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import {assertNativeContract} from './native-contract.mjs';

assert.equal(process.env.DEVBOX_RESEARCH_LIVE,'1','Set DEVBOX_RESEARCH_LIVE=1 for authorized live source validation');
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const binary=process.env.DEVBOX_CPP_BINARY||path.join(repo,'.cpp-build/windows/cpp-mcp/Release/devbox-mcp.exe');
const hash=createHash('sha256').update(await readFile(binary)).digest('hex');
const root=process.env.DEVBOX_RESEARCH_EVIDENCE_ROOT||await mkdtemp(path.join(os.tmpdir(),'devbox-web-live-'));
await mkdir(root,{recursive:true}); await mkdir(path.join(root,'workspace'),{recursive:true});
const port=await new Promise((resolve,reject)=>{const server=net.createServer();server.once('error',reject);server.listen(0,'127.0.0.1',()=>{const p=server.address().port;server.close(()=>resolve(p));});});
const child=spawn(binary,[],{cwd:repo,windowsHide:true,stdio:['ignore','pipe','pipe'],env:{...process.env,
  DEVBOX_PROJECT_ROOT:root,HOST:'127.0.0.1',PORT:String(port),PUBLIC_BASE_URL:'',MCP_AUTH_MODE:'none',
  DEVBOX_RUNTIME_MODE:'host',ENABLE_HOST_EXEC:'true',DEVBOX_AUTO_START:'false',DEVBOX_COMPUTER_USE_PIPE:'',
  DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE:'0',HOST_DEFAULT_WORKDIR:path.join(root,'workspace'),
  HOST_WORKSPACE_PATH:path.join(root,'workspace'),DEVBOX_WORKSPACE_PATH:path.join(root,'workspace'),
  MCP_JOBS_ROOT:path.join(root,'jobs'),MCP_EXEC_SLOT_ROOT:path.join(root,'slots'),
  MCP_PERFORMANCE_STATE_PATH:path.join(root,'run','mcp-performance.json'),
  MCP_JOB_HEARTBEAT_MS:'100',MAX_TEXT_OUTPUT_CHARS:'4000000',MAX_MCP_TRANSFER_CHARS:'4000000'}});
let output='';for(const stream of [child.stdout,child.stderr])stream.on('data',bytes=>{output=(output+bytes.toString()).slice(-12000);});
const exited=new Promise((resolve,reject)=>{child.once('exit',resolve);child.once('error',reject);});
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const report={startedAt:new Date().toISOString(),binarySha256:hash,pid:child.pid,root,case:process.env.DEVBOX_RESEARCH_CASE||'catalogue',results:{}};
await writeFile(path.join(root,'owner.json'),JSON.stringify({pid:child.pid,executable:binary,binarySha256:hash,port,root,startedAt:report.startedAt},null,2));
let client;
const jobs=[];
try {
  const base=`http://127.0.0.1:${port}`;
  const startup=Date.now()+20000;
  for(;;){assert(Date.now()<startup,output);assert.equal(child.exitCode,null,output);try{const res=await fetch(base,{signal:AbortSignal.timeout(500)});if(res.ok){assert.equal((await res.json()).build.binarySha256,hash);break;}}catch(error){if(error.code==='ERR_ASSERTION')throw error;}await delay(100);}
  client=new Client({name:'native-web-research-live-validation',version:'1'},{capabilities:{}});
  await client.connect(new StreamableHTTPClientTransport(new URL(base+'/mcp')));
  const invoke=async(name,args)=>{const result=await client.callTool({name,arguments:args},undefined,{timeout:45000});assert.equal(result.isError,false,JSON.stringify(result));return result.structuredContent.data;};
  const capabilities=await invoke('devbox_capabilities',{});
  assertNativeContract((await client.listTools()).tools,capabilities);
  const denied=await client.callTool({name:'devbox_web_fetch',arguments:{urls:['http://169.254.169.254/latest/meta-data/']}});
  assert.equal(denied.isError,true,'private endpoint cannot be fetched');
  async function run(label,args){
    const input={task_id:'live_native_research',operation_id:label,...args};
    const submitted=await invoke('devbox_web_research',input);jobs.push(submitted.job_id);
    const replay=await invoke('devbox_web_research',input);assert.equal(replay.job_id,submitted.job_id);assert.equal(replay.replayed,true);
    const conflict=await client.callTool({name:'devbox_web_research',arguments:{...input,topic:input.topic+' changed'}});assert.equal(conflict.isError,true);
    const deadline=Date.now()+420000;
    for(;;){assert(Date.now()<deadline,'live research hard deadline');const state=await invoke('devbox_job_status',{job_id:submitted.job_id,wait_seconds:20});if(['succeeded','failed','cancelled','timed_out','interrupted'].includes(state.status)){assert.equal(state.status,'succeeded',JSON.stringify(state));break;}}
    let evidence=await invoke('devbox_web_evidence',{job_id:submitted.job_id,max_chars:128000});
    const sources=[...evidence.sources];
    while(evidence.next_offset!==null){const offset=evidence.next_offset;assert(!evidence.minimum_required_chars,'evidence budget must make progress');evidence=await invoke('devbox_web_evidence',{job_id:submitted.job_id,offset,max_chars:128000});assert(evidence.next_offset===null||evidence.next_offset>offset,'evidence cursor advances');sources.push(...evidence.sources);}
    assert.equal(sources.length,evidence.usable_sources,'all source briefs read, no hidden count inflation');
    assert.equal(new Set(sources.map(source=>source.content_sha256)).size,sources.length,'unique content hashes');
    report.results[label]={...evidence,sources};
    await writeFile(path.join(root,label+'.json'),JSON.stringify(report.results[label],null,2));
    console.log(JSON.stringify({case:label,job_id:submitted.job_id,target:evidence.target_sources,usable:evidence.usable_sources,domains:evidence.distinct_domains,elapsed_ms:evidence.elapsed_ms,decoded_bytes:evidence.decoded_bytes,target_met:evidence.target_met,stop_reason:evidence.stop_reason}));
  }
  if(report.case==='catalogue'){
    const urls=['https://e-catalog.co.uk/list/122/pr-56675/','https://curl.se/libcurl/c/libcurl-multi.html'];
    report.results.fetch=await invoke('devbox_web_fetch',{urls,query:'Snapdragon price libcurl',max_chars:64000});
    assert(report.results.fetch.documents.some(doc=>doc.status==='ok'),'at least one live document readable');
    const catalogue=report.results.fetch.documents.find(doc=>doc.url.includes('e-catalog.co.uk'));
    assert.equal(catalogue.status,'ok','target niche catalogue readable');
    assert(catalogue.structured_metadata.some(item=>String(item['@type']).includes('Offer')&&item.context_name&&item.priceCurrency==='GBP'),'live offer remains associated with its product');
    report.results.cached=await invoke('devbox_web_fetch',{urls,query:'Snapdragon price libcurl',max_age_seconds:300,max_chars:64000});
    await run('catalogue_fast',{topic:'Snapdragon 8 Elite Gen 5 smartphone prices UK GBP',mode:'fast',queries:['Snapdragon 8 Elite Gen 5 phone prices'],exact_terms:['Snapdragon 8 Elite Gen 5'],urls:[urls[0]],domains:['e-catalog.co.uk'],discovery:'web'});
    assert(report.results.catalogue_fast.usable_sources>0,'niche research yields actual evidence');
    assert(report.results.catalogue_fast.sources.every(source=>source.matched_exact_terms.includes('Snapdragon 8 Elite Gen 5')),'every counted source meets the entity filter');
  }else if(report.case==='rfc'){
    const urls=Array.from({length:151},(_,i)=>`https://www.rfc-editor.org/rfc/rfc${9000+i}.html`);
    const common={topic:'Internet protocol transport security specification',urls,discovery:'none',domains:['rfc-editor.org'],max_age_seconds:3600};
    await run('rfc_fast',{...common,mode:'fast'});
    await run('rfc_standard',{...common,mode:'standard'});
    assert.equal(report.results.rfc_fast.usable_sources,50,'50 actual live documents');
    assert.equal(report.results.rfc_standard.usable_sources,100,'100 actual live documents');
    report.coverageCaveat='These are real primary technical documents from one publisher, not 100 independent publishers.';
  }else throw new Error('Unknown live case');
  report.ok=true;report.completedAt=new Date().toISOString();
}catch(error){report.ok=false;report.error=String(error.stack||error);throw error;}
finally{
  if(client){for(const job_id of jobs){try{const status=(await client.callTool({name:'devbox_job_status',arguments:{job_id,wait_seconds:0}})).structuredContent.data;if(!['succeeded','failed','cancelled','timed_out','interrupted'].includes(status.status)){await client.callTool({name:'devbox_job_cancel',arguments:{job_id}});await client.callTool({name:'devbox_job_status',arguments:{job_id,wait_seconds:10}});}}catch{}}
    await client.close().catch(()=>{});}
  if(child.exitCode===null&&child.signalCode===null){child.kill();await Promise.race([exited,delay(10000)]);}
  report.serverStopped=child.exitCode!==null||child.signalCode!==null;
  await writeFile(path.join(root,'result.json'),JSON.stringify(report,null,2));
  assert(report.serverStopped,'owned validation server stopped');
}
