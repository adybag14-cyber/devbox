// Development-only differential check. The shipped C++ binary never starts Rust.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,mkdir,readFile,rm} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {StreamableHTTPClientTransport} from '@modelcontextprotocol/sdk/client/streamableHttp.js';
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const reference=process.env.DEVBOX_RUST_REFERENCE_BINARY||path.join(repo,'.cpp-build/reference/devbox-rust-reference.exe');
const candidate=process.env.DEVBOX_CPP_BINARY||path.join(repo,'.cpp-build/windows/cpp-mcp/Release/devbox-mcp.exe');
assert.equal(createHash('sha256').update(await readFile(reference)).digest('hex'),'6b33147a1032368a293557705811047cd4be01b080000de0de7fc2f4dcad0114','frozen reference identity');
const root=await mkdtemp(path.join(os.tmpdir(),'devbox-cpp-schema-'));
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const freePort=()=>new Promise((resolve,reject)=>{const s=net.createServer();s.once('error',reject);s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>resolve(p));});});
const variants=[['host',{}],['docker',{}],['host',{MAX_COMMAND_OUTPUT_CHARS:'8192',MAX_TEXT_OUTPUT_CHARS:'900',MAX_MCP_TRANSFER_CHARS:'262144',MCP_WAIT_MAX_SECONDS:'3'}],['host',{MAX_COMMAND_OUTPUT_CHARS:'0',MAX_TEXT_OUTPUT_CHARS:'0',MAX_MCP_TRANSFER_CHARS:'0'}]];
const results=[];
try {
  for(const [index,[mode,overrides]] of variants.entries()) {
    const state=path.join(root,String(index));await mkdir(state);
    const workspace=path.join(state,'workspace'),hostDir=path.join(state,'host');await mkdir(workspace);await mkdir(hostDir);
    const port=await freePort();
    const env={...process.env,DEVBOX_PROJECT_ROOT:state,HOST:'127.0.0.1',PORT:String(port),MCP_AUTH_MODE:'none',PUBLIC_BASE_URL:'',DEVBOX_RUNTIME_MODE:mode,ENABLE_HOST_EXEC:'false',DEVBOX_AUTO_START:'false',DEVBOX_CONTAINER_NAME:`devbox-cpp-schema-${process.pid}`,HOST_WORKSPACE_PATH:workspace,DEVBOX_WORKSPACE_PATH:workspace,HOST_DEFAULT_WORKDIR:hostDir,DEVBOX_DEFAULT_USER:'schema-fixture-user',NODE_EXE:process.execPath,MCP_JOBS_ROOT:path.join(state,'jobs'),MCP_EXEC_SLOT_ROOT:path.join(state,'slots'),MCP_PERFORMANCE_STATE_PATH:path.join(state,'performance.json'),MAX_COMMAND_OUTPUT_CHARS:'65536',MAX_TEXT_OUTPUT_CHARS:'4000000',MAX_MCP_TRANSFER_CHARS:'4000000',MCP_WAIT_MAX_SECONDS:'300',DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE:'0',...overrides};
    const server=spawn(reference,[],{cwd:repo,env,windowsHide:true,stdio:['ignore','pipe','pipe']});
    let logs='';for(const stream of [server.stdout,server.stderr])stream.on('data',s=>{logs=(logs+s).slice(-16000);});
    const exited=new Promise((resolve,reject)=>{server.once('exit',resolve);server.once('error',reject);});let client;
    try {
      const base=`http://127.0.0.1:${port}`;const deadline=Date.now()+15000;
      for(;;){assert(Date.now()<deadline,logs);assert.equal(server.exitCode,null,logs);try{if((await fetch(`${base}/healthz`,{signal:AbortSignal.timeout(500)})).ok)break;}catch{}await delay(50);}
      client=new Client({name:'cpp-schema-differential',version:'1'});
      await client.connect(new StreamableHTTPClientTransport(new URL(base)));
      const expected=(await client.listTools()).tools;
      const dump=spawn(candidate,['--dump-contract'],{cwd:repo,env,windowsHide:true,stdio:['ignore','pipe','pipe']});
      let output='',error='';dump.stdout.on('data',s=>{output+=s;});dump.stderr.on('data',s=>{error+=s;});
      const code=await new Promise((resolve,reject)=>{dump.once('exit',resolve);dump.once('error',reject);});
      assert.equal(code,0,error);const actual=JSON.parse(output);
      const agentNames=new Set(['devbox_capabilities','devbox_file_state','devbox_write_file_atomic','devbox_job_submit','devbox_job_list','devbox_task_get','devbox_task_put','devbox_task_list']);
      for(const tool of expected)if(agentNames.has(tool.name))for(const field of ['title','description'])if(tool[field])tool[field]=tool[field].replaceAll('Rust','C++');
      const sort=tools=>tools.sort((a,b)=>a.name.localeCompare(b.name));
      assert.deepEqual(sort(actual),sort(expected),`full contract variant ${index} (${mode})`);
      results.push({mode,variant:index,tools:actual.length,match:true});
    } finally {
      await client?.close().catch(()=>{});
      if(server.exitCode===null&&server.signalCode===null){server.kill();await Promise.race([exited,delay(10000)]);}
      assert(server.exitCode!==null||server.signalCode!==null,'owned reference process stopped');
    }
  }
  console.log(JSON.stringify({ok:true,comparison:'full schemas and metadata except declared implementation name',profiles:results},null,2));
} finally { await rm(root,{recursive:true,force:true,maxRetries:20,retryDelay:200}); }
