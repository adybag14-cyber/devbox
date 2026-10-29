import {MAX_HTML, READ_ACTIONS, authorize, boundedInteger, validate, publicUrl} from './policy.mjs';
import {BridgeState, ActionContext, LIMITS} from './state.mjs';
import {pageTask} from './page.mjs';

const failure=(error,context)=>({ok:false,error:{code:String(error.message).split(':')[0],message:String(error.message).slice(0,1000)},
  input_sent:context?.inputSent===true,outcome:context?.inputSent?'unknown':'not_sent',automatic_input_replay_allowed:false});
const random=()=>crypto.randomUUID();
async function fingerprint(request){
  const normalized=Object.fromEntries(Object.entries(request).filter(([key])=>key!=='id'&&!key.startsWith('_bridge_')).sort(([a],[b])=>a.localeCompare(b)));
  return [...new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(JSON.stringify(normalized))))].map(n=>n.toString(16).padStart(2,'0')).join('');
}
function keySpec(value){
  if(typeof value!=='string'||value.length>64)throw new Error('INVALID_KEY');
  const parts=value.split('+'),name=parts.pop();let modifiers=0;
  for(const part of parts){if(part==='Ctrl'||part==='Control')modifiers|=2;else if(part==='Alt')modifiers|=1;else if(part==='Shift')modifiers|=8;else if(part==='Meta')modifiers|=4;else throw new Error('INVALID_KEY_MODIFIER');}
  const codes={Enter:13,Return:13,Tab:9,Escape:27,Backspace:8,Delete:46,ArrowDown:40,ArrowUp:38,ArrowLeft:37,ArrowRight:39,Home:36,End:35,PageDown:34,PageUp:33,Space:32};
  const code=codes[name]??(name.length===1?name.toUpperCase().charCodeAt(0):null);
  if(code===null)throw new Error('UNSUPPORTED_KEY');
  return {key:name==='Return'?'Enter':name==='Space'?' ':name,windowsVirtualKeyCode:code,modifiers};
}

