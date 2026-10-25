// Build-time only. Neither Rust, Cargo, Git nor Node is shipped or needed by
// the installed native resolver. Upstream prebuilt archives enable PCRE2;
// compile a reviewed source pin without that optional engine instead.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {access,chmod,copyFile,mkdir,readFile,readdir,stat,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
import {inspectRuntimeFile} from './runtime-imports.mjs';

const repo=path.resolve(import.meta.dirname,'../..');
const pin=JSON.parse(await readFile(path.join(repo,'cpp-mcp/bundled-ripgrep.json'),'utf8'));
assert.match(pin.source,/^[a-f0-9]{40}$/u);assert.match(pin.rust,/^\d+\.\d+\.\d+$/u);
assert.deepEqual(pin.features,[]);assert.equal(pin.pcre2,false);
const root=path.resolve(process.argv[2]||path.join(repo,'.cpp-build/bundled-ripgrep'));
const localDiagnostic=process.argv[3]==='--local-diagnostic';
assert(!localDiagnostic||!process.env.GITHUB_ACTIONS,'Hosted bundles require every upstream test');
const source=path.join(root,'source');
await mkdir(root,{recursive:true});
const run=async(file,args,cwd=root,env=process.env,timeoutMs=180000)=>{
  console.log(`ripgrep qualification: ${path.basename(file)} ${args.join(' ')}`);
  return (await runCheckedProcess(file,args,
    {cwd,env,timeoutMs,maxCaptureChars:4*1024*1024,label:'Pinned optional ripgrep build'})).stdout.trim();
};
const exists=async file=>{try{await access(file);return true;}catch(error){if(error.code==='ENOENT')return false;throw error;}};
if(!await exists(path.join(source,'.git'))) {
  await mkdir(source,{recursive:true});
  assert.deepEqual(await readdir(source),[],'Do not reuse an unrelated source directory');
  await run('git',['init','--quiet'],source);
  await run('git',['-c','core.hooksPath=/dev/null','fetch','--depth=1',pin.repository,pin.source],source);
  await run('git',['-c','core.hooksPath=/dev/null','checkout','--detach','--quiet','FETCH_HEAD'],source);
}
assert.equal(await run('git',['rev-parse','HEAD'],source),pin.source);
assert.equal(await run('git',['status','--porcelain','--untracked-files=all'],source),'');
const rustVersion=await run('rustc',[`+${pin.rust}`,'--version']);
assert(rustVersion.startsWith(`rustc ${pin.rust} `),'Install the pinned build toolchain explicitly');
const host=(await run('rustc',[`+${pin.rust}`,'-vV'])).split('\n').find(line=>line.startsWith('host: ')).slice(6);
assert(/^(?:x86_64|aarch64)-(?:pc-windows-msvc|unknown-linux-gnu|apple-darwin)$/u.test(host));
const target=process.platform==='linux'?host.replace('unknown-linux-gnu','unknown-linux-musl'):host;
const env={...process.env,CARGO_TARGET_DIR:path.join(root,'target'),CARGO_INCREMENTAL:'0',
  CARGO_ENCODED_RUSTFLAGS:[`--remap-path-prefix=${source}=/ripgrep`,...(['win32','linux'].includes(process.platform)?['-Ctarget-feature=+crt-static']:[])].join('\x1f')};
