import test from 'node:test';import assert from 'node:assert/strict';
import {waitForDevToolsEndpoint} from './devtools-endpoint.mjs';
const endpoint='/devtools/browser/01234567-89ab-4cde-8012-3456789abcde';
const error=code=>Object.assign(new Error(code),{code});

test('endpoint discovery retries sharing locks and partial records before accepting readiness',async()=>{
 let clock=0,calls=0;const values=[error('ENOENT'),error('EBUSY'),'9222\n','9222\n/devtools/browser/',`9222\n${endpoint}`];
 const result=await waitForDevToolsEndpoint('owned-fixture',{now:()=>clock,sleep:async ms=>{clock+=ms;},read:()=>{const next=values[calls++];if(next instanceof Error)throw next;return next;}});
 assert.deepEqual(result,{port:'9222',path:endpoint});assert.equal(calls,5);assert.equal(clock,100);
});
test('persistent endpoint locks stop at the deadline and retain their cause',async()=>{
 let clock=0;const busy=error('EBUSY');
 await assert.rejects(waitForDevToolsEndpoint('owned-fixture',{timeoutMs:10,now:()=>clock,sleep:async ms=>{clock+=ms;},read:()=>{throw busy;}}),e=>e.cause===busy&&/deadline/.test(e.message));
 assert.equal(clock,10);
});
test('nontransient endpoint read errors are not hidden by retries',async()=>{
 const denied=error('EACCES');let calls=0;
 await assert.rejects(waitForDevToolsEndpoint('owned-fixture',{read:()=>{calls++;throw denied;}}),e=>e===denied);assert.equal(calls,1);
});
test('an exited owned browser is rejected before reading a stale endpoint',async()=>{
 await assert.rejects(waitForDevToolsEndpoint('owned-fixture',{alive:()=>false,read:()=>{throw new Error('must not read');}}),/exited before/);
});
test('malformed and oversized endpoint descriptors never become connection targets',async()=>{
 for(const value of [`0\n${endpoint}`,`65536\n${endpoint}`,`9222\n${endpoint}\nextra`]){
  let clock=0;await assert.rejects(waitForDevToolsEndpoint('owned-fixture',{timeoutMs:10,now:()=>clock,sleep:async ms=>{clock+=ms;},read:()=>value}),/deadline/);
 }
 await assert.rejects(waitForDevToolsEndpoint('owned-fixture',{read:()=>'x'.repeat(513)}),/exceeds its bound/);
});
