const status=document.querySelector('#status');
async function call(action){try{const result=await chrome.runtime.sendMessage({action,allow_new_tabs:document.querySelector('#new-tabs').checked});status.textContent=JSON.stringify(result,null,2);}catch(e){status.textContent=e.message;}}
for(const [id,action]of [['connect','connect'],['grant','grant'],['revoke','revoke_all']])document.querySelector('#'+id).addEventListener('click',()=>call(action));
await call('status');
