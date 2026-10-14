// Opt-in, bounded comparison through the actual MCP wire, in an isolated root.
// Never reads a deployment .env, uses a logged-in profile, or changes production.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdir,readFile,writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import path from 'node:path';
import net from 'node:net';
import {Client} from '@modelcontextprotocol/sdk/client/index.js';
import {StreamableHTTPClientTransport} from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import {assertNativeContract} from './native-contract.mjs';
assert.equal(process.env.DEVBOX_BROWSER_LIVE,'1','Explicit live network opt-in required');
const [binaryArg,corpusArg,evidenceArg]=process.argv.slice(2);
assert(binaryArg&&corpusArg&&evidenceArg,'Pass exact binary, fixed corpus JSON and NEW evidence directory');
const binary=path.resolve(binaryArg),root=path.resolve(evidenceArg);
const corpus=JSON.parse(await readFile(corpusArg,'utf8'));
assert(Array.isArray(corpus)&&corpus.length>=1&&corpus.length<=20);
for(const item of corpus){assert.match(item.id,/^[a-z0-9-]+$/);assert(['http:','https:'].includes(new URL(item.url).protocol));}
assert.equal(new Set(corpus.map(x=>x.id)).size,corpus.length);
await mkdir(root);await mkdir(path.join(root,'workspace'));
const hash=createHash('sha256').update(await readFile(binary)).digest('hex');
const clean={};for(const [k,v]of Object.entries(process.env))if(['PATH','SYSTEMROOT','WINDIR','SYSTEMDRIVE','TEMP','TMP','TMPDIR','HOME','USERPROFILE','LANG','LOCALAPPDATA','APPDATA','PATHEXT','PROGRAMFILES','PROGRAMFILES(X86)'].includes(k.toUpperCase()))clean[k]=v;
const port=await new Promise((resolve,reject)=>{const s=net.createServer();s.once('error',reject);s.listen(0,'127.0.0.1',()=>{const p=s.address().port;s.close(()=>resolve(p));});});
const env={...clean,DEVBOX_PROJECT_ROOT:root,HOST:'127.0.0.1',PORT:String(port),MCP_AUTH_MODE:'none',PUBLIC_BASE_URL:'',
 DEVBOX_RUNTIME_MODE:'host',ENABLE_HOST_EXEC:'true',DEVBOX_AUTO_START:'false',MCP_STATE_BACKEND:'legacy',
 DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE:'1',HOST_WORKSPACE_PATH:path.join(root,'workspace'),HOST_DEFAULT_WORKDIR:path.join(root,'workspace'),
 DEVBOX_WORKSPACE_PATH:path.join(root,'workspace'),MCP_JOBS_ROOT:path.join(root,'jobs'),MCP_EXEC_SLOT_ROOT:path.join(root,'slots'),
 MCP_PERFORMANCE_STATE_PATH:path.join(root,'run','mcp-performance.json')};
