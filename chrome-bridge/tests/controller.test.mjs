import test from 'node:test';import assert from 'node:assert/strict';
import {ChromeBridge} from '../extension/controller.mjs';
const event=()=>({listeners:[],addListener(fn){this.listeners.push(fn);},emit(...args){for(const fn of this.listeners)fn(...args);}});
function fixture(){
  const state={tabs:new Map([[41,{id:41,url:'https://example.test/page',title:'Fixture',active:true,windowId:1,status:'complete'}]]),clicks:0,inputs:[],failCapture:false,hideUrl:false};
  const port={onDisconnect:event(),onMessage:event(),postMessage(){}};
  const api={runtime:{id:'fixture',onStartup:event(),onInstalled:event(),onMessage:event(),connectNative:()=>port,getManifest:()=>({version:'test'}),getURL:path=>'chrome-extension://fixture/'+path},
    tabs:{onUpdated:event(),onRemoved:event(),query:async()=>[state.tabs.get(41)],get:async id=>{const value=state.tabs.get(id);if(!value)throw new Error('No tab');const copy={...value};if(state.hideUrl)delete copy.url;return copy;},
      create:async options=>{const tab={id:42,url:options.url,title:'Created',windowId:1,status:'complete'};state.tabs.set(42,tab);return {...tab};},
      remove:async id=>{state.tabs.delete(id);api.tabs.onRemoved.emit(id);},update:async()=>{},reload:async()=>{},goBack:async()=>{},goForward:async()=>{}},
    windows:{update:async()=>{}},debugger:{onDetach:event(),attach:async()=>{},detach:async source=>api.debugger.onDetach.emit(source),sendCommand:async(source,method,params)=>{
      if(method==='Page.getFrameTree')return {frameTree:{frame:{id:'frame-'+source.tabId,url:state.tabs.get(source.tabId).url}}};
      if(method==='Page.createIsolatedWorld')return {executionContextId:source.tabId};
      if(method==='Page.captureScreenshot'){if(state.failCapture)throw new Error('capture failed');return {data:'aW1hZ2U='};}
      if(method==='Input.dispatchMouseEvent'){state.inputs.push(params);if(params.type==='mousePressed')state.clicks++;return {};}
      if(method==='Runtime.evaluate'){
        if(params.expression.includes('"action":"inspect"'))return {result:{value:{url:state.tabs.get(source.tabId).url,title:'Fixture',text:'counter '+state.clicks,elements:[],viewport:{width:800,height:600},scroll:{x:0,y:0},html_base64:btoa('<html>fixture</html>'),html:{truncated:false,capture_kind:'bounded_dom_serialization'}}}};
        if(params.expression.includes('"action":"point"'))return {result:{value:{x:10,y:20}}};
        return {result:{value:true}};
      }
      return {};
    }}};
  const bridge=new ChromeBridge(api);bridge.install();port.onMessage.emit({control:'ready',protocol:2,version:'test'});
  const call=args=>bridge.handle({...args,_bridge_deadline_unix_ms:Date.now()+10000});
  return {api,state,bridge,call,port};
}
async function allowed(f){await f.bridge.popup({action:'grant',allow_new_tabs:true});const result=await f.call({action:'observe',tab_id:41,screenshot:false});assert(result.ok);return result.data.observation_id;}
test('page messages cannot grant access; only the exact packaged popup sender can',async()=>{
 const f=fixture(),listener=f.api.runtime.onMessage.listeners[0];let replies=0;
 assert.equal(listener({action:'grant'},{id:'fixture',url:'https://example.test/'},()=>replies++),undefined);
 assert.equal(f.bridge.state.grants.size,0);assert.equal(replies,0);
 await new Promise(resolve=>listener({action:'grant'},{id:'fixture',url:'chrome-extension://fixture/popup.html'},value=>{assert(!value.error);resolve();}));
 assert.equal(f.bridge.state.grants.size,1);
});
test('identical input retries recover receipts and conflicting IDs never execute',async()=>{
 const f=fixture(),observation_id=await allowed(f);
 const request={action:'click',tab_id:41,observation_id,request_id:'one',x:10,y:20,screenshot:false};
 const first=await f.call(request);assert(first.ok);assert.equal(f.state.clicks,1);
 const retry=await f.call({...request,id:'different-native-id'});assert(retry.ok&&retry.replayed);assert.equal(f.state.clicks,1);
 const conflict=await f.call({...request,x:99});assert.equal(conflict.error.code,'REQUEST_ID_CONFLICT');assert.equal(f.state.clicks,1);
 const stale=await f.call({...request,request_id:'two'});assert.equal(stale.error.code,'OBSERVATION_STALE');assert.equal(stale.input_sent,false);assert.equal(f.state.clicks,1);
});
test('revocation prevents cached page data from being disclosed again',async()=>{
 const f=fixture(),observation_id=await allowed(f),request={action:'click',tab_id:41,observation_id,request_id:'one',x:10,y:20,screenshot:false};
 assert((await f.call(request)).ok);await f.bridge.popup({action:'revoke_all'});
 const result=await f.call(request);assert(result.replayed);assert(result.data.details_expired);assert(!JSON.stringify(result).includes('counter'));assert.equal(f.state.clicks,1);
});
test('a failed observation invalidates old node references before any later input',async()=>{
 const f=fixture(),observation_id=await allowed(f);f.state.failCapture=true;
 const failed=await f.call({action:'observe',tab_id:41});assert(!failed.ok);assert.equal(failed.input_sent,false);
 const click=await f.call({action:'click',tab_id:41,observation_id,request_id:'one',x:10,y:20});assert.equal(click.error.code,'OBSERVATION_STALE');assert.equal(f.state.clicks,0);
});
test('agent-created same-origin tabs work without tabs URL permission',async()=>{
 const f=fixture(),observation_id=await allowed(f);
 const created=await f.call({action:'open',tab_id:41,observation_id,request_id:'open-one',url:'https://example.test/next'});assert(created.ok);
 f.state.hideUrl=true;const observed=await f.call({action:'observe',tab_id:42,screenshot:false});assert(observed.ok);assert.equal(observed.data.url,'https://example.test/next');
});
test('acknowledged tab closure remains successful when onRemoved revokes its grant',async()=>{
 const f=fixture(),observation_id=await allowed(f);const request={action:'close',tab_id:41,observation_id,request_id:'close-one'};
 const result=await f.call(request);assert(result.ok);assert(result.data.closed);assert.equal(f.bridge.state.grants.size,0);
 const retry=await f.call(request);assert(retry.replayed);assert(retry.ok);
});
test('old native-port disconnect events cannot disconnect a newer connection',()=>{
 const f=fixture();f.bridge.port=null;const replacement={onDisconnect:event(),onMessage:event(),postMessage(){}};f.api.runtime.connectNative=()=>replacement;
 f.bridge.connect();f.port.onDisconnect.emit();assert.equal(f.bridge.port,replacement);
});
test('queued or expired commands do not dispatch input',async()=>{
 const f=fixture(),observation_id=await allowed(f);
 const result=await f.bridge.handle({action:'click',tab_id:41,observation_id,request_id:'expired',x:10,y:20,_bridge_deadline_unix_ms:Date.now()-1});
 assert.equal(result.error.code,'REQUEST_EXPIRED');assert.equal(result.input_sent,false);assert.equal(f.state.clicks,0);
});
test('native client cancellation interrupts a pending browser read without later input',async()=>{
 const f=fixture(),observation_id=await allowed(f);let entered;
 const started=new Promise(resolve=>entered=resolve),original=f.api.debugger.sendCommand;
 f.api.debugger.sendCommand=async(source,method,params)=>{if(method==='Page.getFrameTree'){entered();return new Promise(()=>{});}return original(source,method,params);};
 const id='a'.repeat(32);
 const pending=f.bridge.handle({action:'click',tab_id:41,observation_id,request_id:'cancel-one',x:10,y:20,id,_bridge_deadline_unix_ms:Date.now()+10000});
 await started;f.port.onMessage.emit({control:'cancel',id});
 const result=await pending;assert.equal(result.error.code,'REQUEST_CANCELLED');assert.equal(result.input_sent,false);assert.equal(f.state.clicks,0);
 assert.equal(f.bridge.activeRequests.size,0);
});
test('revocation while a screenshot is pending does not disclose the late image',async()=>{
 const f=fixture();await allowed(f);let entered;
 const started=new Promise(resolve=>entered=resolve),original=f.api.debugger.sendCommand;
 f.api.debugger.sendCommand=async(source,method,params)=>{if(method==='Page.captureScreenshot'){entered();return new Promise(()=>{});}return original(source,method,params);};
 const pending=f.call({action:'observe',tab_id:41});await started;await f.bridge.popup({action:'revoke_all'});
 const result=await pending;assert(!result.ok);assert(!result.image);assert(!result.data);assert.equal(f.bridge.state.observations.size,0);
});

