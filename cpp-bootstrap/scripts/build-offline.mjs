import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {chmod,copyFile,mkdir,readFile,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
import {inspectRuntimeFile} from '../../cpp-mcp/scripts/runtime-imports.mjs';

const repo=path.resolve(import.meta.dirname,'../..');
const build=path.resolve(process.argv[2]||'.cpp-build/ci');
const packageRoot=path.resolve(process.argv[3]||'.cpp-build/package');
const manifest=JSON.parse(await readFile(path.join(packageRoot,'build-manifest.json'),'utf8'));
assert.equal(manifest.build.sourceDirty,false);
assert.equal(manifest.build.sanitizers,false);
assert.match(process.env.GITHUB_RUN_ID||'',/^[0-9]+$/u,'Pass the hosted workflow identity');
const suffix=process.platform==='win32'?'.exe':'';
const bin=path.join(build,'cpp-bootstrap',...(process.platform==='win32'?['Release']:[]));
const packer=path.join(bin,'devbox-bundle-pack'+suffix);
const staging=path.join(build,'offline-input');
const output=path.join(packageRoot,'offline');
await mkdir(staging);await mkdir(output);
await copyFile(path.join(repo,'docs/OFFLINE.md'),path.join(packageRoot,'OFFLINE.md'));
const payload=path.join(staging,'devbox.payload'),metadata=path.join(staging,'manifest.json');
const run=(file,args)=>runCheckedProcess(file,args,{cwd:repo,timeoutMs:300000,maxCaptureChars:65536,label:'Native offline bundle construction'});
await run(packer,['pack',packageRoot,payload,metadata,process.env.GITHUB_RUN_ID]);
const embedded=JSON.parse(await readFile(metadata,'utf8'));
assert.equal(embedded.source,manifest.gitSha);assert.equal(embedded.tree,manifest.sourceTree);
await run('cmake',['-S',repo,'-B',build,`-DDEVBOX_OFFLINE_MANIFEST=${metadata}`]);
await run('cmake',['--build',build,'--config','Release','--target','devbox-offline','--parallel','2',...(process.platform==='win32'?['--','/nodeReuse:false']:[])]);
const stub=path.join(bin,'devbox-offline'+suffix);
const runtime=await inspectRuntimeFile(stub);
const files=[];
let installer;
await copyFile(path.join(repo,'docs/OFFLINE.md'),path.join(output,'OFFLINE.md'));
files.push('OFFLINE.md');
if(process.platform==='darwin') {
  const contents=path.join(output,'Devbox.app','Contents');
  await mkdir(path.join(contents,'MacOS'),{recursive:true});
  await mkdir(path.join(contents,'Resources'));
  installer=path.join(contents,'MacOS','devbox-offline');
  await copyFile(stub,installer);await chmod(installer,0o755);
  await copyFile(payload,path.join(contents,'Resources','devbox.payload'));
  await writeFile(path.join(contents,'Info.plist'),`<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleExecutable</key><string>devbox-offline</string>
<key>CFBundleIdentifier</key><string>com.adrianotothestar.devbox.offline</string>
<key>CFBundleName</key><string>Devbox Offline Installer</string>
<key>CFBundleVersion</key><string>${manifest.setupVersion}</string>
<key>CFBundlePackageType</key><string>APPL</string></dict></plist>\n`);
  files.push('Devbox.app/Contents/MacOS/devbox-offline','Devbox.app/Contents/Resources/devbox.payload','Devbox.app/Contents/Info.plist');
} else {
  const name='devbox-offline'+(suffix||'.run');
  installer=path.join(output,name);
  await run(packer,['attach',stub,payload,installer]);
  await chmod(installer,0o755);
  files.push(name);
}
const inspected=JSON.parse((await run(installer,['--inspect'])).stdout);
assert.deepEqual(inspected,embedded);
const records=[];
for(const file of files) {
  const bytes=await readFile(path.join(output,file));
  records.push({file,bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex')});
}
await writeFile(path.join(output,'offline-manifest.json'),JSON.stringify({
  schema:1,source:manifest.gitSha,tree:manifest.sourceTree,target:manifest.target,
  version:manifest.setupVersion,installer:path.relative(output,installer).split(path.sep).join('/'),
  runtime,payload:embedded,files:records
},null,2)+'\n');
console.log(JSON.stringify({ok:true,source:manifest.gitSha,target:manifest.target,installer,output}));
