// Owned synthetic fixture only. A tunnel, if requested, exposes an authenticated
// four-route proxy; it never exposes an MCP endpoint or arbitrary request input.
import assert from 'node:assert/strict';
import {spawn,execFileSync} from 'node:child_process';
import {createHash,randomBytes} from 'node:crypto';
import {mkdir,writeFile,readFile,access} from 'node:fs/promises';
import http from 'node:http';
import https from 'node:https';
import {Resolver} from 'node:dns/promises';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const [binaryArgument,outputArgument,tunnelBinary]=process.argv.slice(2);
assert(['win32','linux'].includes(process.platform),'CPU qualification supports Windows and Linux');
assert(binaryArgument&&outputArgument,'Pass absolute candidate binary, NEW output directory, optional cloudflared');
const binary=path.resolve(binaryArgument),root=path.resolve(outputArgument);
await mkdir(root);await mkdir(path.join(root,'fixture'));
const workspace=path.join(root,'fixture');
const samples=Number(process.env.DEVBOX_BENCH_SAMPLES||1000),runs=Number(process.env.DEVBOX_BENCH_RUNS||3);
assert(Number.isInteger(samples)&&samples>=1000&&samples<=10000);
assert(Number.isInteger(runs)&&runs>=1&&runs<=10&&(runs>=3||process.env.DEVBOX_BENCH_PAIRED_ROUND==='1'));
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const digest=hash(await readFile(binary));
const clean={};
for(const [key,value]of Object.entries(process.env)) if(['PATH','SYSTEMROOT','WINDIR','SYSTEMDRIVE','TEMP','TMP','TMPDIR','HOME','USERPROFILE','LANG','LOCALAPPDATA','APPDATA','PATHEXT'].includes(key.toUpperCase())) clean[key]=value;
const identity=JSON.parse(execFileSync(binary,['--build-info'],{env:clean,cwd:root,windowsHide:true,timeout:10000,encoding:'utf8'}));
assert.equal(identity.binarySha256,digest);
const freePort=()=>new Promise((resolve,reject)=>{const s=net.createServer();s.once('error',reject);s.listen(0,'127.0.0.1',()=>{const port=s.address().port;s.close(()=>resolve(port));});});
const port=await freePort(),base=`http://127.0.0.1:${port}`;
const stateBackend=process.env.DEVBOX_BENCH_STATE_BACKEND||'legacy';assert(['legacy','sqlite'].includes(stateBackend));
const env={...clean,DEVBOX_PROJECT_ROOT:workspace,HOST:'127.0.0.1',PORT:String(port),MCP_AUTH_MODE:'none',PUBLIC_BASE_URL:'',
  MCP_STATE_BACKEND:stateBackend,DEVBOX_RUNTIME_MODE:'host',ENABLE_HOST_EXEC:'true',DEVBOX_AUTO_START:'false',ENABLE_GATEWAY_BRIDGE:'false',
  HOST_WORKSPACE_PATH:workspace,HOST_DEFAULT_WORKDIR:workspace,DEVBOX_WORKSPACE_PATH:workspace,NODE_EXE:process.execPath,
  MCP_JOBS_ROOT:path.join(workspace,'jobs'),MCP_EXEC_SLOT_ROOT:path.join(workspace,'slots'),
  MCP_STATE_ROOT:path.join(workspace,'state'),MCP_PERFORMANCE_STATE_PATH:path.join(workspace,'run/performance.json'),
  MCP_USAGE_LOG_MAX_BYTES:'67108864',MCP_USAGE_LOG_ROTATIONS:'2',DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE:'1'};