// Rust supplies the matching musl CRT/linker inputs. An older host musl-gcc
// wrapper can inject a dynamic interpreter into a static PIE, so leave final
// linking to Rust's target defaults; musl-gcc remains a build-only C compiler.
if(process.platform==='linux')delete env[`CARGO_TARGET_${target.replaceAll('-','_').toUpperCase()}_LINKER`];
const common=[`+${pin.rust}`,'--locked','--no-default-features','--package','ripgrep','--target',target];
// Validate the resolved *used* graph, not every optional workspace member in
// Cargo.lock. In particular PCRE2 must be absent, including transitive edges.
const tree=await run('cargo',[common[0],'tree',...common.slice(1),'--edges','normal,build','--prefix','none','--format','{p}'],source,env);
assert(!/\bpcre2(?:-sys)?\b/u.test(tree),'Optional PCRE2 must not enter the bundle');
const used=new Set(tree.split('\n').filter(Boolean).map(line=>{
  const match=line.match(/^([a-zA-Z0-9_-]+) v([^ (]+)/u);assert(match,'Resolved crate identity');return `${match[1]}@${match[2]}`;
}));
const metadata=JSON.parse(await run('cargo',[common[0],'metadata','--locked','--no-default-features','--format-version','1','--filter-platform',target],source,env));
await run('cargo',[common[0],'build',...common.slice(1),'--profile',pin.profile,'--jobs','2'],source,env,600000);
const binary=path.join(root,'target',target,pin.profile,'rg'+(process.platform==='win32'?'.exe':''));
const runtime=await inspectRuntimeFile(binary);
if(process.platform==='linux')assert.deepEqual(runtime.imports,[],'The portable Linux fallback must have no dynamic runtime dependency');
const version=await run(binary,['--version']);
assert(version.startsWith(`ripgrep ${pin.version} `));assert.match(version,/features:-pcre2/u);
assert.match(version,/PCRE2 is not available/u);
const fixture=path.join(root,'search-fixture.txt');await writeFile(fixture,'first\nphone £799\nlast\n');
assert.equal(await run(binary,['--no-config','--fixed-strings','--line-number','phone',fixture]),'2:phone £799');
let qualified=true,limitation=null;
try {
  await run('cargo',[common[0],'test',...common.slice(1),'--profile',pin.profile,'--test','integration','--jobs','2'],source,env,600000);
} catch(error) {
  const failed=[...String(error.message).matchAll(/^---- (\S+) stdout ----$/gmu)].map(match=>match[1]).sort();
  if(!localDiagnostic||process.platform!=='win32'||!String(error.message).includes('code: 1314')||
     JSON.stringify(failed)!==JSON.stringify(['misc::symlink_nofollow','regression::r1389_bad_symlinks_no_biscuit']))throw error;
  qualified=false;limitation={kind:'local_symbolic_link_privilege_unavailable',failed,
    releaseAllowed:false,message:'The two upstream symlink fixtures require Windows privileges absent from this development shell.'};
  await writeFile(path.join(root,'local-test-environment-failure.txt'),String(error.message));
}
const packages=[],notices=[];
for(const item of metadata.packages) {
  if(!used.has(`${item.name}@${item.version}`))continue;
  assert(item.license||item.license_file,`Crate ${item.name} needs a declared license`);
  const directory=path.dirname(item.manifest_path);
  let names=(await readdir(directory)).filter(name=>/^(?:LICENSE|LICENCE|COPYING|UNLICENSE|NOTICE)(?:[.-].*)?$/iu.test(name));
  if(!names.length && directory.startsWith(source+path.sep))names=['LICENSE-MIT','UNLICENSE'].map(name=>path.relative(directory,path.join(source,name)));
  if(item.license_file && !names.includes(item.license_file))names.push(item.license_file);
  const texts=[];
  for(const name of names) {
    const file=path.resolve(directory,name);
    assert(file.startsWith(directory+path.sep)||file.startsWith(source+path.sep),'Notice must belong to its package/source');
    if(!(await stat(file)).isFile())continue;
    const text=await readFile(file,'utf8');assert(Buffer.byteLength(text)<=256*1024,'Bounded license file');texts.push(`${name}\n${text}`);
  }
  assert(texts.length,`Missing redistribution notice for ${item.name}`);
  const notice=texts.join('\n');
  const row={name:item.name,version:item.version,license:item.license||'NOASSERTION',ecosystem:'crates.io',
    source:item.source||`${pin.repository}/tree/${pin.source}`,bundledTool:'ripgrep',
    noticeSha256:createHash('sha256').update(notice).digest('hex'),abi:pin.source,buildSupportOnly:false};
  packages.push(row);notices.push(`===== ${item.name} ${item.version} =====\n${notice}`);
}
assert.equal(packages.length,used.size,'Every linked/build crate must have an inventory row');
// Rust's standard library is statically linked as well. Retain its source pin
// and both upstream license texts in the same redistribution notice.
for(const name of ['LICENSE-MIT','LICENSE-APACHE']) {
  const response=await fetch(`https://raw.githubusercontent.com/rust-lang/rust/${pin.rust}/${name}`,{signal:AbortSignal.timeout(20000),redirect:'error'});
  assert.equal(response.status,200);const text=await response.text();assert(Buffer.byteLength(text)<65536);
  notices.push(`===== Rust ${pin.rust} ${name} =====\n${text}`);
}
packages.push({name:'rust-standard-library',version:pin.rust,license:'MIT OR Apache-2.0',ecosystem:'generic',
  source:`https://github.com/rust-lang/rust/tree/${pin.rust}`,bundledTool:'ripgrep',buildSupportOnly:false,
  abi:createHash('sha256').update(rustVersion).digest('hex')});
const file='devbox-rg'+(process.platform==='win32'?'.exe':'');
await copyFile(binary,path.join(root,file));await chmod(path.join(root,file),0o755);
const bytes=await readFile(path.join(root,file));
const license='ripgrep-NOTICES.txt';const notice=notices.join('\n');
await writeFile(path.join(root,license),notice);
const result={schema:1,name:'ripgrep',version:pin.version,source:pin.source,repository:pin.repository,
  compiler:rustVersion,target,runtime,features:[],pcre2:false,file,bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex'),
  license,noticeSha256:createHash('sha256').update(notice).digest('hex'),packages,qualified,limitation,
  tests:qualified?'upstream integration and actual file search passed':'local test environment incomplete; not release qualified'};
await writeFile(path.join(root,'manifest.json'),JSON.stringify(result,null,2)+'\n');
console.log(JSON.stringify({ok:true,manifest:path.join(root,'manifest.json'),sha256:result.sha256,bytes:result.bytes,crates:used.size,pcre2:false}));