export class ChromeBridge {
  constructor(chrome){this.chrome=chrome;this.state=new BridgeState();this.attached=new Set();this.contexts=new Map();this.activeRequests=new Map();this.cancelledRequests=new Set();this.port=null;this.nativeReady=false;this.nativeVersion=null;this.connectionError='';this.queue=Promise.resolve();this.queued=0;}
  connect(){
    if(this.port)return;
    const port=this.chrome.runtime.connectNative('com.devbox.chrome_bridge');this.port=port;this.connectionError='';
    port.onDisconnect.addListener(()=>{if(this.port===port){this.connectionError=this.chrome.runtime.lastError?.message??'Native host disconnected';this.port=null;this.nativeReady=false;this.state.observations.clear();for(const context of this.activeRequests.values())context.cancel();}});
    port.onMessage.addListener(request=>{
      if(request.control==='ready'){if(this.port===port&&request.protocol===2){this.nativeReady=true;this.nativeVersion=request.version;}return;}
      if(request.control==='cancel'){this.activeRequests.get(request.id)?.cancel();this.cancelledRequests.add(request.id);if(this.cancelledRequests.size>32)this.cancelledRequests.delete(this.cancelledRequests.values().next().value);return;}
      if(this.queued>=LIMITS.queued){port.postMessage({id:request.id,...failure(new Error('BUSY: bridge queue is full'))});return;}
      ++this.queued;
      this.queue=this.queue.then(async()=>{
        const started=performance.now();const response=await this.handle(request,port);
        response.id=request.id;response.elapsed_ms=Math.round((performance.now()-started)*1000)/1000;
        try{port.postMessage(response);}catch{this.connectionError='Native host disconnected before response delivery';}
      }).catch(()=>{this.connectionError='Bridge response dispatch failed';}).finally(()=>--this.queued);
    });
    port.postMessage({control:'hello',protocol:2,version:this.chrome.runtime.getManifest().version});
  }
  install(){
    this.chrome.runtime.onStartup.addListener(()=>this.connect());
    this.chrome.runtime.onInstalled.addListener(()=>this.connect());
    this.chrome.debugger.onDetach.addListener(source=>this.forget(source.tabId));
    this.chrome.tabs.onRemoved.addListener(id=>this.forget(id));
    this.chrome.tabs.onUpdated.addListener((id,change)=>{
      if(change.status==='loading'||change.url){this.contexts.delete(id);this.state.observations.delete(id);}
      if(change.url&&this.state.grants.has(id)){try{if(publicUrl(change.url).origin!==this.state.grants.get(id).origin)void this.revoke(id);}catch{void this.revoke(id);}}
    });
    this.chrome.runtime.onMessage.addListener((message,sender,reply)=>{
      if(sender.id!==this.chrome.runtime.id||sender.url!==this.chrome.runtime.getURL('popup.html'))return;
      this.popup(message).then(reply,error=>reply({error:String(error.message)}));return true;
    });
    this.connect();
  }
  forget(id){this.state.grants.delete(id);this.state.observations.delete(id);this.contexts.delete(id);this.attached.delete(id);for(const context of this.activeRequests.values())if(context.tabId===id&&!context.closing)context.cancel();}
  async revoke(id){const attached=this.attached.has(id);this.forget(id);if(attached)try{await this.chrome.debugger.detach({tabId:id});}catch{}}
  async popup(message){
    if(message.action==='connect')this.connect();
    if(message.action==='grant'){
      const [tab]=await this.chrome.tabs.query({active:true,currentWindow:true});
      if(!tab)throw new Error('NO_ACTIVE_TAB');
      this.state.grant(tab.id,{origin:publicUrl(tab.url).origin,expires:Date.now()+3600000,nonce:random(),allow_new_tabs:message.allow_new_tabs===true});this.connect();
    }
    if(message.action==='revoke_all')for(const id of [...this.state.grants.keys()])await this.revoke(id);
    for(const [id,grant]of this.state.grants)if(Date.now()>=grant.expires)await this.revoke(id);
    return {connected:!!this.port&&this.nativeReady,native_version:this.nativeVersion,error:this.connectionError,grants:[...this.state.grants].map(([tab_id,grant])=>({tab_id,origin:grant.origin,expires:new Date(grant.expires).toISOString(),allow_new_tabs:grant.allow_new_tabs})),limits:LIMITS};
  }
  command(context,id,method,params={},input=false,dispatch=()=>{}){return context.run(()=>{dispatch();return this.chrome.debugger.sendCommand({tabId:id},method,params);},input);}
  async release(id,method,params){
    // Cleanup only releases input this request already pressed. It is bounded
    // even after deadline/revocation and never starts another click or key-down.
    let timer;
    try{await Promise.race([this.chrome.debugger.sendCommand({tabId:id},method,params),new Promise(resolve=>{timer=setTimeout(resolve,250);})]);}catch{}finally{clearTimeout(timer);}
  }
  async attach(context,id){if(!this.attached.has(id)){await context.run(()=>this.chrome.debugger.attach({tabId:id},'1.3'));this.attached.add(id);await this.command(context,id,'Page.enable');}}
  async tabInfo(context,id){
    const tab=await context.run(()=>this.chrome.tabs.get(id));
    // activeTab does not automatically grant tabs.get URL metadata for a tab
    // created by the agent. The debugger permission can inspect frame metadata;
    // body access still waits for the exact explicit origin check below.
    if(!tab.url){await this.attach(context,id);const tree=await this.command(context,id,'Page.getFrameTree');tab.url=tree.frameTree.frame.url;}
    if(tab.url==='about:blank'&&tab.status==='loading')throw new Error('PAGE_LOADING: observe after navigation completes');
    if(typeof tab.url!=='string'||tab.url.length>8192)throw new Error('TAB_URL_LIMIT');
    tab.title=typeof tab.title==='string'?tab.title.slice(0,512):'';
    return tab;
  }
  async world(context,id){
    await this.attach(context,id);if(this.contexts.has(id))return this.contexts.get(id);
    const tree=await this.command(context,id,'Page.getFrameTree');
    if(publicUrl(tree.frameTree.frame.url).origin!==this.state.grants.get(id)?.origin)throw new Error('ORIGIN_CHANGED');
    const value=await this.command(context,id,'Page.createIsolatedWorld',{frameId:tree.frameTree.frame.id,worldName:'devbox-chrome-bridge',grantUniveralAccess:false});
    this.contexts.set(id,value.executionContextId);return value.executionContextId;
  }
  async page(context,id,request,input=false){
    const world=await this.world(context,id);
    const value=await this.command(context,id,'Runtime.evaluate',{contextId:world,expression:`(${pageTask.toString()})(${JSON.stringify(request)})`,returnByValue:true,awaitPromise:true},input);
    if(value.exceptionDetails){const description=value.exceptionDetails.exception?.description??value.exceptionDetails.text;throw new Error(String(description).replace(/^\w*Error:\s*/,'').split('\n')[0]);}
    return value.result.value;
  }
  async ensureOrigin(context,id){
    const tree=await this.command(context,id,'Page.getFrameTree');
    if(publicUrl(tree.frameTree.frame.url).origin!==this.state.grants.get(id)?.origin)throw new Error('ORIGIN_CHANGED');
  }
  async settle(context,id){await this.command(context,id,'Runtime.evaluate',{contextId:await this.world(context,id),expression:'new Promise(resolve=>{let done=false;const finish=()=>{if(!done){done=true;resolve(true)}};requestAnimationFrame(()=>requestAnimationFrame(finish));setTimeout(finish,250)})',returnByValue:true,awaitPromise:true});}
  async capture(context,id){
    const image=await this.command(context,id,'Page.captureScreenshot',{format:'jpeg',quality:65,fromSurface:true,captureBeyondViewport:false});
    if(typeof image.data!=='string'||image.data.length>4*1024*1024)throw new Error('IMAGE_LIMIT');
    return {base64:image.data,mime:'image/jpeg'};
  }
  async observe(context,id,withImage=true){
    // Inspect rebuilds its node-reference map. Invalidate the old observation
    // first so a failed read cannot leave old IDs pointing at different nodes.
    this.state.observations.delete(id);
    const before=await this.tabInfo(context,id);authorize(this.state.grants.get(id),before,{action:'observe'});
    const state=await this.page(context,id,{action:'inspect'});
    const bytes=Uint8Array.from(atob(state.html_base64),c=>c.charCodeAt(0));delete state.html_base64;
    if(bytes.byteLength>MAX_HTML)throw new Error('HTML_LIMIT');
    const digest=[...new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))].map(n=>n.toString(16).padStart(2,'0')).join('');context.check();
    const image=withImage?await this.capture(context,id):undefined;
    const after=await this.tabInfo(context,id);if(after.url!==before.url||state.url!==after.url)throw new Error('NAVIGATION_CHANGED: observe again');
    const observation={id:random(),at:Date.now(),url:after.url,html:bytes,sha256:digest};this.state.rememberObservation(id,observation);
    return {ok:true,data:{...state,tab_id:id,observation_id:observation.id,observed_at:new Date(observation.at).toISOString(),observation_ttl_ms:30000,content_untrusted:true,html:{retained_bytes:bytes.length,retained_sha256:digest,...state.html}},...(image?{image}:{})};
  }
  async handle(request,destination=this.port){
    let context;
    try{
      validate(request);context=new ActionContext(request,()=>this.port===destination&&destination!==null);this.activeRequests.set(request.id??context,context);if(this.cancelledRequests.has(request.id))context.cancel();context.check();
      if(request.action==='status')return {ok:true,data:{connected:!!this.port&&this.nativeReady,version:this.chrome.runtime.getManifest().version,native_version:this.nativeVersion,bridge_protocol:2,...this.state.statistics(),queued:this.queued,per_tab_grants:true,native_fallback:'Use native_chrome only for an authorized desktop task when the bridge is unavailable; never bypass a denied tab grant',native_host_error:this.connectionError}};
      if(!this.nativeReady)throw new Error('BRIDGE_NOT_READY');
      if(request.action==='tabs'){
        const tabs=[],errors=[];for(const [id,grant]of this.state.grants){
          if(Date.now()>=grant.expires){await this.revoke(id);continue;}
          const tabContext=new ActionContext(request,()=>this.port===destination&&this.state.grants.get(id)?.nonce===grant.nonce&&Date.now()<grant.expires);
          try{const tab=await this.tabInfo(tabContext,id);authorize(grant,tab,{action:'tabs'});tabs.push({tab_id:id,title:tab.title,url:tab.url,active:tab.active,window_id:tab.windowId,grant_expires_at:new Date(grant.expires).toISOString()});}
          catch(error){if(String(error.message).startsWith('REQUEST_'))throw error;errors.push({tab_id:id,message:String(error.message).slice(0,1000)});if(/ORIGIN_CHANGED|TAB_NOT_GRANTED|No tab with id/.test(String(error.message))&&this.state.grants.get(id)?.nonce===grant.nonce)await this.revoke(id);}
        }
        return {ok:errors.length===0,data:{tabs,errors,ungranted_tabs_omitted:true},input_sent:false};
      }
      const id=boundedInteger(request.tab_id,undefined,0,2**31-1);if(id===undefined)throw new Error('TAB_ID_REQUIRED');
      context.tabId=id;
      const mutation=!READ_ACTIONS.has(request.action),signature=mutation?await fingerprint(request):null;context.check();
      const receipt=mutation?this.state.receipts.get(request.request_id):null;
      if(receipt){
        if(receipt.signature!==signature)throw new Error('REQUEST_ID_CONFLICT');
        const grant=this.state.grants.get(id);
        if(!grant||grant.nonce!==receipt.grantNonce||Date.now()>=grant.expires)return {ok:receipt.response.ok,replayed:true,data:{details_expired:true,input_sent:receipt.response.data?.input_sent??receipt.response.input_sent??false,next:'Grant and observe before further actions'}};
        return {...structuredClone(receipt.response),replayed:true};
      }
      const grant=this.state.grants.get(id);if(!grant||Date.now()>=grant.expires)throw new Error('TAB_NOT_GRANTED: grant the tab in the extension popup');
      context.valid=()=>this.port===destination&&this.state.grants.get(id)?.nonce===grant.nonce&&Date.now()<grant.expires;
      const tab=await this.tabInfo(context,id);authorize(grant,tab,request,this.state.observations.get(id));
      if(request.action==='observe')return await this.observe(context,id,request.screenshot!==false);
      if(request.action==='screenshot'){await this.attach(context,id);await this.ensureOrigin(context,id);return {ok:true,data:{tab_id:id,url:tab.url,content_untrusted:true},image:await this.capture(context,id)};}
      if(request.action==='read_html'){
        const observation=this.state.observations.get(id);if(!observation||request.observation_id!==observation.id||Date.now()-observation.at>30000)throw new Error('OBSERVATION_STALE');
        const offset=boundedInteger(request.offset,0,0,MAX_HTML),maximum=boundedInteger(request.max_bytes,800,1,64000),chunk=observation.html.slice(offset,offset+maximum);
        // Stay below Chrome's argument-stack limit. Every nonfinal block is a
        // multiple of three so concatenating its base64 adds no inner padding.
        let encoded='';for(let at=0;at<chunk.length;at+=16383)encoded+=btoa(String.fromCharCode(...chunk.subarray(at,at+16383)));
        return {ok:true,data:{tab_id:id,observation_id:observation.id,offset,bytes:chunk.length,retained_bytes:observation.html.length,retained_sha256:observation.sha256,content_base64:encoded,content_untrusted:true,next_offset:offset+chunk.length<observation.html.length?offset+chunk.length:null}};
      }
      this.state.observations.delete(id);
      let response;try{response=await this.mutate(context,id,tab,request,grant);}catch(error){response=failure(error,context);}
      const retained=structuredClone(response);delete retained.image;
      this.state.rememberReceipt(request.request_id,{signature,grantNonce:grant.nonce,response:retained});return response;
    }catch(error){return failure(error,context);}finally{this.activeRequests.delete(request.id??context);this.cancelledRequests.delete(request.id);}
  }
  async mutate(context,id,tab,request,grant){
    const action=request.action;
    if(action==='close'){
      // The removed event revokes the grant, so don't require it after the
      // acknowledged close. No page content is returned from this operation.
      context.closing=true;await context.run(()=>this.chrome.tabs.remove(id),true,false);return {ok:true,data:{closed:true,tab_id:id,input_sent:true}};
    }
    if(action==='open'){
      if(this.state.grants.size>=LIMITS.grants)throw new Error('TAB_GRANT_LIMIT');
      const created=await context.run(()=>this.chrome.tabs.create({url:request.url,active:request.active===true}),true);
      this.state.grant(created.id,{...grant,nonce:random()});return {ok:true,data:{tab_id:created.id,url:request.url,created:true,next:'observe after navigation',input_sent:true}};
    }
    if(['navigate','reload','back','forward'].includes(action)){
      const operation=()=>action==='navigate'?this.chrome.tabs.update(id,{url:request.url}):action==='reload'?this.chrome.tabs.reload(id):action==='back'?this.chrome.tabs.goBack(id):this.chrome.tabs.goForward(id);
      await context.run(operation,true);return {ok:true,data:{tab_id:id,input_sent:true,next:'observe after navigation; cross-origin navigation revokes access'}};
    }
    if(action==='focus'){await context.run(()=>this.chrome.windows.update(tab.windowId,{focused:true}),true);await context.run(()=>this.chrome.tabs.update(id,{active:true}),true);return this.observe(context,id);}
    await this.attach(context,id);await this.ensureOrigin(context,id);
    let point,beforeScroll;
    if(['click','double_click','move','drag','scroll'].includes(action))point=await this.page(context,id,{...request,action:'point'});
    if(action==='scroll'){
      const dx=request.delta_x??0,dy=request.delta_y??650;if(Math.abs(dx)>4000||Math.abs(dy)>4000)throw new Error('SCROLL_LIMIT');
      beforeScroll=await this.page(context,id,{action:'scroll_state',...point});
      await this.command(context,id,'Input.dispatchMouseEvent',{type:'mouseWheel',...point,deltaX:dx,deltaY:dy},true);
    }else if(['click','double_click','move','drag'].includes(action)){
      const button=request.button??'left';if(!['left','right','middle'].includes(button))throw new Error('INVALID_BUTTON');
      const target=action==='drag'?await this.page(context,id,{action:'point',x:request.to_x,y:request.to_y}):null;
      await this.command(context,id,'Input.dispatchMouseEvent',{type:'mouseMoved',...point},true);
      if(action!=='move')for(let n=1;n<=(action==='double_click'?2:1);++n){
        await this.ensureOrigin(context,id);let down=false;
        try{
          await this.command(context,id,'Input.dispatchMouseEvent',{type:'mousePressed',...point,button,clickCount:n},true,()=>{down=true;});
          if(target){const start=point;for(let step=1;step<=8;++step){point={x:start.x+(target.x-start.x)*step/8,y:start.y+(target.y-start.y)*step/8};await this.command(context,id,'Input.dispatchMouseEvent',{type:'mouseMoved',...point,button,buttons:button==='left'?1:button==='right'?2:4},true);}}
          await this.command(context,id,'Input.dispatchMouseEvent',{type:'mouseReleased',...point,button,clickCount:n},true,()=>{down=false;});
        }finally{if(down)await this.release(id,'Input.dispatchMouseEvent',{type:'mouseReleased',...point,button,clickCount:n});}
      }
    }else if(action==='type'){
      if(request.target_id)await this.page(context,id,{...request,action:'focus'},true);
      await this.page(context,id,{action:'editable'});await this.ensureOrigin(context,id);
      await this.command(context,id,'Input.insertText',{text:request.text??''},true);
    }else if(action==='key'){
      const key=keySpec(request.key);let down=false;
      try{await this.command(context,id,'Input.dispatchKeyEvent',{type:'keyDown',...key},true,()=>{down=true;});await this.command(context,id,'Input.dispatchKeyEvent',{type:'keyUp',...key},true,()=>{down=false;});}
      finally{if(down)await this.release(id,'Input.dispatchKeyEvent',{type:'keyUp',...key});}
    }else if(['select_text','select_option'].includes(action)){await this.page(context,id,request,true);}
    else if(action==='wait'){await context.run(()=>new Promise(resolve=>setTimeout(resolve,boundedInteger(request.wait_ms,250,0,1000))));}
    else throw new Error('UNSUPPORTED_ACTION');
    await this.settle(context,id);let movement;
    if(beforeScroll){const after=await this.page(context,id,{action:'scroll_state',...point});movement={before:beforeScroll,after,movement_observed:JSON.stringify(beforeScroll)!==JSON.stringify(after)};}
    const result=await this.observe(context,id,request.screenshot!==false);result.data.input_sent=context.inputSent;if(movement)result.data.movement=movement;return result;
  }
}
