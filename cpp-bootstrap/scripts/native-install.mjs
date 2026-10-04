import assert from 'node:assert/strict';
import {mkdtemp,mkdir,readFile,rm} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
const binary=process.env.DEVBOX_CPP_BINARY;
const setup=process.env.DEVBOX_SETUP_TEST_BINARY;
assert(binary&&setup,'Native installer qualification requires both packaged executables');
const fixture=await mkdtemp(path.join(os.tmpdir(),'devbox-native-install-'));
const root=path.join(fixture,'installation');const empty=path.join(fixture,'empty-path');await mkdir(empty);
const env=Object.fromEntries(Object.entries(process.env).filter(([key])=>['SYSTEMROOT','WINDIR','TEMP','TMP','HOME','USERPROFILE','TMPDIR'].includes(key.toUpperCase())));
env.PATH=empty;
const port=await new Promise(resolve=>{const server=net.createServer();server.listen(0,'127.0.0.1',()=>{const port=server.address().port;server.close(()=>resolve(port));});});
const run=(file,args)=>runCheckedProcess(file,args,{env,cwd:fixture,timeoutMs:120000,label:'Native packaged setup without Node/Git'});
let configured=false;
try {
  await run(setup,['--native-root',root,'--runtime-binary',binary,'--allow-local-build','--port',String(port),'--no-start']);
  configured=true;
  const config=JSON.parse(await readFile(path.join(root,'run/native/config.json'),'utf8'));
  assert.equal(config.policy,'local-development');assert.equal(config.environment.PORT,String(port));
  assert.equal(JSON.parse((await run(binary,['manage','status','--root',root])).stdout).running,false);
  const serving=JSON.parse((await run(binary,['manage','start','--root',root])).stdout);assert.equal(serving.healthy,true);
  assert(!config.environment.PATH,'No development-tool path silently persisted');
  console.log(JSON.stringify({ok:true,profile:'native',nodeOnRuntimePath:false,checks:['native installer','no source checkout','no npm provisioning','later native start']}));
} finally {
  if(configured) {
    const state=JSON.parse((await run(binary,['manage','status','--root',root])).stdout);
    if(state.running)await run(binary,['manage','stop','--root',root]);
    for(let n=0;n<50;n++){if(!JSON.parse((await run(binary,['manage','status','--root',root])).stdout).running)break;await new Promise(r=>setTimeout(r,100));}
    assert.equal(JSON.parse((await run(binary,['manage','status','--root',root])).stdout).running,false,'retain fixture if owned process did not stop');
  }
  await rm(fixture,{recursive:true,force:true});
}
