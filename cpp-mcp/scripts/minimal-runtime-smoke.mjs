import assert from 'node:assert/strict';
import {randomUUID,createHash} from 'node:crypto';
import {readFile,mkdtemp,mkdir,copyFile,writeFile,rm} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
import {inspectRuntimeFile} from './runtime-imports.mjs';

assert.equal(process.platform,'linux');
const binary=path.resolve(process.argv[2]||process.env.DEVBOX_CPP_BINARY||'');
const image=process.argv[3]||'debian:13-slim';
assert(['debian:13-slim','alpine:3.23'].includes(image));
const identity=`devbox-minimal-${randomUUID()}`;
const label='io.devbox.minimal.fixture';
const root=await mkdtemp(path.join(os.tmpdir(),'devbox-minimal-container-'));
const run=(file,args,options={})=>runCheckedProcess(file,args,{timeoutMs:120000,maxCaptureChars:65536,label:'Clean minimal native runtime',...options});
try {
  const input=path.join(root,'input');await mkdir(input);await copyFile(binary,path.join(input,'devbox-mcp'));
  const imports=await inspectRuntimeFile(binary);
  await writeFile(path.join(input,'run.sh'),`set -eu
for tool in node npm git python3 chromium Xvfb; do
  if command -v "$tool" >/dev/null 2>&1; then echo "Unexpected runtime dependency installed: $tool" >&2; exit 1; fi
done
cp /input/devbox-mcp /tmp/devbox-mcp
chmod 700 /tmp/devbox-mcp
/tmp/devbox-mcp manage init --root /tmp/core --allow-local-build --port 18195
/tmp/devbox-mcp manage start --root /tmp/core
/tmp/devbox-mcp manage status --root /tmp/core
/tmp/devbox-mcp manage restart --root /tmp/core
/tmp/devbox-mcp manage stop --root /tmp/core
`);
  await run('docker',['pull',image],{timeoutMs:600000});
  const info=JSON.parse((await run('docker',['image','inspect',image])).stdout)[0];
  const result=await run('docker',['run','--rm','--name',identity,'--label',`${label}=${identity}`,'--network','none',
    '--memory','256m','--pids-limit','128','-v',`${input}:/input:ro`,info.Id,'sh','/input/run.sh']);
  assert(result.stdout.includes('"healthy": true'),result.stdout);
  console.log(JSON.stringify({ok:true,image,imageId:info.Id,network:'none',memoryLimitMiB:256,
    sha256:createHash('sha256').update(await readFile(binary)).digest('hex'),imports,
    absent:['node','npm','git','python3','chromium','Xvfb'],checks:['native init','start','health','restart','drained stop']}));
} finally {
  const named=(await run('docker',['ps','-a','--filter',`label=${label}=${identity}`,'--format','{{.Names}}'])).stdout.trim();
  if(named) {
    assert.equal(named,identity);const info=JSON.parse((await run('docker',['inspect',identity])).stdout)[0];
    assert.equal(info.Config.Labels[label],identity);
    assert(info.Mounts.some(m=>m.Source===path.join(root,'input')&&m.Destination==='/input'&&!m.RW));
    await run('docker',['rm','-f',info.Id]);
  }
  await rm(root,{recursive:true,force:true});
}
