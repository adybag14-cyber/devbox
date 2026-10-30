import test from 'node:test';import assert from 'node:assert/strict';
import {BudgetMap,BridgeState,ActionContext,LIMITS} from '../extension/state.mjs';
test('retained storage respects byte and count budgets through replacement and eviction',()=>{
 const cache=new BudgetMap(2,100);cache.set('a',1,40);cache.set('b',2,50);cache.set('a',3,80);
 assert.equal(cache.get('b'),undefined);assert.equal(cache.get('a'),3);assert.equal(cache.bytes,80);
 cache.set('c',4,10);cache.set('d',5,10);assert.equal(cache.get('a'),undefined);assert.equal(cache.bytes,20);
 assert.throws(()=>cache.set('huge',0,101),/LIMIT/);assert.throws(()=>cache.set('bad',0,-1),/LIMIT/);
 cache.delete('c');cache.delete('c');assert.equal(cache.bytes,10);cache.clear();assert.equal(cache.bytes,0);
});
test('observations and replay details remain bounded under many tabs and large responses',()=>{
 const state=new BridgeState();
 for(let i=0;i<LIMITS.grants;i++)state.grant(i,{nonce:String(i)});
 assert.throws(()=>state.grant(99,{}),/GRANT_LIMIT/);
 for(let i=0;i<100;i++)state.rememberObservation(i,{html:new Uint8Array(2*1024*1024)});
 assert(state.observations.bytes<=LIMITS.observationBytes);assert(state.observations.size<=LIMITS.observations);
 for(let i=0;i<100;i++)state.rememberReceipt(String(i),{response:{ok:true,data:{text:'x'.repeat(100000)}}});
 assert(state.receipts.bytes<=LIMITS.receiptBytes);assert(state.receipts.size<=LIMITS.receipts);
});
test('expired requests and revocation fail before dispatch, including queued work',async()=>{
 let now=100,valid=true,calls=0;
 const context=new ActionContext({_bridge_deadline_unix_ms:200},()=>valid,{wall:()=>100,mono:()=>now});
 await context.run(()=>{calls++;});assert.equal(calls,1);
 valid=false;await assert.rejects(context.run(()=>{calls++;},true),/REVOKED/);assert.equal(calls,1);
 valid=true;now=201;await assert.rejects(context.run(()=>{calls++;},true),/EXPIRED/);assert.equal(calls,1);
 assert.throws(()=>new ActionContext({},()=>true),/DEADLINE_REQUIRED/);
});
test('a late command result cannot authorize a later input',async()=>{
 let now=0,calls=0;
 const context=new ActionContext({_bridge_deadline_unix_ms:100},()=>true,{wall:()=>0,mono:()=>now});
 await assert.rejects(context.run(async()=>{now=101;return 'late';}),/EXPIRED/);
 await assert.rejects(context.run(()=>calls++,true),/EXPIRED/);assert.equal(calls,0);
});
test('request duration is capped even if a sender supplies a far future deadline',()=>{
 const context=new ActionContext({_bridge_deadline_unix_ms:Number.MAX_SAFE_INTEGER},()=>true,{wall:()=>0,mono:()=>50});
 assert.equal(context.end,50+LIMITS.actionMs);
});
test('browser timer functions retain the global receiver rather than the action context',async()=>{
 let cleared=false;
 const context=new ActionContext({_bridge_deadline_unix_ms:100},()=>true,{wall:()=>0,mono:()=>0,
   timer:function(){assert.equal(this,globalThis);return 7;},
   clearTimer:function(id){assert.equal(this,globalThis);assert.equal(id,7);cleared=true;}});
 assert.equal(await context.run(()=>42),42);assert(cleared);
});
