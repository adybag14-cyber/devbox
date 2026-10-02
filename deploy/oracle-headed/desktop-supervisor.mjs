import {spawn} from 'node:child_process';
import {mkdir,readFile,writeFile,rename,chmod,open} from 'node:fs/promises';
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
    browser:'headed Chromium',backend:'native C++23 XCB/XTEST',
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
  '--user-data-dir=/workspace/.home/chromium','about:blank'];
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
   await child('chromium','chromium',chromiumArgs);
   await status(true);
   while(!stopping){
    for(const name of ['xvfb','dbus','openbox']){
     const s=children.get(name);if(s.p.exitCode!==null||s.p.signalCode!==null)throw Error(`${name} exited`);
    }
    const browser=children.get('chromium');
    if(browser.p.exitCode!==null||browser.p.signalCode!==null){await stop(browser);await pause(1500);await child('chromium','chromium',chromiumArgs);await status(true);}
    await pause(1000);
   }
  }catch(error){await status(false,String(error.message));}
  for(const name of ['chromium','openbox','dbus','xvfb'])await stop(children.get(name));
  if(!stopping)await pause(3000);
 }
}finally{for(const s of [...children.values()].reverse())await stop(s);await status(false,'Desktop supervisor stopped');}
