import {publicUrl} from './policy.mjs';
const status=document.querySelector('#status'),context=document.querySelector('#tab-context');
let target;
try{const [tab]=await chrome.tabs.query({active:true,currentWindow:true});if(!tab||!Number.isSafeInteger(tab.id)||tab.id<0)throw new Error('No active tab');target=Object.freeze({tab_id:tab.id,tab_origin:publicUrl(tab.url).origin});context.textContent=`This tab: ${(tab.title||'Untitled').slice(0,160)} (${target.tab_origin})`;}
catch(e){context.textContent='Open the popup on a public web page to grant that tab.';}
for(const id of ['grant','revoke-current'])document.querySelector('#'+id).disabled=!target;
async function call(action){try{const result=await chrome.runtime.sendMessage({action,...target,duration:document.querySelector('#duration').value,allow_new_tabs:document.querySelector('#new-tabs').checked});status.textContent=JSON.stringify(result,null,2);}catch(e){status.textContent=e.message;}}
for(const [id,action]of [['connect','connect'],['grant','grant'],['revoke-current','revoke_current'],['revoke','revoke_all']])document.querySelector('#'+id).addEventListener('click',()=>call(action));
document.querySelector('#duration').addEventListener('change',()=>{document.querySelector('#grant').textContent=document.querySelector('#duration').value==='until_revoked'?'Allow this tab until revoked':'Allow this tab for one hour';});
await call('status');
