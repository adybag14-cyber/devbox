import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,mkdir,readFile,access,rm,writeFile,realpath,symlink} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {StreamableHTTPClientTransport} from '@modelcontextprotocol/sdk/client/streamableHttp.js';

assert.equal(process.platform,'linux');
const binary=process.env.DEVBOX_CPP_BINARY;
assert(binary&&path.isAbsolute(binary));
const root=await mkdtemp(path.join(os.tmpdir(),'devbox-native-desktop-'));
const workspace=path.join(root,'workspace'),desktopRoot=path.join(root,'desktop');
await mkdir(workspace);
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const exists=async file=>{try{await access(file);return true;}catch(e){if(e.code==='ENOENT')return false;throw e;}};
let display;
for(let n=150;n<250;n++)if(!await exists(`/tmp/.X11-unix/X${n}`)&&!await exists(`/tmp/.X${n}-lock`)){display=n;break;}
assert(display,'A private unused X11 display is required');
let chromium;
for(const file of [process.env.DEVBOX_DESKTOP_TEST_CHROMIUM,'/opt/google/chrome/chrome','/usr/lib/chromium/chromium','/usr/lib/chromium-browser/chromium-browser'].filter(Boolean))
  if(await exists(file)){chromium=await realpath(file);break;}
assert(chromium,'Fixture requires the actual Chromium executable');
const env=Object.fromEntries(Object.entries(process.env).filter(([key])=>['PATH','HOME','LANG','TMPDIR','USER','LOGNAME'].includes(key)));
// The native supervisor and Chromium need no xauth, xdpyinfo, Openbox, D-Bus
// daemon, shell, Python or Node executable on their child runtime PATH.
const helpers=path.join(root,'only-xvfb');await mkdir(helpers);
await symlink(await realpath('/usr/bin/Xvfb'),path.join(helpers,'Xvfb'));
env.PATH=helpers;
Object.assign(env,{
  DEVBOX_BROWSER_SHARED:'1',DEVBOX_BROWSER_HEADED:'1',DEVBOX_BROWSER_TEXT_ONLY:'1',
  DEVBOX_BROWSER_EXECUTABLE:chromium,
  DEVBOX_BROWSER_SHARED_PROFILE:path.join(desktopRoot,'browser'),
  DISPLAY:`:${display}`,XAUTHORITY:path.join(desktopRoot,'authority'),
  XDG_RUNTIME_DIR:path.join(desktopRoot,'runtime'),DEVBOX_NATIVE_DESKTOP_SENTINEL:'must-not-reach-children',
  DEVBOX_COMPUTER_USE_X11:'1',
});
const children=[];
const launch=(args,childEnv=env)=>{
  const child=spawn(binary,args,{cwd:root,env:childEnv,stdio:['ignore','pipe','pipe']});
  let log='';for(const stream of [child.stdout,child.stderr])stream.on('data',bytes=>{log=(log+bytes).slice(-16384);});
  const exited=new Promise((resolve,reject)=>{child.once('exit',(code,signal)=>resolve({code,signal}));child.once('error',reject);});
  // Keep a rejection observed while another owned process is being checked.
  exited.catch(()=>{});
  const value={child,exited,log:()=>log};children.push(value);return value;
};
async function until(check,message,timeout=30000) {
  const deadline=Date.now()+timeout;
  do {if(await check())return;await delay(100);}while(Date.now()<deadline);
  throw new Error(message());
}
const stat=async pid=>{
  try {const text=await readFile(`/proc/${pid}/stat`,'utf8');return text.slice(text.lastIndexOf(')')+2).split(' ')[19];}
  catch(e){if(e.code==='ENOENT'||e.code==='ESRCH')return null;throw e;}
};
const status=async()=>{try{return JSON.parse(await readFile(path.join(root,'run/desktop/status.json'),'utf8'));}catch(e){if(e.code==='ENOENT')return null;throw e;}};
const args=['--desktop-supervisor','--project-root',root,'--workspace',workspace,
  '--desktop-root',desktopRoot,'--display',String(display),'--chromium',chromium];
