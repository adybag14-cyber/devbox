import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFile,mkdtemp,rm} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
import {assertCppExtensions} from './native-contract.mjs';

export function matchQualifiedBinary(receipt,{target,sourceSha,bytes}) {
  assert.equal(receipt.schema,1);assert.equal(receipt.sourceSha,sourceSha);
  assert.match(String(receipt.workflowRunId||''),/^\d+$/u,'Hosted qualification receipt required');
  assert.equal(receipt.qualification,'complete_required_workflow_dependency_graph');
  for(const name of ['native','android','termux','distributions','alpine','security']) assert(receipt.requiredJobs.includes(name));
  const profile=receipt.targets.find(row=>row.target===target);assert(profile,'Qualified target required');
  assert.equal(profile.assurance?.schema,1,'Resolved dependency assurance required');
  const binary=profile.binaries.find(row=>row.name==='devbox-mcp');assert(binary,'Qualified MCP binary required');
  const sha256=createHash('sha256').update(bytes).digest('hex');
  assert.equal(binary.bytes,bytes.length);assert.equal(binary.sha256,sha256,'Downloaded and tested binary digests must be identical');
  return sha256;
}
export async function verifyPromotion({binary,target,receipt:receiptPath,bundle,sourceSha,contractVersion},runner=runCheckedProcess) {
  assert.match(sourceSha,/^[a-f0-9]{40}$/u);assert(Number.isInteger(contractVersion)&&contractVersion>0);
  // Authenticate both the binary and its receipt before executing the candidate or trusting its claims.
  for(const file of [binary,receiptPath]) await runner('gh',['attestation','verify',path.resolve(file),
    '--repo','adybag14-cyber/devbox','--signer-workflow','adybag14-cyber/devbox/.github/workflows/cpp-runtime.yml',
    '--source-digest',sourceSha,'--deny-self-hosted-runners','--bundle',path.resolve(bundle),'--format','json'],
    {timeoutMs:30000,label:'Verify signed exact-artifact provenance'});
  const receipt=JSON.parse(await readFile(receiptPath,'utf8'));
  const sha256=matchQualifiedBinary(receipt,{target,sourceSha,bytes:await readFile(binary)});
  const fixture=await mkdtemp(path.join(os.tmpdir(),'devbox-promotion-check-'));
  const env={};
  for(const [key,value] of Object.entries(process.env)) if(['PATH','SYSTEMROOT','WINDIR','SYSTEMDRIVE','TEMP','TMP','TMPDIR','HOME','USERPROFILE','LANG','LC_ALL'].includes(key.toUpperCase())) env[key]=value;
  Object.assign(env,{DEVBOX_PROJECT_ROOT:fixture,HOST_WORKSPACE_PATH:fixture,HOST_DEFAULT_WORKDIR:fixture,
    DEVBOX_WORKSPACE_PATH:fixture,MCP_JOBS_ROOT:path.join(fixture,'jobs'),MCP_STATE_ROOT:path.join(fixture,'state'),
    MCP_EXEC_SLOT_ROOT:path.join(fixture,'slots'),MCP_AUTH_MODE:'none',PUBLIC_BASE_URL:'',ENABLE_HOST_EXEC:'false',
    DEVBOX_AUTO_START:'false',ENABLE_GATEWAY_BRIDGE:'false',DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE:'1'});
  try {
  const output=await runner(path.resolve(binary),['--build-info'],{env,cwd:fixture,timeoutMs:10000,label:'Verified candidate identity'});
  const build=JSON.parse(output.stdout);
  assert.equal(build.binarySha256,sha256);assert.equal(build.gitSha,sourceSha);assert.equal(build.sourceDirty,false);
  assert.equal(build.sourceTree,receipt.sourceTree);assert.equal(build.sanitizers,false);
  assert.equal(build.stateSchemaVersion,2,'Reviewed durable state schema required; unsafe data-format changes cannot be promoted');
  assert.equal(build.stateCoordinatorProtocol,1,'Reviewed coordinator protocol required');
  assert.equal(build.contractVersion,contractVersion,'Binary contract version must match the reviewed schema');
  const contract=await runner(path.resolve(binary),['--dump-contract'],{env,cwd:fixture,timeoutMs:10000,label:'Verified candidate schema'});
  const tools=JSON.parse(contract.stdout);
  assert(Array.isArray(tools)&&tools.length>=50,'Complete native contract required');
  const registry=JSON.parse(await readFile(new URL('../contract/tool-registry.json',import.meta.url),'utf8'));
  assert.equal(registry.contract_version,contractVersion);assert.equal(tools.length,registry.tools.length);
  assert.equal(build.toolCount,tools.length);assertCppExtensions(tools);
  assert.deepEqual(tools.map(t=>t.name).sort(),registry.tools.map(t=>t.name).sort(),'Candidate capabilities must match reviewed registry');
  return {verified:true,sourceSha,sourceTree:receipt.sourceTree,binarySha256:sha256,contractVersion,target,
    stateSchemaVersion:build.stateSchemaVersion,stateCoordinatorProtocol:build.stateCoordinatorProtocol,
    workflowRunId:receipt.workflowRunId,policy:'signed_exact_tested_artifact'};
  } finally {
    assert(path.isAbsolute(fixture)&&path.basename(fixture).startsWith('devbox-promotion-check-'));
    await rm(fixture,{recursive:true,force:true});
  }
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)) {
  const [binary,target,receipt,bundle,sourceSha,version]=process.argv.slice(2);
  assert(binary&&target&&receipt&&bundle&&sourceSha&&version,'Pass binary target receipt bundle sourceSha contractVersion');
  console.log(JSON.stringify(await verifyPromotion({binary,target,receipt,bundle,sourceSha,contractVersion:Number(version)}),null,2));
}
