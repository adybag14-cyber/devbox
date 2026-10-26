// Build-time only. Neither Rust, Cargo, Git nor Node is shipped or needed by
// the installed native resolver. Upstream prebuilt archives enable PCRE2;
// compile a reviewed source pin without that optional engine instead.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {access,chmod,copyFile,mkdir,mkdtemp,readFile,readdir,stat,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
import {inspectRuntimeFile} from './runtime-imports.mjs';
import {applyRipgrepFixtures,fixtureHash,qualifyRipgrepAccessOrder} from './ripgrep-fixtures.mjs';

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
// Upstream tests deliberately distinguish a non-repository directory from a
// repository. Devbox's Windows TEMP is inside its checkout for separate tests;
// use a private sibling on the same volume, outside every .git ancestor.
const testTemp=await mkdtemp(path.join(path.dirname(repo),'devbox-rg-tests-'));
for(let directory=testTemp;;directory=path.dirname(directory)) {
  assert(!await exists(path.join(directory,'.git')),'ripgrep test TEMP must be outside a repository');
  if(path.dirname(directory)===directory)break;
}
env.TEMP=testTemp;env.TMP=testTemp;env.TMPDIR=testTemp;
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
const accessTimeProbes=await qualifyRipgrepAccessOrder(path.join(testTemp,'controlled-access-order'),
  (args,cwd)=>run(binary,args,cwd,env));
let qualified=true,limitation=null;
const runtimeBeforeTests=fixtureHash(await readFile(binary));
const fixtureUpdate=await applyRipgrepFixtures(source,pin.source);
try {
  const testLog=await run('cargo',[common[0],'test',...common.slice(1),'--profile',pin.profile,'--test','integration','--jobs','2'],source,env,600000);
  assert.match(testLog,/test result: ok\./u,'Upstream integration completion is required');
  await writeFile(path.join(root,'upstream-integration.log'),testLog+'\n');
} catch(error) {
  const failed=[...String(error.message).matchAll(/^---- (\S+) stdout ----$/gmu)].map(match=>match[1]).sort();
  if(!localDiagnostic||process.platform!=='win32'||!String(error.message).includes('code: 1314')||
     JSON.stringify(failed)!==JSON.stringify(['misc::symlink_nofollow','regression::r1389_bad_symlinks_no_biscuit']))throw error;
  qualified=false;limitation={kind:'local_symbolic_link_privilege_unavailable',failed,
    releaseAllowed:false,message:'The two upstream symlink fixtures require Windows privileges absent from this development shell.'};
  await writeFile(path.join(root,'local-test-environment-failure.txt'),String(error.message));
} finally {
  await fixtureUpdate.restore();
}
assert.equal(await run('git',['status','--porcelain','--untracked-files=all'],source),'');
assert.equal(fixtureHash(await readFile(binary)),runtimeBeforeTests,'Test-only fixture edits must not alter the shipped ripgrep binary');
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
// Rust's standard library is statically linked too. Its component copyright
// report includes notices beyond the project-level MIT/Apache licence pair.
// Use the authenticated pinned toolchain's installed documents, without another
// network fetch, and retain the licence texts referenced by that report.
const sysroot=await run('rustc',[`+${pin.rust}`,'--print','sysroot']);
const rustDocs=path.join(sysroot,'share/doc/rust');
const boundedNotice=async(file,limit)=>{
  const info=await stat(file);assert(info.isFile()&&info.size>0&&info.size<=limit,'Bounded complete Rust notice');
  return readFile(file,'utf8');
};
const libraryCopyright=await boundedNotice(path.join(rustDocs,'COPYRIGHT-library.html'),1024*1024);
assert(libraryCopyright.includes('Copyright notices for The Rust Standard Library'));
const rustNotices=[`===== Rust ${pin.rust} COPYRIGHT-library.html =====\n${libraryCopyright}`];
const licenseDirectory=path.join(rustDocs,'licenses');
const licenseNames=(await readdir(licenseDirectory)).sort();
assert(licenseNames.length<=128&&licenseNames.includes('MIT.txt')&&licenseNames.includes('Apache-2.0.txt'));
for(const name of licenseNames) {
  assert(/^[A-Za-z0-9.-]{1,72}\.txt$/u.test(name),'Pinned Rust license filename');
  if(!['MIT.txt','Apache-2.0.txt'].includes(name)&&!libraryCopyright.includes(name.slice(0,-4)))continue;
  rustNotices.push(`===== Rust ${pin.rust} licenses/${name} =====\n${await boundedNotice(path.join(licenseDirectory,name),256*1024)}`);
}
const rustNotice=rustNotices.join('\n');notices.push(rustNotice);
packages.push({name:'rust-standard-library',version:pin.rust,license:'NOASSERTION',ecosystem:'generic',
  source:`https://github.com/rust-lang/rust/tree/${pin.rust}`,bundledTool:'ripgrep',buildSupportOnly:false,
  noticeSha256:createHash('sha256').update(rustNotice).digest('hex'),
  abi:createHash('sha256').update(rustVersion).digest('hex')});
const file='devbox-rg'+(process.platform==='win32'?'.exe':'');
await copyFile(binary,path.join(root,file));await chmod(path.join(root,file),0o755);
const bytes=await readFile(path.join(root,file));
const license='ripgrep-NOTICES.txt';const notice=notices.join('\n');
await writeFile(path.join(root,license),notice);
const result={schema:1,name:'ripgrep',version:pin.version,source:pin.source,repository:pin.repository,
  compiler:rustVersion,target,runtime,features:[],pcre2:false,file,bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex'),
  license,noticeSha256:createHash('sha256').update(notice).digest('hex'),packages,qualified,limitation,
  testFixtures:{...fixtureUpdate.metadata,runtimeBinaryUnchanged:true,accessTimeProbes},
  tests:qualified?'upstream integration and actual file search passed':'local test environment incomplete; not release qualified'};
await writeFile(path.join(root,'manifest.json'),JSON.stringify(result,null,2)+'\n');
console.log(JSON.stringify({ok:true,manifest:path.join(root,'manifest.json'),sha256:result.sha256,bytes:result.bytes,crates:used.size,pcre2:false}));