const data=Buffer.alloc(4096,120);await writeFile(path.join(workspace,'fixed.bin'),data);
const children=[];let proxy;let sequence=0;const observations=[];
const pinned=process.env.DEVBOX_BENCH_PIN_WINDOWS==='1';
let affinity=[];
async function pin(pid,expectedPath,expectedParent,mask) {
  assert.equal(process.platform,'win32');assert(os.cpus().length>=32,'Pinned qualification requires the recorded 32-thread host');
  const helper=path.join(root,'pin-owned.ps1');
  const script=`param([int]$OwnedId,[string]$ExpectedPath,[int]$ExpectedParent,[long]$Mask)
$ErrorActionPreference='Stop'
$identity=Get-CimInstance Win32_Process -Filter ('ProcessId='+$OwnedId)
if (!$identity -or $identity.ExecutablePath -ne $ExpectedPath -or ($ExpectedParent -ne 0 -and $identity.ParentProcessId -ne $ExpectedParent)) { throw 'Owned benchmark process identity changed' }
$owned=Get-Process -Id $OwnedId
# CIM timestamps have microsecond resolution; Process.StartTime retains 100 ns.
$tickDelta=$owned.StartTime.ToUniversalTime().Ticks-$identity.CreationDate.ToUniversalTime().Ticks
if ($owned.HasExited -or $owned.Path -ne $ExpectedPath -or [Math]::Abs($tickDelta) -gt 9) { throw 'Owned benchmark instance changed' }
$owned.ProcessorAffinity=[IntPtr]$Mask
@{pid=$OwnedId;path=$owned.Path;startedAt=$owned.StartTime.ToUniversalTime().ToString('o');mask=$owned.ProcessorAffinity.ToInt64()}|ConvertTo-Json -Compress
`;
  await writeFile(helper,script);
  affinity.push(JSON.parse(execFileSync('powershell.exe',['-NoProfile','-NonInteractive','-File',helper,String(pid),expectedPath,String(expectedParent),String(mask)],
    {windowsHide:true,timeout:15000,encoding:'utf8'})));
}
function launch(file,args,environment) {
  const child=spawn(file,args,{cwd:root,env:environment,windowsHide:true,stdio:['ignore','pipe','pipe']});
  const record={file,args,pid:child.pid,startedAt:new Date().toISOString(),child,output:''};
  record.done=new Promise((resolve,reject)=>{child.once('exit',(code,signal)=>resolve({code,signal}));child.once('error',reject);});
  for(const stream of[child.stdout,child.stderr])stream.on('data',chunk=>{record.output=(record.output+chunk.toString()).slice(-32768);});
  children.push(record);return record;
}
if(pinned)await pin(process.pid,process.execPath,0,4294901760);
const service=launch(binary,[],env);
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const publicDns=process.env.DEVBOX_BENCH_PUBLIC_DNS==='1';
const dns=new Resolver({timeout:2000,tries:2});if(publicDns)dns.setServers(['1.1.1.1']);
const resolved=new Map();
const localAgent=new http.Agent({keepAlive:true,maxSockets:4});
const remoteAgent=new https.Agent({keepAlive:true,maxSockets:4,...(publicDns?{lookup:(host,options,done)=>{
  if(!host.endsWith('.trycloudflare.com'))return done(new Error('Only the owned synthetic tunnel may use this resolver'));
  (async()=>{if(!resolved.has(host))resolved.set(host,await dns.resolve4(host));const addresses=resolved.get(host);
    assert(addresses.length>0&&addresses.every(ip=>ip==='104.16.230.132'||ip==='104.16.231.132'),'Expected observed public Cloudflare edge addresses');
    const choices=addresses.map(address=>({address,family:4}));if(options.all)done(null,choices);else done(null,choices[0].address,4);
  })().catch(done);
}}:{})});
// Identical core HTTP client for both measured paths. TLS certificate and SNI
// verification remain enabled even when the explicit test-only DNS resolver is selected.
const request=(url,options={})=>new Promise((resolve,reject)=>{
  const address=new URL(url),remote=address.protocol==='https:';
  const req=(remote?https:http).request(address,{method:options.method||'GET',headers:options.headers,
    agent:remote?remoteAgent:localAgent,signal:AbortSignal.timeout(30000)},response=>{
    const parts=[];let size=0;
    response.on('data',chunk=>{size+=chunk.length;if(size>32*1024*1024){req.destroy(new Error('Benchmark response byte budget'));return;}parts.push(chunk);});
    response.on('error',reject);response.on('end',()=>{const body=Buffer.concat(parts,size).toString('utf8');resolve({
      status:response.statusCode,ok:response.statusCode>=200&&response.statusCode<300,
      headers:{get:key=>response.headers[key.toLowerCase()]},text:async()=>body,json:async()=>JSON.parse(body)});});
  });req.on('error',reject);req.end(options.body);
});
async function cpu() {
  if(process.platform==='win32') return Number(execFileSync('powershell.exe',['-NoProfile','-NonInteractive','-Command',
    `(Get-Process -Id ${service.pid}).TotalProcessorTime.TotalMilliseconds.ToString([Globalization.CultureInfo]::InvariantCulture)`],
    {windowsHide:true,timeout:10000,encoding:'utf8'}).trim());
  const stat=await readFile(`/proc/${service.pid}/stat`,'utf8');const fields=stat.slice(stat.lastIndexOf(')')+2).split(' ');
  const ticks=Number(execFileSync('getconf',['CLK_TCK'],{encoding:'utf8'}));return(Number(fields[11])+Number(fields[12]))*1000/ticks;
}
try {
  if(pinned)await pin(service.pid,binary,process.pid,65535);
  const until=Date.now()+20000;for(;;){assert(Date.now()<until,service.output);assert.equal(service.child.exitCode,null,service.output);
    try{const r=await request(base);if(r.ok){assert.equal((await r.json()).build.binarySha256,digest);break;}}catch(e){if(e.code==='ERR_ASSERTION')throw e;}await pause(50);}
  const headers={'content-type':'application/json',accept:'application/json, text/event-stream'};
  const rpc=async(method,params)=>{const id=++sequence;const response=await request(`${base}/mcp`,{method:'POST',headers,body:JSON.stringify({jsonrpc:'2.0',id,method,params})});
    const session=response.headers.get('mcp-session-id');if(session)headers['mcp-session-id']=session;
    assert.equal(response.status,200);const bytes=await response.text();
    const values=response.headers.get('content-type')?.includes('text/event-stream')?
      bytes.split('\n').filter(line=>line.startsWith('data:')).map(line=>JSON.parse(line.slice(5))):[JSON.parse(bytes)];
    const value=values.find(row=>row.id===id);assert(value,'Matching RPC result required');assert(!value.error,JSON.stringify(value));return value;};
  await rpc('initialize',{protocolVersion:'2025-03-26',capabilities:{},clientInfo:{name:'owned-matched-qualification',version:'1'}});
  const operations={
    health:async()=>{const response=await request(`${base}/healthz`);assert.equal(await response.text(),'ok');return {ok:true};},
    capabilities:()=>rpc('tools/call',{name:'devbox_capabilities',arguments:{tool_name:'devbox_web_research'}}),
    read4k:()=>rpc('tools/call',{name:'devbox_read_large_file',arguments:{path:path.join(workspace,'fixed.bin'),max_bytes:4096}}),
    child:()=>rpc('tools/call',{name:'host_run_program',arguments:{program:'node',args:['-e',"process.stdout.write('fixed-benchmark-ok')"],working_dir:workspace,timeout_seconds:10}}),
  };
  const classes=(process.env.DEVBOX_BENCH_CLASSES||Object.keys(operations).join(',')).split(',');
  assert(classes.length>0&&classes.every(name=>Object.hasOwn(operations,name)));
  const token=randomBytes(32).toString('hex');
  proxy=http.createServer(async(req,res)=>{
    if(req.method!=='GET'||req.headers.authorization!==`Bearer ${token}`||!Object.hasOwn(operations,req.url.slice(1))) {res.writeHead(403).end();return;}
    // No body, path, argument, environment or destination is accepted from callers.
    try {const value=await operations[req.url.slice(1)]();assert(!value.result?.isError,JSON.stringify(value));
      const bytes=JSON.stringify(value);res.writeHead(200,{'content-type':'application/json','cache-control':'no-store','content-length':Buffer.byteLength(bytes)}).end(bytes);
    }catch(error){res.writeHead(502,{'content-type':'application/json'}).end(JSON.stringify({fixtureError:String(error)}));}
  });
  proxy.maxConnections=8;proxy.requestTimeout=30000;proxy.headersTimeout=10000;
  await new Promise(resolve=>proxy.listen(0,'127.0.0.1',resolve));const local=`http://127.0.0.1:${proxy.address().port}`;
  assert.equal((await request(`${local}/health`)).status,403,'Proxy rejects missing authentication');
  assert.equal((await request(`${local}/mcp`,{headers:{authorization:`Bearer ${token}`}})).status,403,'No MCP route exposed');
  const routes={localhost:local};
  if(tunnelBinary) {
    const config=path.join(root,'owned-cloudflared.yml');await writeFile(config,'{}\n',{flag:'wx'});
    const tunnel=launch(path.resolve(tunnelBinary),['tunnel','--config',config,'--no-autoupdate','--protocol','http2','--url',local],clean);
    const deadline=Date.now()+60000;let url;
    while(!(url=tunnel.output.match(/https:\/\/[a-z0-9-]+\.trycloudflare\.com/u)?.[0])) {assert(Date.now()<deadline&&tunnel.child.exitCode===null,tunnel.output);await pause(250);}
    routes.tunnel=url;
    // Record only the synthetic public route, never its ephemeral bearer token.
    await writeFile(path.join(root,'tunnel.json'),JSON.stringify({url,pid:tunnel.pid,startedAt:tunnel.startedAt,readOnlyFixedRoutes:true})+'\n');
    let ready=false,lastFailure='';const readyDeadline=Date.now()+90000;
    while(!ready&&Date.now()<readyDeadline){try{const r=await request(`${url}/health`,{headers:{authorization:`Bearer ${token}`}});
      const body=await r.text();ready=r.ok&&body.includes('"ok":true');lastFailure=`HTTP ${r.status}: ${body.slice(0,300)}`;
    }catch(error){lastFailure=String(error)+': '+String(error.cause);}if(!ready)await pause(1000);}
    assert(ready,`Owned tunnel did not become ready: ${lastFailure}`);
  }
  const metadata={schema:1,build:identity,binarySha256:digest,stateBackend,samplesPerClassPerRun:samples,runs,classes,concurrency:4,
    cpu:os.cpus().map(c=>c.model),platform:os.platform(),release:os.release(),payloadSha256:hash(data),cacheState:'warm OS file and connection caches; no response caching',
    fixtureOnly:true,productionRequests:0,startedAt:new Date().toISOString(),affinity,testResolver:publicDns?'1.1.1.1 for the owned tunnel only':'system',
    powerProfile:process.platform==='win32'?execFileSync('powercfg',['/getactivescheme'],{encoding:'utf8',windowsHide:true}).trim():null};
  await writeFile(path.join(root,'metadata.json'),JSON.stringify(metadata,null,2)+'\n');
  const routeOrder=round=>round%2?Object.entries(routes).reverse():Object.entries(routes);
  const overallDeadline=Date.now()+45*60*1000;
  for(let round=0;round<runs;round++)for(const [route,url]of routeOrder(round))for(const name of classes) {
    const call=async()=>{assert(Date.now()<overallDeadline,'Overall benchmark budget');const begin=performance.now();const response=await request(`${url}/${name}`,{headers:{authorization:`Bearer ${token}`}});const value=await response.json();assert.equal(response.status,200,JSON.stringify(value));assert(!value.result?.isError,JSON.stringify(value));
      if(name==='read4k')assert.equal(value.result.structuredContent.data.content_base64,data.toString('base64'));
      if(name==='child')assert(JSON.stringify(value).includes('fixed-benchmark-ok'));
      return performance.now()-begin;};
    for(let i=0;i<20;i++)await call();
    const before=await cpu(),start=new Date().toISOString();const begin=performance.now();const latency=new Array(samples);let next=0;
    await Promise.all(Array.from({length:4},async()=>{for(;;){const i=next++;if(i>=samples)return;latency[i]=await call();}}));
    const wallMs=performance.now()-begin,end=new Date().toISOString(),serviceCpuMs=(await cpu())-before;
    const row={round,route,name,start,end,wallMs,serviceCpuMs,latencyMs:latency,failures:0};observations.push(row);
    await writeFile(path.join(root,'observations.json'),JSON.stringify(observations)+'\n');
    const sorted=[...latency].sort((a,b)=>a-b);console.log(JSON.stringify({round,route,name,samples,wallMs,serviceCpuMs,p50:sorted[Math.floor(samples*.5)],p95:sorted[Math.floor(samples*.95)],p99:sorted[Math.floor(samples*.99)]}));
  }
  await writeFile(path.join(root,'complete.json'),JSON.stringify({ok:true,observations:observations.length*samples,binarySha256:digest,productionRequests:0})+'\n');
} finally {
  localAgent.destroy();remoteAgent.destroy();
  proxy?.closeAllConnections();if(proxy)await new Promise(resolve=>proxy.close(resolve));
  for(const record of [...children].reverse()) {
    if(record.child.exitCode===null&&record.child.signalCode===null)record.child.kill('SIGTERM');
    await Promise.race([record.done,pause(10000)]);
    if(record.child.exitCode===null&&record.child.signalCode===null)record.child.kill('SIGKILL');
    const result=await record.done;
    await writeFile(path.join(root,`owned-${record.pid}.json`),JSON.stringify({file:record.file,args:record.args,pid:record.pid,startedAt:record.startedAt,result})+'\n');
    await writeFile(path.join(root,`owned-${record.pid}.log`),record.output);
  }
  if(stateBackend==='sqlite') {
    try {await access(path.join(env.MCP_STATE_ROOT,'coordinator.json'));
      execFileSync(binary,['--stop-state-coordinator',env.MCP_STATE_ROOT],{env,cwd:root,windowsHide:true,timeout:15000,encoding:'utf8'});
    }catch(error){if(error.code!=='ENOENT')throw error;}
  }
}