let client,supervisor,report;
const ownedIdentities=[];
try {
  supervisor=launch(args);
  await until(async()=>{assert.equal(supervisor.child.exitCode,null,supervisor.log());return (await status())?.ready;},()=>supervisor.log(),180000);
  let first=await status();
  assert.equal(first.implementation,'cpp');assert.equal(first.browser_mode,'shared_native');
  assert.equal(first.supervisor_pid,supervisor.child.pid);
  assert.equal(first.children.length,2);
  assert.equal(first.window_manager,'native C++23');
  assert.equal(first.window_manager_ready,true);
  assert.equal(first.session_bus,'disabled');
  assert.deepEqual(first.children.map(x=>x.name).sort(),['chromium','xvfb']);
  for(const child of first.children) {
    assert.equal(await stat(child.pid),child.startTicks);
    const environment=await readFile(`/proc/${child.pid}/environ`,'utf8');
    assert(!environment.includes('DEVBOX_NATIVE_DESKTOP_SENTINEL='));
    ownedIdentities.push(child);
  }
  // A second supervisor must not attach to or replace the first one's children.
  const duplicate=launch(args);const rejected=await Promise.race([duplicate.exited,delay(5000).then(()=>{throw Error('Duplicate lock did not reject');})]);
  assert.notEqual(rejected.code,0);
  assert.equal((await status()).supervisor_pid,supervisor.child.pid);
  const port=await new Promise(resolve=>{const s=net.createServer();s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>resolve(p));});});
  const server=launch([],{
    ...env,DEVBOX_PROJECT_ROOT:root,HOST:'127.0.0.1',PORT:String(port),MCP_AUTH_MODE:'none',
    DEVBOX_RUNTIME_MODE:'host',ENABLE_HOST_EXEC:'true',DEVBOX_AUTO_START:'false',
    DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE:'1',MCP_STATE_BACKEND:'legacy',
    HOST_WORKSPACE_PATH:workspace,HOST_DEFAULT_WORKDIR:workspace,DEVBOX_WORKSPACE_PATH:workspace,
    MCP_JOBS_ROOT:path.join(root,'jobs'),MCP_EXEC_SLOT_ROOT:path.join(root,'slots'),
    MCP_PERFORMANCE_STATE_PATH:path.join(root,'run/performance.json'),
  });
  await until(async()=>{assert.equal(server.child.exitCode,null,server.log());try{return (await fetch(`http://127.0.0.1:${port}/readyz`,{signal:AbortSignal.timeout(1000)})).ok;}catch{return false;}},()=>server.log());
  client=new Client({name:'native-desktop-fixture',version:'1'},{capabilities:{}});
  await client.connect(new StreamableHTTPClientTransport(new URL(`http://127.0.0.1:${port}/mcp`)));
  const call=args=>client.callTool({name:'devbox_browser',arguments:args},undefined,{timeout:45000});
  let result=await call({action:'status'});
  assert.equal(result.isError,false,JSON.stringify(result));
  assert.equal(result.structuredContent.data.available,true,JSON.stringify(result));
  assert.equal(result.structuredContent.data.headed,true);
  let windows;
  await until(async()=>{
    windows=await client.callTool({name:'host_computer_windows',arguments:{title_contains:'Chrom'}});
    assert.equal(windows.isError,false,JSON.stringify(windows));
    return windows.structuredContent.data.windows.length>0;
  },()=>JSON.stringify(windows));
  const observed=await client.callTool({name:'host_computer_use',arguments:{
    action:'observe',window_id:windows.structuredContent.data.windows[0].window_id,settle_ms:50}});
  assert.equal(observed.isError,false,JSON.stringify(observed));
  const image=observed.content.find(item=>item.type==='image');
  assert(image,'Native X11 capture of the supervised Chromium window is required');
  assert(Buffer.from(image.data,'base64').subarray(0,8).equals(Buffer.from([137,80,78,71,13,10,26,10])));
  // Kill only the fixture's Chromium after independently checking PID birth,
  // executable and the exact private profile. Other desktop children must survive.
  const original=first.children.find(child=>child.name==='chromium');
  assert(original);
  assert.equal(await stat(original.pid),original.startTicks);
  assert.equal(await realpath(`/proc/${original.pid}/exe`),chromium);
  const command=await readFile(`/proc/${original.pid}/cmdline`,'utf8');
  assert(command.split('\0').includes('--user-data-dir='+path.join(desktopRoot,'browser')),
    JSON.stringify({expectedProfile:path.join(desktopRoot,'browser'),ownedBrowserArguments:command.split('\0')}));
  process.kill(original.pid,'SIGKILL');
  await until(async()=>{
    const next=await status();
    return next?.ready&&next.browser_restarts===1&&next.children.some(child=>child.name==='chromium'&&child.pid!==original.pid);
  },()=>supervisor.log(),180000);
  const recovered=await status();
  assert(recovered.last_browser_exit?.signal===9||recovered.last_browser_exit?.exit_code===137,
    'Native supervisor must retain the owned browser termination reason');
  assert.equal(typeof recovered.browser_ready_at,'string');
  assert.equal(recovered.children.find(x=>x.name==='xvfb').pid,first.children.find(x=>x.name==='xvfb').pid);
  assert.equal(recovered.window_manager_ready,true);
  ownedIdentities.push(recovered.children.find(x=>x.name==='chromium'));
  result=await call({action:'status'});assert.equal(result.isError,false,JSON.stringify(result));
  assert.equal(result.structuredContent.data.available,true);
  report={ok:true,supervisor:'native C++23',source:process.env.GITHUB_SHA||null,
    checks:['private Xauthority and X11','real headed Chromium CDP readiness','native MCP shared-browser identity',
      'native X11 window capture','no inherited credential sentinel','exclusive supervisor lock',
      'owned Chromium crash recovery','desktop children retained','owned child shutdown']};
} finally {
  if(client)await client.close().catch(()=>{});
  for(const value of children.reverse()) {
    if(value.child.exitCode===null&&value.child.signalCode===null)value.child.kill('SIGTERM');
    await Promise.race([value.exited,delay(30000).then(()=>{throw Error('Owned desktop fixture did not stop: '+value.log());})]);
  }
  for(const identity of ownedIdentities)
    assert.notEqual(await stat(identity.pid),identity.startTicks,'retained identity indicates an orphaned owned desktop child');
  await rm(root,{recursive:true,force:true});
}
await writeFile(path.resolve('.cpp-build/native-desktop-result.json'),JSON.stringify(report,null,2)+'\n');
console.log(JSON.stringify(report));
