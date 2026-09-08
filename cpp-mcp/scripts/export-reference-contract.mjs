// Run only against an isolated copy of the immutable baseline executable.
// Regenerates wire fixtures; the C++ runtime never starts the Rust executable.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,mkdir,writeFile,readFile,rm} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {StreamableHTTPClientTransport} from '@modelcontextprotocol/sdk/client/streamableHttp.js';
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const binary=process.env.DEVBOX_RUST_REFERENCE_BINARY||path.join(repo,'.cpp-build/reference/devbox-rust-reference.exe');
const expectedSha='6b33147a1032368a293557705811047cd4be01b080000de0de7fc2f4dcad0114';
assert.equal(createHash('sha256').update(await readFile(binary)).digest('hex'),expectedSha,'immutable baseline binary');
const root=await mkdtemp(path.join(os.tmpdir(),'devbox-cpp-contract-'));
const output=path.join(repo,'cpp-mcp/contract');await mkdir(output,{recursive:true});
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const cases={};const wire=[];
for(const mode of ['host','docker']){
 const state=path.join(root,mode);await mkdir(state,{recursive:true});
 const workspace=path.join(state,'workspace'),hostWorkdir=path.join(state,'host-workdir');await mkdir(workspace);await mkdir(hostWorkdir);
 const port=await new Promise((resolve,reject)=>{const s=net.createServer();s.once('error',reject);s.listen(0,'127.0.0.1',()=>{const port=s.address().port;s.close(()=>resolve(port));});});
 const env={...process.env,DEVBOX_PROJECT_ROOT:state,HOST:'127.0.0.1',PORT:String(port),MCP_AUTH_MODE:'none',PUBLIC_BASE_URL:'',DEVBOX_RUNTIME_MODE:mode,ENABLE_HOST_EXEC:'true',DEVBOX_AUTO_START:'false',DEVBOX_CONTAINER_NAME:`devbox-cpp-reference-${process.pid}`,HOST_WORKSPACE_PATH:workspace,DEVBOX_WORKSPACE_PATH:workspace,HOST_DEFAULT_WORKDIR:hostWorkdir,DEVBOX_DEFAULT_USER:'__DEVBOX_USER__',NODE_EXE:process.execPath,MCP_JOBS_ROOT:path.join(state,'jobs'),MCP_EXEC_SLOT_ROOT:path.join(state,'slots'),MAX_COMMAND_OUTPUT_CHARS:'65536',MAX_TEXT_OUTPUT_CHARS:'4000000',MAX_MCP_TRANSFER_CHARS:'4000000',MCP_WAIT_MAX_SECONDS:'300',DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE:'0'};
 const child=spawn(binary,[],{env,cwd:repo,windowsHide:true,stdio:['ignore','ignore','pipe']});let stderr='';child.stderr.setEncoding('utf8');child.stderr.on('data',s=>stderr=(stderr+s).slice(-16000));const exited=new Promise((resolve,reject)=>{child.once('exit',resolve);child.once('error',reject);});let client;
 try{
  const url=new URL(`http://127.0.0.1:${port}/`);const deadline=Date.now()+20000;
  for(;;){assert(Date.now()<deadline,stderr);assert.equal(child.exitCode,null,stderr);try{const r=await fetch(new URL('healthz',url),{signal:AbortSignal.timeout(500)});if(r.ok)break;}catch{}await sleep(100);}
  const recorder=async(input,init)=>{if(init?.body)wire.push({mode,headers:Object.fromEntries(new Headers(init.headers)),request:JSON.parse(init.body)});const r=await fetch(input,init);return r;};
  client=new Client({name:'cpp-contract-reference',version:'1'});await client.connect(new StreamableHTTPClientTransport(url,{fetch:recorder}));
  const tools=(await client.listTools()).tools;assert.equal(tools.length,45);tools.sort((a,b)=>a.name.localeCompare(b.name));
  const replace=value=>typeof value==='string'?value.split(workspace).join('__DEVBOX_WORKSPACE__').split(hostWorkdir).join('__HOST_WORKDIR__'):Array.isArray(value)?value.map(replace):value&&typeof value==='object'?Object.fromEntries(Object.entries(value).map(([k,v])=>[k,replace(v)])):value;
  cases[mode]=replace(tools);
 }finally{await client?.close().catch(()=>{});if(child.exitCode===null&&child.signalCode===null){child.kill();await Promise.race([exited,sleep(12000)]);if(child.exitCode===null&&child.signalCode===null){child.kill('SIGKILL');await Promise.race([exited,sleep(3000)]);}assert(child.exitCode!==null||child.signalCode!==null,'owned reference stopped');}}
}
await writeFile(path.join(output,'reference-tools.json'),JSON.stringify({source_revision:'cd8803c81ad3a14ec3b3fa2afa0d08256975b1e9',source_binary_sha256:expectedSha,profiles:cases},null,2)+'\n');
await writeFile(path.join(output,'reference-wire.json'),JSON.stringify(wire.filter(x=>x.request.method!=='tools/list'),null,2)+'\n');
await rm(root,{recursive:true,force:true,maxRetries:20,retryDelay:250});console.log('Exported both 45-tool reference profiles and protocol requests.');
