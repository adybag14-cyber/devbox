import {spawn} from 'node:child_process';
import {mkdir,readFile,writeFile,rename,chmod,open,unlink,lstat} from 'node:fs/promises';
import {randomBytes} from 'node:crypto';
import path from 'node:path';

const root='/tmp/devbox-desktop';
const logs='/opt/devbox/run/desktop';
const env={...process.env,DISPLAY:':99',XAUTHORITY:path.join(root,'authority'),
  XDG_RUNTIME_DIR:path.join(root,'runtime'),DBUS_SESSION_BUS_ADDRESS:`unix:path=${root}/runtime/bus`};
delete env.SESSION_MANAGER;delete env.WAYLAND_DISPLAY;
await mkdir(logs,{recursive:true,mode:0o700});
await mkdir(env.XDG_RUNTIME_DIR,{recursive:true,mode:0o700});
await chmod(env.XDG_RUNTIME_DIR,0o700);
const children=new Map();let stopping=false;
// Native C++ owns either an on-demand process or isolated contexts in this daemon.
const sharedBrowser=process.env.DEVBOX_BROWSER_SHARED==='1';
const browserProfile=sharedBrowser?path.join(root,'browser'):'/workspace/.home/chromium';
const browserAutostart=sharedBrowser||process.env.DEVBOX_DESKTOP_BROWSER_AUTOSTART!=='0';
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function birth(pid){try{const s=await readFile(`/proc/${pid}/stat`,'utf8');return s.slice(s.lastIndexOf(')')+2).split(' ')[19];}catch{return null;}}
async function child(name,file,args,{pipe=false}={}){
  const log=await open(path.join(logs,`${name}.log`),'w',0o600);
  const p=spawn(file,args,{env,cwd:'/workspace',stdio:[pipe?'pipe':'ignore',log.fd,log.fd]});
  const closed=new Promise((resolve,reject)=>{p.once('exit',(code,signal)=>resolve({code,signal}));p.once('error',reject);});
  // Observe failures even while a different desktop child is being restarted.
  closed.catch(()=>{});
  await new Promise((resolve,reject)=>{p.once('spawn',resolve);p.once('error',reject);});
  await log.close();
  const state={name,p,closed,birth:await birth(p.pid)};children.set(name,state);return state;
}
async function stop(state){
  if(!state)return;
  if(state.p.exitCode===null && state.p.signalCode===null && state.birth && await birth(state.p.pid)===state.birth){
    state.p.kill('SIGTERM');await Promise.race([state.closed,pause(5000)]);
    if(state.p.exitCode===null && state.p.signalCode===null && await birth(state.p.pid)===state.birth){state.p.kill('SIGKILL');await state.closed;}
  }
  children.delete(state.name);
}
async function status(ready,error=''){
  const value={ready,updatedAt:new Date().toISOString(),display:env.DISPLAY,
    browser:browserAutostart?'headed Chromium':'on-demand native browser',backend:'native C++23 XCB/XTEST',
    browser_mode:sharedBrowser?'shared_native':browserAutostart?'desktop':'on_demand',
    browser_profile:browserProfile,
    children:[...children.values()].map(s=>({name:s.name,pid:s.p.pid,startTicks:s.birth,running:s.p.exitCode===null&&s.p.signalCode===null})),error};
  const file=path.join(logs,'status.json');await writeFile(file+'.tmp',JSON.stringify(value,null,2),{mode:0o600});await rename(file+'.tmp',file);
}
async function probe(file,args){
  const p=spawn(file,args,{env,stdio:'ignore'});
  const done=new Promise(resolve=>{p.once('exit',code=>resolve(code===0));p.once('error',()=>resolve(false));});
  const timer=setTimeout(()=>p.kill('SIGTERM'),3000);
  try{return await done;}finally{clearTimeout(timer);}
}
process.on('SIGTERM',()=>{stopping=true;});process.on('SIGINT',()=>{stopping=true;});
const chromiumArgs=['--ozone-platform=x11','--disable-gpu','--no-first-run','--no-default-browser-check',
  '--disable-background-networking','--disable-component-update','--disable-sync',
  '--disable-features=Translate,MediaRouter,OptimizationHints','--renderer-process-limit=2',
  '--js-flags=--max-old-space-size=128','--window-size=1200,740','--window-position=20,20',
  '--user-data-dir='+browserProfile,
  ...(sharedBrowser?['--remote-debugging-address=127.0.0.1','--remote-debugging-port=0',
    '--disable-extensions','--disable-quic','--force-webrtc-ip-handling-policy=disable_non_proxied_udp',
    '--proxy-server=http://127.0.0.1:9','--proxy-bypass-list=<-loopback>',
    '--host-resolver-rules=MAP * ~NOTFOUND, EXCLUDE 127.0.0.1']:[]),'about:blank'];
