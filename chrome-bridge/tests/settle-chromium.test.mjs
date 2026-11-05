import test from 'node:test';
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {existsSync,mkdtempSync,readFileSync,rmSync} from 'node:fs';
import {dirname,join,resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {ChromeBridge} from '../extension/controller.mjs';
import {ActionContext} from '../extension/state.mjs';

// A real Chrome scheduler regression, using only a private headless profile.
// Node hosts the extension controller; the page's timers run in real Chrome.
test('real frozen Chrome cannot hold the extension settling deadline', {timeout:25000}, async()=>{
 const chrome=[process.env.DEVBOX_TEST_CHROME,process.env.ProgramFiles&&join(process.env.ProgramFiles,'Google/Chrome/Application/chrome.exe'),'/usr/bin/google-chrome','/usr/bin/chromium'].find(p=>p&&existsSync(p));
 assert(chrome,'Set DEVBOX_TEST_CHROME to an existing Chrome executable');
 const parent=resolve(tmpdir()),profile=mkdtempSync(join(parent,'devbox-settle-chromium-'));
 assert.equal(dirname(profile),parent);
 const child=spawn(chrome,['--headless=new','--no-first-run','--no-default-browser-check','--disable-extensions','--remote-debugging-address=127.0.0.1','--remote-debugging-port=0',`--user-data-dir=${profile}`,'about:blank'],{windowsHide:true,stdio:['ignore','ignore','pipe']});
 let stderr='',socket,serial=0;const pending=new Map();
 child.stderr.on('data',bytes=>{stderr=(stderr+bytes).slice(-6000);});
 const exited=new Promise((resolve,reject)=>{child.once('exit',resolve);child.once('error',reject);});
 const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
 function command(method,params={},sessionId){
  return new Promise((resolve,reject)=>{
   const id=++serial,timer=setTimeout(()=>{pending.delete(id);reject(new Error('CDP deadline: '+method));},4000);
   pending.set(id,{resolve,reject,timer});socket.send(JSON.stringify({id,method,params,...(sessionId?{sessionId}:{})}));
  });
 }
 try{
  const end=Date.now()+12000,portFile=join(profile,'DevToolsActivePort');
  while(!existsSync(portFile)){assert.equal(child.exitCode,null,stderr);assert(Date.now()<end,'Chrome startup deadline: '+stderr);await delay(25);}
  const [port,path]=readFileSync(portFile,'utf8').trim().split(/\r?\n/);
  assert(/^\d+$/.test(port)&&path.startsWith('/devtools/browser/'));
  socket=new WebSocket(`ws://127.0.0.1:${port}${path}`);
  await new Promise((resolve,reject)=>{socket.addEventListener('open',resolve,{once:true});socket.addEventListener('error',reject,{once:true});});
  socket.addEventListener('message',event=>{const value=JSON.parse(event.data),wait=pending.get(value.id);if(!wait)return;pending.delete(value.id);clearTimeout(wait.timer);if(value.error)wait.reject(new Error(value.error.message));else wait.resolve(value.result);});
  const {targetId}=await command('Target.createTarget',{url:'about:blank'});
  const {sessionId}=await command('Target.attachToTarget',{targetId,flatten:true});
  const page=(method,params)=>command(method,params,sessionId);
  await page('Page.enable');
  const {frameTree}=await page('Page.getFrameTree');
  const {executionContextId}=await page('Page.createIsolatedWorld',{frameId:frameTree.frame.id,worldName:'devbox-settle-regression',grantUniveralAccess:false});
  // Demonstrate the browser condition itself before exercising the controller.
  await page('Page.setWebLifecycleState',{state:'frozen'});
  await page('Runtime.evaluate',{contextId:executionContextId,expression:'globalThis.timerRan=false;setTimeout(()=>{globalThis.timerRan=true},10);true',returnByValue:true});
  await delay(80);
  const frozen=await page('Runtime.evaluate',{contextId:executionContextId,expression:'globalThis.timerRan',returnByValue:true});
  assert.equal(frozen.result.value,false,'the page timer must actually be suspended');
  const bridge=new ChromeBridge({debugger:{sendCommand:(_source,method,params)=>page(method,params)}});
  bridge.attached.add(41);bridge.contexts.set(41,executionContextId);
  const context=new ActionContext({_bridge_deadline_unix_ms:Date.now()+1000},()=>true);
  const started=performance.now();await bridge.settle(context,41);
  assert(performance.now()-started<800,'settling must finish before the request deadline without page timers');
  const stillFrozen=await page('Runtime.evaluate',{contextId:executionContextId,expression:'globalThis.timerRan',returnByValue:true});
  assert.equal(stillFrozen.result.value,false,'settling must not activate or unfreeze the tab');
  await page('Page.setWebLifecycleState',{state:'active'});
  console.log(JSON.stringify({frozenPageTimer:false,settlingCompleted:true,elapsedMs:performance.now()-started,profile:'isolated headless'}));
 }finally{
  if(socket?.readyState===WebSocket.OPEN)try{await command('Browser.close');}catch{}
  await Promise.race([exited,delay(3000)]);
  if(child.exitCode===null){child.kill();await exited;}
  socket?.close();for(const wait of pending.values()){clearTimeout(wait.timer);wait.reject(new Error('Owned test browser closed'));}pending.clear();
  assert.equal(dirname(profile),parent);rmSync(profile,{recursive:true,force:true,maxRetries:5,retryDelay:100});
 }
});
