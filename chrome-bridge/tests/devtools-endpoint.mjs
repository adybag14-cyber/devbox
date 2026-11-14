import {readFileSync} from 'node:fs';
import {performance} from 'node:perf_hooks';
import {setTimeout as pause} from 'node:timers/promises';

// Chrome can create this file before closing its Windows write handle or
// publishing both lines. Existence alone is not an endpoint-ready signal.
export async function waitForDevToolsEndpoint(file,{timeoutMs=12000,now=()=>performance.now(),
 alive=()=>true,read=path=>readFileSync(path,'utf8'),sleep=pause}={}){
 const deadline=now()+timeoutMs;let lastError;
 while(now()<deadline){
  if(!alive())throw new Error('Chrome exited before publishing its endpoint');
  try{
   const value=read(file);
   if(value.length>512)throw new Error('Chrome endpoint descriptor exceeds its bound');
   const match=/^(\d{1,5})\r?\n(\/devtools\/browser\/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\r?\n?$/i.exec(value);
   if(match&&Number(match[1])>0&&Number(match[1])<=65535)return {port:match[1],path:match[2]};
  }catch(error){
   if(!['ENOENT','EBUSY'].includes(error.code))throw error;
   lastError=error;
  }
  const remaining=deadline-now();if(remaining>0)await sleep(Math.min(25,remaining));
 }
 throw new Error('Chrome startup endpoint deadline',{cause:lastError});
}