async function startBrowser(){
 if(sharedBrowser){
  await mkdir(browserProfile,{recursive:true,mode:0o700});
  const profileState=await lstat(browserProfile);
  if(!profileState.isDirectory()||profileState.isSymbolicLink())throw Error('Managed browser profile is not a private directory');
  await chmod(browserProfile,0o700);
  await unlink(path.join(browserProfile,'DevToolsActivePort')).catch(e=>{if(e.code!=='ENOENT')throw e;});
 }
 const s=await child('chromium','chromium',chromiumArgs);
 if(!sharedBrowser)return;
 await status(false,'Managed browser is warming');
 const until=Date.now()+180000;
 while(!stopping){
  if(s.p.exitCode!==null||s.p.signalCode!==null)throw Error('Managed browser exited during startup');
  try{
   const endpoint=(await readFile(path.join(browserProfile,'DevToolsActivePort'),'utf8')).trim().split(/\r?\n/u);
   if(endpoint.length===2&&/^\d+$/u.test(endpoint[0])&&Number(endpoint[0])>0&&Number(endpoint[0])<=65535&&/^\/devtools\/browser\/[a-z0-9-]+$/iu.test(endpoint[1]))return;
  }catch(e){if(e.code!=='ENOENT')throw e;}
  if(Date.now()>until)throw Error('Managed browser startup deadline');
  await pause(200);
 }
}
try{
 while(!stopping){
  try{
   await writeFile(env.XAUTHORITY,'',{mode:0o600});await chmod(env.XAUTHORITY,0o600);
   const auth=await child('xauth','xauth',['-f',env.XAUTHORITY,'source','-'],{pipe:true});
   auth.p.stdin.end(`add ${env.DISPLAY} . ${randomBytes(16).toString('hex')}\n`);
   const exit=await auth.closed;if(exit.code!==0)throw Error('Desktop authentication initialization failed');children.delete('xauth');
   await child('xvfb','Xvfb',[env.DISPLAY,'-screen','0','1280x800x24','-nolisten','tcp','-auth',env.XAUTHORITY,'-noreset']);
   const readyBy=Date.now()+20000;
   while(!await probe('xdpyinfo',[])){if(stopping||Date.now()>readyBy)throw Error('X11 desktop did not become ready');await pause(200);}
   await child('dbus','dbus-daemon',['--session','--nofork',`--address=${env.DBUS_SESSION_BUS_ADDRESS}`]);
   await child('openbox','openbox',['--sm-disable']);
   if(browserAutostart)await startBrowser();
   await status(true);
   while(!stopping){
    for(const name of ['xvfb','dbus','openbox']){
     const s=children.get(name);if(s.p.exitCode!==null||s.p.signalCode!==null)throw Error(`${name} exited`);
    }
    const browser=children.get('chromium');
    if(browser&&(browser.p.exitCode!==null||browser.p.signalCode!==null)){await stop(browser);await pause(1500);await startBrowser();await status(true);}
    await pause(1000);
   }
  }catch(error){await status(false,String(error.message));}
  for(const name of ['chromium','openbox','dbus','xvfb'])await stop(children.get(name));
  if(!stopping)await pause(3000);
 }
}finally{for(const s of [...children.values()].reverse())await stop(s);await status(false,'Desktop supervisor stopped');}