test('a dispatched release with a lost response is not sent twice',async()=>{
 for(const action of ['click','key']){
  const f=fixture(),observation_id=await allowed(f),original=f.api.debugger.sendCommand;let releases=0;
  f.api.debugger.sendCommand=async(source,method,params)=>{if(params.type==='mouseReleased'||params.type==='keyUp'){++releases;throw new Error('response lost');}return original(source,method,params);};
  const result=await f.call({action,tab_id:41,observation_id,request_id:'lost-release',x:10,y:20,key:'Enter',screenshot:false});
  assert(!result.ok);assert(result.input_sent);assert.equal(result.outcome,'unknown');assert.equal(releases,1);
 }
});

test('an attempted press with a lost response gets one bounded release cleanup',async()=>{
 const f=fixture(),observation_id=await allowed(f),original=f.api.debugger.sendCommand;let presses=0,releases=0;
 f.api.debugger.sendCommand=async(source,method,params)=>{if(params.type==='mousePressed'){++presses;throw new Error('response lost');}if(params.type==='mouseReleased')++releases;return original(source,method,params);};
 const result=await f.call({action:'click',tab_id:41,observation_id,request_id:'lost-press',x:10,y:20,screenshot:false});
 assert(!result.ok);assert(result.input_sent);assert.equal(presses,1);assert.equal(releases,1);
});

