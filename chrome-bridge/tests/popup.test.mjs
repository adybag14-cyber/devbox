import test from 'node:test';
import assert from 'node:assert/strict';

async function popup(t,tab){
 const elements=new Map(['status','tab-context','grant','revoke-current','revoke','connect','duration','new-tabs'].map(id=>[id,{textContent:'',value:id==='duration'?'until_revoked':'',checked:false,disabled:false,handlers:new Map(),addEventListener(event,fn){this.handlers.set(event,fn);}}]));
 const previous={chrome:globalThis.chrome,document:globalThis.document},messages=[];let queries=0;
 globalThis.document={querySelector:selector=>elements.get(selector.slice(1))};
 globalThis.chrome={tabs:{query:async()=>{queries++;return tab?[tab]:[];}},runtime:{sendMessage:async message=>{messages.push(message);return {connected:true};}}};
 t.after(()=>{for(const name of ['chrome','document']){if(previous[name]===undefined)delete globalThis[name];else globalThis[name]=previous[name];}});
 const url=new URL('../extension/popup.mjs',import.meta.url);url.searchParams.set('fixture',t.name);
 await import(url.href);
 return {elements,messages,queries:()=>queries,click:id=>elements.get(id).handlers.get('click')()};
}
test('the real popup module captures its displayed target once for both grant and revoke',async t=>{
 const p=await popup(t,{id:41,title:'Selected page',url:'https://example.test/first'});
 globalThis.chrome.tabs.query=async()=>{throw new Error('Active tab changed; popup must not retarget');};
 await p.click('grant');await p.click('revoke-current');
 assert.equal(p.queries(),1);assert.match(p.elements.get('tab-context').textContent,/Selected page.*https:\/\/example.test/);
 for(const action of ['grant','revoke_current']){const message=p.messages.find(m=>m.action===action);assert.equal(message.tab_id,41);assert.equal(message.tab_origin,'https://example.test');}
 assert.equal(p.messages.find(m=>m.action==='grant').duration,'until_revoked');
});
test('the popup cannot grant a missing or restricted page but keeps revoke-all available',async t=>{
 const p=await popup(t,{id:41,title:'Extensions',url:'chrome://extensions/'});
 assert.equal(p.elements.get('grant').disabled,true);assert.equal(p.elements.get('revoke-current').disabled,true);
 await p.click('revoke');assert(p.messages.some(m=>m.action==='revoke_all'));
 assert(!p.messages.some(m=>m.action==='grant'));
});
