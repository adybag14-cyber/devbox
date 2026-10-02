import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, mkdir, readFile, writeFile, rm} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import {fileURLToPath} from 'node:url';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {StreamableHTTPClientTransport} from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
import {stopStateFixture} from './state-fixture.mjs';

assert.equal(process.platform, 'linux');
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const binary=process.env.DEVBOX_MCP_TEST_BINARY; assert(binary && path.isAbsolute(binary));
const root=await mkdtemp(path.join(os.tmpdir(),'devbox-native-input-'));
const children=[]; let client, server;
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(check, message, timeout=15000) {
  const end=Date.now()+timeout;
  while(!await check()) {assert(Date.now()<end,message());await delay(25);}
}
function child(file,args,env,display=false) {
  const handle=spawn(file,args,{cwd:root,env,stdio:display?['ignore','pipe','pipe','pipe']:['pipe','pipe','pipe']});
  let output='';for(const stream of [handle.stdout,handle.stderr])stream.on('data',b=>{output=(output+b).slice(-100000);});
  const exited=new Promise((resolve,reject)=>{handle.once('exit',resolve);handle.once('error',reject);});
  const state={handle,exited,output:()=>output};children.push(state);return state;
}
const run=(file,args,env=process.env)=>runCheckedProcess(file,args,{cwd:root,env,timeoutMs:30000,label:'Owned X11 input fixture'});
try {
  await run('c++',['-std=c++23','-O2',path.join(repo,'cpp-mcp/tests/computer_x11_fixture.cpp'),'-lX11','-o',path.join(root,'fixture')]);
  const display=child('Xvfb',['-displayfd','3','-screen','0','1024x768x24','-nolisten','tcp','-noreset'],process.env,true);
  const number=await new Promise((resolve,reject)=>{
    let output='';const timer=setTimeout(()=>reject(new Error(display.output())),10000);
    display.handle.stdio[3].on('data',bytes=>{output+=bytes;if(output.includes('\n')){clearTimeout(timer);resolve(output.trim());}});
    display.exited.then(()=>{clearTimeout(timer);reject(new Error(display.output()));},reject);
  });
  assert.match(number,/^\d+$/);
  const env={...process.env,DISPLAY:`:${number}`,DEVBOX_COMPUTER_USE_X11:'1'};
  delete env.WAYLAND_DISPLAY;delete env.DEVBOX_COMPUTER_USE_PIPE;
  delete env.SESSION_MANAGER;delete env.DBUS_SESSION_BUS_ADDRESS;
  env.HOME=path.join(root,'home');await mkdir(env.HOME);
  if(process.env.DEVBOX_X11_TEST_WM==='1') {
    const wm=child('openbox',['--sm-disable'],env);
    await until(async()=>{const value=await run('xprop',['-root','_NET_SUPPORTING_WM_CHECK'],env);return value.stdout.includes('window id #');},()=>wm.output());
  }
  const window=child(path.join(root,'fixture'),[],env);
  await until(()=>window.output().includes('ready '),()=>window.output());
  const port=await new Promise(resolve=>{const socket=net.createServer();socket.listen(0,'127.0.0.1',()=>{const p=socket.address().port;socket.close(()=>resolve(p));});});
  const workspace=path.join(root,'workspace');await mkdir(workspace);
  Object.assign(env,{DEVBOX_PROJECT_ROOT:root,HOST:'127.0.0.1',PORT:String(port),MCP_AUTH_MODE:'none',PUBLIC_BASE_URL:'',DEVBOX_RUNTIME_MODE:'host',ENABLE_HOST_EXEC:'true',DEVBOX_AUTO_START:'false',HOST_WORKSPACE_PATH:workspace,DEVBOX_WORKSPACE_PATH:workspace,HOST_DEFAULT_WORKDIR:workspace,HOST_SHELL:'/bin/sh',MCP_STATE_ROOT:path.join(root,'run/state'),MCP_JOBS_ROOT:path.join(root,'jobs'),MCP_EXEC_SLOT_ROOT:path.join(root,'slots'),DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE:'1'});
  server=child(binary,[],env);
  const base=`http://127.0.0.1:${port}`;
  const digest=createHash('sha256').update(await readFile(binary)).digest('hex');
  await until(async()=>{try{const r=await fetch(base,{signal:AbortSignal.timeout(500)});const value=await r.json();assert.equal(value.build.binarySha256,digest);return true;}catch{return false;}},()=>server.output());
  client=new Client({name:'native-x11-input-test',version:'1'},{capabilities:{}});
  await client.connect(new StreamableHTTPClientTransport(new URL(base+'/mcp')));
  const invoke=async(name,args={})=>{
    const result=await client.callTool({name,arguments:args});assert.equal(result.isError,false,JSON.stringify(result));
    return {data:result.structuredContent.data,image:result.content.find(c=>c.type==='image')};
  };
  const capability=await invoke('devbox_capabilities');
  assert.equal(capability.data.computer_use.supported,true);
  assert.equal(capability.data.platform_availability.desktop_input,'permission_probe_on_use');
  const reject=async(args,pattern)=>{const value=await client.callTool({name:'host_computer_use',arguments:args});assert.equal(value.isError,true);assert.match(JSON.stringify(value),pattern);};
  window.handle.stdin.write('map\n');
  await until(()=>window.output().includes('ack map'),()=>window.output());
  let inventory;
  try {
    await until(async()=>{inventory=await invoke('host_computer_windows',{title_contains:'Devbox native input fixture'});return inventory.data.windows.length===1;},()=>JSON.stringify(inventory)+server.output()+window.output());
  } catch(error) {
    console.error((await run('xprop',['-root','_NET_CLIENT_LIST_STACKING','_NET_CLIENT_LIST'],env)).stdout);
    console.error((await run('xwininfo',['-root','-tree'],env)).stdout);
    for(const item of children)console.error({pid:item.handle.pid,exited:item.handle.exitCode,output:item.output(),wait:await readFile(`/proc/${item.handle.pid}/wchan`,'utf8').catch(()=>'?')});
    throw error;
  }
  assert.equal(inventory.data.supported,true);assert.equal(inventory.data.windows.length,1,JSON.stringify(inventory)+server.output());
  const id=inventory.data.windows[0].window_id;
  let observed;
  const observe=async()=>{observed=await invoke('host_computer_use',{action:'observe',window_id:id,settle_ms:50});return observed;};
  const act=async(action,args={})=>{observed=await invoke('host_computer_use',{action,observation_id:observed.data.observation_id,settle_ms:50,...args});return observed;};
  await observe();assert.equal(observed.data.image_width,800);assert.equal(observed.data.image_height,600);
  const png=Buffer.from(observed.image.data,'base64');assert(png.subarray(0,8).equals(Buffer.from([137,80,78,71,13,10,26,10])));
  await writeFile(path.join(root,'observed.png'),png);
  assert.match((await run('convert',[path.join(root,'observed.png'),'-crop','1x1+40+40','-depth','8','txt:-'])).stdout,/#2978E6/i);
  const before=observed.data.observation_id;
  await act('click',{x:100,y:100});assert.match(window.output(),/button_down 1\nbutton_up 1/);
  await reject({action:'click',observation_id:before,x:100,y:100},/COMPUTER_STALE_OBSERVATION/);
  await reject({action:'scroll',observation_id:observed.data.observation_id,scroll_y:3},/COMPUTER_COORDINATES_REQUIRED/);
  await act('scroll',{x:500,y:300,scroll_y:3,scroll_x:-2});
  assert.equal(observed.data.scroll.wheel_events_sent,5);assert.equal(observed.data.scroll.content_movement_verified,false);
  assert.equal((window.output().match(/button_down 5\n/g)||[]).length,3);
  assert.equal((window.output().match(/button_down 6\n/g)||[]).length,2);
  await act('type',{text:'Ab9 £éλ🙂'});
  // MCP reports delivery, not application processing. Wait for the real event
  // receiver (including Xlib's first-use keyboard-map initialization) to ack.
  for(const key of [65,98,57,163,233,0x10003bb,0x101f642])
    await until(()=>window.output().includes(`key_down ${key}\n`),()=>`Unicode keysym ${key}: ${window.output()}`);
  const beforeBudget=window.output().length;
  await reject({action:'type',observation_id:observed.data.observation_id,text:'λ'.repeat(100)},/COMPUTER_TEXT_BUDGET/);
  assert.equal(window.output().length,beforeBudget,'over-budget text emits no input');
  await act('type',{text:'Still usable'});
  await act('key',{keys:['CTRL','L']});
  await act('key_sequence',{sequence:[{keys:['UP'],duration_ms:30},{keys:['UP','RIGHT'],duration_ms:30},{keys:[],duration_ms:10}]});
  await act('drag',{path:[{x:100,y:200},{x:250,y:200},{x:400,y:200}],duration_ms:100});
  assert.match(window.output(),/motion 400 200/);
  await reject({action:'key',observation_id:observed.data.observation_id,keys:['CTRL','ALT','F1']},/COMPUTER_SYSTEM_SHORTCUT_DENIED/);
  async function control(command){const start=window.output().length;window.handle.stdin.write(command+'\n');await until(()=>window.output().slice(start).includes('ack '+command),()=>window.output());}
  await control('title');await reject({action:'click',observation_id:observed.data.observation_id,x:100,y:100},/COMPUTER_STALE_OBSERVATION/);
  await observe();await control('cover');await reject({action:'click',observation_id:observed.data.observation_id,x:100,y:100},/COMPUTER_WINDOW_OCCLUDED/);
  await control('uncover');await observe();
  await control('owned_popup');await act('click',{x:70,y:70});await control('uncover');await observe();
  await control('steal_ping');
  const beforeSteal=window.output().length;
  await reject({action:'type',observation_id:observed.data.observation_id,text:'λ'},/COMPUTER_STALE_OBSERVATION/);
  assert(!window.output().slice(beforeSteal).includes('key_down '),'focus loss during mapping acknowledgement sends no glyph');
  await observe();
  const controller=new AbortController();
  const beforeCancel=window.output().length;
  const cancelled=client.callTool({name:'host_computer_use',arguments:{action:'key',observation_id:observed.data.observation_id,keys:['SHIFT'],hold_ms:5000}},undefined,{signal:controller.signal}).catch(()=>{});
  await until(()=>window.output().slice(beforeCancel).includes('key_down 65505\n'),()=>window.output());controller.abort();await cancelled;
  await until(()=>window.output().slice(beforeCancel).includes('key_up 65505\n'),()=>window.output());
  await observe();await act('click',{x:10,y:10});
  await control('resize');await reject({action:'click',observation_id:observed.data.observation_id,x:10,y:10},/COMPUTER_STALE_OBSERVATION/);
  await observe();assert.equal(observed.data.image_width,720);
  display.handle.kill();await display.exited;
  const disconnected=await client.callTool({name:'host_computer_windows',arguments:{}});
  assert.equal(disconnected.isError,true);assert.equal((await fetch(base+'/healthz')).status,200);
  const result={ok:true,binarySha256:digest,windowManager:process.env.DEVBOX_X11_TEST_WM==='1',checks:['real pixels','native click','single-use IDs','point-targeted vertical and horizontal scroll','literal Unicode','key chord','timed key sequence','drag','stale title and geometry','occlusion rejection','cancellation releases keys','display loss preserves MCP health']};
  await mkdir(path.join(repo,'.cpp-build'),{recursive:true});
  await writeFile(path.join(repo,'.cpp-build/x11-computer-result.json'),JSON.stringify(result,null,2));console.log(JSON.stringify(result,null,2));
} finally {
  await client?.close().catch(()=>{});
  for(const state of children.reverse()) {
    if(state.handle.exitCode===null && state.handle.signalCode===null){state.handle.kill();await Promise.race([state.exited,delay(5000)]);}
    if(state.handle.exitCode===null && state.handle.signalCode===null){state.handle.kill('SIGKILL');await state.exited;}
  }
  if(server)await stopStateFixture(binary,root);
  await rm(root,{recursive:true,force:true});
}