const child=spawn(binary,[],{cwd:root,env,windowsHide:true,stdio:['ignore','pipe','pipe']});
let log='';for(const s of[child.stdout,child.stderr])s.on('data',b=>{log=(log+b.toString()).slice(-12000);});
const exited=new Promise((resolve,reject)=>{child.once('exit',(code,signal)=>resolve({code,signal}));child.once('error',reject);});
const report={started_at:new Date().toISOString(),pid:child.pid,binarySha256:hash,corpus,results:[],environment:'local private canary; not Oracle',warnings:['Usable content is not verified selected-variant price accuracy.','Fixed denominator includes all failures. No replacement URLs or paid proxies.']};
await writeFile(path.join(root,'owner.json'),JSON.stringify({pid:child.pid,executable:binary,started_at:report.started_at,binarySha256:hash,port},null,2));
let client,session;
const pause=ms=>new Promise(r=>setTimeout(r,ms));
const unpack=r=>r.structuredContent?.data;
try{
 const start=Date.now()+20000;
 for(;;){assert(Date.now()<start,log);assert.equal(child.exitCode,null,log);try{const r=await fetch(`http://127.0.0.1:${port}`,{signal:AbortSignal.timeout(500)});if(r.ok){assert.equal((await r.json()).build.binarySha256,hash);break;}}catch(e){if(e.code==='ERR_ASSERTION')throw e;}await pause(100);}
 client=new Client({name:'native-browser-comparison',version:'1'},{capabilities:{}});
 await client.connect(new StreamableHTTPClientTransport(new URL(`http://127.0.0.1:${port}/mcp`)));
 const call=(name,args)=>client.callTool({name,arguments:args},undefined,{timeout:50000});
 report.capabilities=unpack(await call('devbox_capabilities',{}));assertNativeContract((await client.listTools()).tools,report.capabilities);
 report.browser_status=unpack(await call('devbox_browser',{action:'status'}));
 assert.equal(report.browser_status.available,true);
 for(const item of corpus){
  const row={id:item.id,url:item.url};
  for(const mode of['http','browser']){
   const began=performance.now();
   try{
    const result=mode==='http'?await call('devbox_web_fetch',{urls:[item.url],max_age_seconds:0,view:'offers',max_chars:12000,excerpt_chars:4000}):await call('devbox_browser',{action:'open',url:item.url,include_html:true});
    let data=unpack(result);session=mode==='browser'?data?.session_id:session;
    row[mode]={ok:!result.isError,elapsed_ms:Math.round(performance.now()-began),output_bytes:Buffer.byteLength(JSON.stringify(result)),data,summary:result.structuredContent?.summary};
    if(mode==='browser'&&session){
     const img=result.content.find(c=>c.type==='image');if(img)await writeFile(path.join(root,item.id+'.jpg'),Buffer.from(img.data,'base64'));
     row.browser.screenshot=!!img;row.browser.readable_characters=data.text?.length??0;
     row.browser.content_candidate=data.page_status==='content_available';
     row.browser.page_status=data.page_status;
     row.browser.price_verified=false;
     if(data.html_artifact){const part=await call('devbox_browser',{action:'read_html',session_id:session,html_sha256:data.html_artifact.sha256,max_bytes:64000});await writeFile(path.join(root,item.id+'.html.txt'),unpack(part)?.text??'');}
     const scrolled=await call('devbox_browser',{action:'scroll',session_id:session,observation_id:data.observation_id,x:1100,y:600,delta_y:600,screenshot:false});
     row.browser.scroll={ok:!scrolled.isError,before:data.viewport,after:unpack(scrolled)?.viewport,movement_observed:unpack(scrolled)?.scroll_result?.movement_observed,error:scrolled.isError?scrolled.structuredContent?.summary:null};
     if(data.page_status==='consent_required'&&!scrolled.isError){
      const fresh=unpack(scrolled),reject=fresh.controls?.find(x=>x.name==='Reject all'&&x.tag==='button');
      if(reject){
       const consent=await call('devbox_browser',{action:'click',session_id:session,observation_id:fresh.observation_id,x:reject.x+reject.width/2,y:reject.y+reject.height/2,include_html:true});
       row.browser.consent={action:'Reject all',ok:!consent.isError,data:unpack(consent),error:consent.isError?consent.structuredContent?.summary:null};
       const consentImage=consent.content.find(c=>c.type==='image');if(consentImage)await writeFile(path.join(root,item.id+'-after-consent.jpg'),Buffer.from(consentImage.data,'base64'));
      }
     }
    }
   }catch(e){row[mode]={ok:false,elapsed_ms:Math.round(performance.now()-began),error:String(e)};}
   finally{if(mode==='browser'&&session){const closed=await call('devbox_browser',{action:'close',session_id:session});assert.equal(closed.isError,false,JSON.stringify(closed));session=undefined;}}
  }
  report.results.push(row);await writeFile(path.join(root,'result.json'),JSON.stringify(report,null,2));
  console.log(JSON.stringify({id:row.id,http_ok:row.http.ok,browser_ok:row.browser.ok,browser_ms:row.browser.elapsed_ms,content_candidate:row.browser.content_candidate,error:row.browser.error??(row.browser.ok?null:row.browser.summary)}));
 }
 report.completed_at=new Date().toISOString();report.completed=true;
}finally{
 await client?.close().catch(()=>{});
 if(child.exitCode===null&&child.signalCode===null){child.kill();await Promise.race([exited,pause(10000)]);}
 report.serverStopped=child.exitCode!==null||child.signalCode!==null;
 await writeFile(path.join(root,'result.json'),JSON.stringify(report,null,2));
 assert(report.serverStopped,'Owned server must stop');
}