test('page failures expose the stable code instead of the JavaScript stack prefix',async()=>{
 const f=fixture(),observation_id=await allowed(f),original=f.api.debugger.sendCommand;
 f.api.debugger.sendCommand=async(source,method,params)=>method==='Runtime.evaluate'&&params.expression.includes('"action":"point"')?{exceptionDetails:{exception:{description:'Error: TARGET_OBSCURED\n at pageTask'}}}:original(source,method,params);
 const result=await f.call({action:'click',tab_id:41,observation_id,request_id:'blocked',x:10,y:20,screenshot:false});
 assert.equal(result.error.code,'TARGET_OBSCURED');assert.equal(result.input_sent,false);assert.equal(f.state.clicks,0);
});

test('maximum HTML ranges encode exact bytes without a large browser argument list',async()=>{
 const f=fixture(),observation_id=await allowed(f),saved=f.bridge.state.observations.get(41),bytes=Uint8Array.from({length:130000},(_,i)=>i%251);
 f.bridge.state.rememberObservation(41,{...saved,html:bytes});
 const original=String.fromCharCode;String.fromCharCode=(...values)=>{assert(values.length<=32768,'browser argument stack exceeded');return original(...values);};
 try{for(const offset of [0,1,16382,63999,129999]){const result=await f.call({action:'read_html',tab_id:41,observation_id,offset,max_bytes:64000});assert(result.ok);assert.deepEqual(Buffer.from(result.data.content_base64,'base64'),Buffer.from(bytes.slice(offset,offset+64000)));}}
 finally{String.fromCharCode=original;}
});
