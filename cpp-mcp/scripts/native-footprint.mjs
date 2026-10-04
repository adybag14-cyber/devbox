import assert from 'node:assert/strict';
import {mkdtemp,mkdir,readFile,rm,stat,writeFile} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
assert.equal(process.platform,'linux','Linux /proc measurement only');
const binary=path.resolve(process.env.DEVBOX_CPP_BINARY||process.argv[2]||'');
const fixture=await mkdtemp(path.join(os.tmpdir(),'devbox-native-footprint-'));
const root=path.join(fixture,'core'),empty=path.join(fixture,'empty-path');await mkdir(empty);
const env={PATH:empty,HOME:fixture,LANG:'C.UTF-8'};
const command=async(action,...args)=>JSON.parse((await runCheckedProcess(binary,['manage',action,'--root',root,...args],
  {env,cwd:fixture,timeoutMs:120000,label:'Native core footprint fixture'})).stdout);
const sample=async pid=>{
  const status=await readFile(`/proc/${pid}/status`,'utf8');
  const record=await readFile(`/proc/${pid}/stat`,'utf8');const fields=record.slice(record.lastIndexOf(')')+2).split(' ');
  let pssKiB=null;try{pssKiB=Number((await readFile(`/proc/${pid}/smaps_rollup`,'utf8')).match(/^Pss:\s+(\d+)/mu)[1]);}catch{}
  return{pid,startTicks:fields[19],cpuTicks:Number(fields[11])+Number(fields[12]),
    rssKiB:Number(status.match(/^VmRSS:\s+(\d+)/mu)[1]),pssKiB};
};
let initialized=false;
try {
  const port=await new Promise(resolve=>{const server=net.createServer();server.listen(0,'127.0.0.1',()=>{const value=server.address().port;server.close(()=>resolve(value));});});
  await command('init','--allow-local-build','--port',String(port));initialized=true;
  const began=performance.now();const started=await command('start');const startupMs=performance.now()-began;
  const coordinator=JSON.parse(await readFile(path.join(root,'run/state/coordinator.json'),'utf8'));
  const before={supervisor:await sample(started.owner.pid),frontend:await sample(started.child.pid),coordinator:await sample(coordinator.pid)};
  const sampleAt=performance.now();await new Promise(r=>setTimeout(r,5000));
  const durationMs=performance.now()-sampleAt;
  const after={supervisor:await sample(started.owner.pid),frontend:await sample(started.child.pid),coordinator:await sample(coordinator.pid)};
  for(const name of Object.keys(before)){assert.equal(after[name].startTicks,before[name].startTicks);after[name].idleCpuTicks=after[name].cpuTicks-before[name].cpuTicks;}
  const result={ok:true,source:(await (await fetch(`http://127.0.0.1:${port}/`)).json()).build,
    binaryBytes:(await stat(binary)).size,startupMs,idleSampleMs:durationMs,processes:after,
    scope:'supervisor, frontend and SQLite coordinator; filesystem page cache outside these processes excluded',
    method:'Shared development host smoke measurement; PSS accounts for shared pages when readable; CPU ticks retain kernel resolution'};
  if(process.env.DEVBOX_FOOTPRINT_OUTPUT)await writeFile(process.env.DEVBOX_FOOTPRINT_OUTPUT,JSON.stringify(result,null,2));
  console.log(JSON.stringify(result));
} finally {
  if(initialized){const status=await command('status');if(status.running)await command('stop');
    for(let i=0;i<100;i++){if(!(await command('status')).running)break;await new Promise(r=>setTimeout(r,50));}
    assert.equal((await command('status')).running,false);}
  await rm(fixture,{recursive:true,force:true});
}
