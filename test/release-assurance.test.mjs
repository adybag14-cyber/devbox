import test from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdtemp,mkdir,writeFile,readFile,rm} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {collectDependencies} from '../cpp-mcp/scripts/dependency-assurance.mjs';
import {vulnerabilityReport} from '../cpp-mcp/scripts/vulnerability-report.mjs';
import {matchQualifiedBinary,verifyPromotion} from '../cpp-mcp/scripts/verify-promotion.mjs';

test('resolved inventory binds ABI and rejects a missing dependency notice',async()=>{
 const root=await mkdtemp(path.join(os.tmpdir(),'devbox-assurance-test-'));
 try {
  await mkdir(path.join(root,'vcpkg'),{recursive:true});await mkdir(path.join(root,'x64-linux/share/curl'),{recursive:true});
  await writeFile(path.join(root,'vcpkg/status'),'Package: curl\nVersion: 8.22.0\nPort-Version: 1\nArchitecture: x64-linux\nAbi: abc\nStatus: install ok installed\n');
  const spdx={packages:[{SPDXID:'SPDXRef-port',name:'curl',versionInfo:'8.22.0#1',licenseConcluded:'curl',downloadLocation:'https://example.org/source'},
   {SPDXID:'SPDXRef-binary',name:'curl:x64-linux',versionInfo:'abc'}]};
  await writeFile(path.join(root,'x64-linux/share/curl/vcpkg.spdx.json'),JSON.stringify(spdx));
  const options={installed:root,triplet:'x64-linux',manifest:{dependencies:['curl'],'builtin-baseline':'a'.repeat(40)},sourceSha:'b'.repeat(40),output:path.join(root,'out')};
  await assert.rejects(collectDependencies(options),{code:'ENOENT'});
  await writeFile(path.join(root,'x64-linux/share/curl/copyright'),'Synthetic complete dependency notice for the controlled fixture.');
  const result=await collectDependencies(options);assert.equal(result.dependencyCount,1);assert.equal(result.files.length,3);
  const inventory=JSON.parse(await readFile(path.join(root,'out/dependency-inventory.json'),'utf8'));assert.equal(inventory.dependencies[0].abi,'abc');
  spdx.packages[1].versionInfo='wrong';await writeFile(path.join(root,'x64-linux/share/curl/vcpkg.spdx.json'),JSON.stringify(spdx));
  await assert.rejects(collectDependencies({...options,output:path.join(root,'wrong')}),/ABI identity/u);
 } finally {assert(path.basename(root).startsWith('devbox-assurance-test-'));await rm(root,{recursive:true,force:true});}
});
test('vulnerability reports preserve findings and explicitly limited coverage',async()=>{
 const inventories=[{dependencies:[{name:'curl',version:'8.22.0'},{name:'unmapped-library',version:'1'}]}];
 const result=await vulnerabilityReport(inventories,async(url,options)=>{
  assert.equal(url,'https://api.osv.dev/v1/querybatch');assert.equal(options.redirect,'error');
  assert.equal(JSON.parse(options.body).queries[0].package.name,'curl');
  return {status:200,text:async()=>JSON.stringify({results:[{vulns:[{id:'SYNTHETIC-VULN'}]}]})};
 });
 assert.equal(result.gate,'blocked_findings_require_remediation');assert.deepEqual(result.unmapped,['unmapped-library@1']);
 await assert.rejects(vulnerabilityReport(inventories,async()=>({status:503})),/Vulnerability service/u);
});
test('promotion refuses wrong bytes, missing required gates and signature rejection before execution',async()=>{
 const bytes=Buffer.from('controlled binary fixture');const sourceSha='c'.repeat(40);
 const receipt={schema:1,sourceSha,workflowRunId:'123',qualification:'complete_required_workflow_dependency_graph',
  requiredJobs:['native','android','termux','distributions','alpine','security'],targets:[{target:'fixture',assurance:{schema:1},
   binaries:[{name:'devbox-mcp',bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex')}]}]};
 assert.equal(matchQualifiedBinary(receipt,{target:'fixture',sourceSha,bytes}).length,64);
 assert.throws(()=>matchQualifiedBinary(receipt,{target:'fixture',sourceSha,bytes:Buffer.from('tamper')}));
 assert.throws(()=>matchQualifiedBinary({...receipt,requiredJobs:[]},{target:'fixture',sourceSha,bytes}));
 const calls=[];
 await assert.rejects(verifyPromotion({binary:'never-execute',target:'fixture',receipt:'never-read',bundle:'absent',sourceSha,contractVersion:8},async(file,args)=>{
  calls.push({file,args});throw new Error('signature rejected');
 }),/signature rejected/u);
 assert.equal(calls.length,1);assert.equal(calls[0].file,'gh');assert(calls[0].args.includes('--source-digest'));
});
