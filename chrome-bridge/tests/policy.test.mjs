import test from 'node:test';import assert from 'node:assert/strict';
import {authorize,validate,publicUrl,boundedInteger,grantActive} from '../extension/policy.mjs';
const tab={url:'https://example.com/page'},grant={origin:'https://example.com',expires:100000,allow_new_tabs:false},observation={id:'fresh',at:1000,url:tab.url};
test('grants are exact-origin, timed by default and not implied by a tab ID',()=>{
  assert.throws(()=>authorize(null,tab,{action:'observe'},null,2000),/TAB_NOT_GRANTED/);
  assert.throws(()=>authorize({...grant,expires:1000},tab,{action:'observe'},null,2000),/TAB_NOT_GRANTED/);
  assert.throws(()=>authorize(grant,{url:'https://example.com.evil.test/'},{action:'observe'},null,2000),/ORIGIN_CHANGED/);
  assert.doesNotThrow(()=>authorize(grant,tab,{action:'observe'},null,2000));
});

test('unlimited duration requires the explicit until-revoked marker and never widens origin',()=>{
 const forever={origin:grant.origin,duration:'until_revoked',expires:null,allow_new_tabs:false};
 assert(grantActive(forever,1000+10*3600000));
 assert.doesNotThrow(()=>authorize(forever,tab,{action:'observe'},null,1000+10*3600000));
 assert.throws(()=>authorize(forever,{url:'https://other.test/'},{action:'observe'},null,1000),/ORIGIN_CHANGED/);
 for(const invalid of [{}, {expires:null}, {duration:'until_revoked'}, {duration:'until_revoked',expires:123}, {duration:'other',expires:100000}])assert.equal(grantActive(invalid,1000),false);
 assert.throws(()=>validate({action:'grant',duration:'until_revoked'}));
});
test('every input requires its exact recent observation and unchanged URL',()=>{
  const r={action:'click',observation_id:'fresh'};
  assert.doesNotThrow(()=>authorize(grant,tab,r,observation,2000));
  assert.throws(()=>authorize(grant,tab,r,observation,32000),/OBSERVATION_STALE/);
  assert.throws(()=>authorize(grant,tab,{...r,observation_id:'other'},observation,2000),/OBSERVATION_STALE/);
  assert.throws(()=>authorize(grant,{url:'https://example.com/changed'},r,observation,2000),/OBSERVATION_STALE/);
});
test('opening tabs is separately granted and never widens the origin',()=>{
  const r={action:'open',url:'https://example.com/next',observation_id:'fresh'};
  assert.throws(()=>authorize(grant,tab,r,observation,2000),/NEW_TAB_PERMISSION_REQUIRED/);
  assert.doesNotThrow(()=>authorize({...grant,allow_new_tabs:true},tab,r,observation,2000));
  assert.throws(()=>authorize({...grant,allow_new_tabs:true},tab,{...r,url:'https://other.test'},observation,2000),/ORIGIN_NOT_GRANTED/);
});
test('restricted schemes and embedded credentials are rejected before input',()=>{
  for(const url of ['file:///c:/secret','chrome://settings','javascript:alert(1)','data:text/html,x','https://user:pass@example.com'])assert.throws(()=>publicUrl(url));
  assert.throws(()=>validate({action:'navigate',request_id:'one'}),/URL_REQUIRED/);
});
test('invalid coordinates, oversized text and absent replay identity fail closed',()=>{
  assert.throws(()=>validate({action:'click',x:Infinity,request_id:'one'}),/INVALID_COORDINATE/);
  assert.throws(()=>validate({action:'type',text:'x'.repeat(16001),request_id:'one'}),/TEXT_LIMIT/);
  assert.throws(()=>validate({action:'click'}),/REQUEST_ID_REQUIRED/);
  assert.throws(()=>boundedInteger(64001,800,1,64000),/INTEGER_RANGE/);
});
