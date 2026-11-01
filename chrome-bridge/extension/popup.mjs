const status=document.querySelector('#status');
async function call(action){try{const result=await chrome.runtime.sendMessage({action,duration:document.querySelector('#duration').value,allow_new_tabs:document.querySelector('#new-tabs').checked});status.textContent=JSON.stringify(result,null,2);}catch(e){status.textContent=e.message;}}
for(const [id,action]of [['connect','connect'],['grant','grant'],['revoke-current','revoke_current'],['revoke','revoke_all']])document.querySelector('#'+id).addEventListener('click',()=>call(action));
document.querySelector('#duration').addEventListener('change',()=>{document.querySelector('#grant').textContent=document.querySelector('#duration').value==='until_revoked'?'Allow this tab until revoked':'Allow this tab for one hour';});
await call('status');
