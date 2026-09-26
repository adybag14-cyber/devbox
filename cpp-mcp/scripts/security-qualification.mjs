import assert from 'node:assert/strict';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {mkdir,writeFile,readdir,readFile} from 'node:fs/promises';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const mode=process.argv[2];assert(['windows-asan-analysis','linux-tsan','linux-fuzz'].includes(mode));
const vcpkg=process.env.VCPKG_ROOT;assert(vcpkg&&path.isAbsolute(vcpkg));
const build=path.join(repo,'.cpp-build/security',mode);
const env={...process.env,VCPKG_MAX_CONCURRENCY:'4'};
if(process.platform==='win32') {const temp=path.join(repo,'.cpp-build/security-temp');await mkdir(temp,{recursive:true});env.TEMP=temp;env.TMP=temp;}
const run=(file,args)=>runCheckedProcess(file,args,{cwd:repo,env,stdio:'inherit',timeoutMs:80*60*1000,label:`Native security qualification ${mode}`});
const flags=['-S',repo,'-B',build,'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DDEVBOX_BUILD_TUI=OFF','-DDEVBOX_BUILD_TESTS=ON',
  `-DCMAKE_TOOLCHAIN_FILE=${path.join(vcpkg,'scripts/buildsystems/vcpkg.cmake')}`,
  `-DVCPKG_INSTALLED_DIR=${path.join(repo,'.cpp-build/security-vcpkg-installed')}`,
  `-DVCPKG_TARGET_TRIPLET=${process.platform==='win32'?'x64-windows-static-asan':'x64-linux'}`];
let targets,pattern;
if(mode==='windows-asan-analysis') {
  assert.equal(process.platform,'win32');flags.push('-DDEVBOX_MSVC_ASAN=ON','-DDEVBOX_STATIC_ANALYSIS=ON',
    `-DVCPKG_OVERLAY_TRIPLETS=${path.join(repo,'cpp-mcp/triplets')}`);
  targets=['devbox-core-tests','devbox-process-tests','devbox-storage-tests','devbox-security-tests','devbox-provider-tests','devbox-web-offer-tests','devbox-state-coordinator-tests','devbox-state-read-reuse-tests','devbox-state-read-bench','devbox-state-transport-tests','devbox-state-ipc-bench'];
  pattern='^(core|process|storage|security|providers|web-offers|state-coordinator|state-read-reuse|state-read-benchmark-smoke|state-transport|state-ipc-benchmark-smoke)$';
} else if(mode==='linux-tsan') {
  assert.equal(process.platform,'linux');flags.push('-DDEVBOX_THREAD_SANITIZER=ON','-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG');
  targets=['devbox-async-tests','devbox-security-tests','devbox-state-coordinator-tests','devbox-scheduler-notification-tests','devbox-state-read-reuse-tests','devbox-state-read-bench','devbox-state-transport-tests','devbox-state-ipc-bench'];
  pattern='^(async|security|state-coordinator|scheduler-notifications|state-read-reuse|state-read-benchmark-smoke|state-transport|state-ipc-benchmark-smoke)$';
} else {
  assert.equal(process.platform,'linux');flags.push('-DDEVBOX_SANITIZERS=ON','-DDEVBOX_FUZZING=ON','-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG');
  targets=['devbox-fuzz-parsers'];
}
await run('cmake',flags);
await run('cmake',['--build',build,'--config','RelWithDebInfo','--target',...targets,'--parallel','4']);
if(mode==='windows-asan-analysis') {
  const files=[];let visited=0;
  async function visit(root) {
    for(const item of await readdir(root,{withFileTypes:true})) {
      assert(++visited<=50000,'Bounded analysis artifact enumeration');
      const file=path.join(root,item.name);
      if(item.isDirectory()) await visit(file);
      else if(item.isFile()&&item.name.endsWith('.sarif')) files.push(file);
    }
  }
  await visit(build);assert(files.length>0,'Static analysis must produce retained SARIF evidence');
  const findings=[];
  for(const file of files) {
    const bytes=await readFile(file);assert(bytes.length<=32*1024*1024,'Bounded analysis file');
    const sarif=JSON.parse(bytes.toString('utf8').replace(/^\uFEFF/u,''));
    for(const run of sarif.runs||[]) for(const result of run.results||[])
      if(result.level!=='note'&&result.kind!=='pass') findings.push({file:path.relative(build,file),
        ruleId:result.ruleId,level:result.level||'warning',message:result.message});
  }
  await writeFile(path.join(build,'static-analysis-result.json'),JSON.stringify({files:files.length,findings},null,2)+'\n');
  assert.equal(findings.length,0,'Static-analysis findings require correction before qualification');
}
if(pattern) await run('ctest',['--test-dir',build,'-C','RelWithDebInfo','-R',pattern,'--output-on-failure']);
else {
  const corpus=path.join(build,'corpus');await mkdir(corpus,{recursive:true});
  for(const [name,body] of Object.entries({html:'<html><title>Phone</title><main>Untrusted retailer data</main></html>',
    json:'{"@type":"Product","name":"Phone","offers":{"@type":"Offer","price":529,"priceCurrency":"GBP"}}',
    nested:'[[[[{"priceAmount":0,"buyingOptionType":"NEW"}]]]]',date:'Wed, 21 Oct 2026 07:28:00 GMT'}))
    await writeFile(path.join(corpus,name),body);
  await run(path.join(build,'cpp-mcp/devbox-fuzz-parsers'),[corpus,'-runs=20000','-max_len=65536','-timeout=5','-rss_limit_mb=2048']);
}
await writeFile(path.join(build,'security-result.json'),JSON.stringify({schema:1,mode,passed:true,
  scope:pattern||'bounded_html_json_offer_and_retry_after_fuzz',productionArtifact:false},null,2)+'\n');
