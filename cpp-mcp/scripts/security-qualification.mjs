import assert from 'node:assert/strict';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {mkdir,writeFile} from 'node:fs/promises';
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
  targets=['devbox-core-tests','devbox-process-tests','devbox-storage-tests','devbox-security-tests','devbox-provider-tests','devbox-web-offer-tests'];
  pattern='^(core|process|storage|security|providers|web-offers)$';
} else if(mode==='linux-tsan') {
  assert.equal(process.platform,'linux');flags.push('-DDEVBOX_THREAD_SANITIZER=ON','-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG');
  targets=['devbox-async-tests','devbox-security-tests','devbox-state-coordinator-tests','devbox-scheduler-notification-tests'];
  pattern='^(async|security|state-coordinator|scheduler-notifications)$';
} else {
  assert.equal(process.platform,'linux');flags.push('-DDEVBOX_SANITIZERS=ON','-DDEVBOX_FUZZING=ON','-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG');
  targets=['devbox-fuzz-parsers'];
}
await run('cmake',flags);
await run('cmake',['--build',build,'--config','RelWithDebInfo','--target',...targets,'--parallel','4']);
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
