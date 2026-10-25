import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createReadStream } from 'node:fs';
import { chmod, copyFile, mkdir, mkdtemp, readFile, readdir, rename, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runCheckedProcess } from '../../src/mcp-implementation.js';
import {vulnerabilityReport} from './vulnerability-report.mjs';
import {runtimeImports,assertCoreImports} from './runtime-imports.mjs';

assert.equal(process.platform, 'linux', 'Assemble release artifacts on the isolated Linux packaging runner');
const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const input = path.resolve(process.argv[2] || '.cpp-build/release-input');
const output = path.resolve(process.argv[3] || '.cpp-build/release-assets');
const run = async (file, args) => (await runCheckedProcess(file, args,
  { cwd: repo, timeoutMs: 120000, label: 'Verified C++ release packaging' })).stdout.trim();
const expectedSha = await run('git', ['rev-parse', 'HEAD']);
const expectedTree = await run('git', ['rev-parse', 'HEAD^{tree}']);
const version = (await readFile(path.join(repo, 'cpp-bootstrap/VERSION'), 'utf8')).trim();
if (process.env.GITHUB_REF_TYPE === 'tag') assert.equal(process.env.GITHUB_REF_NAME, `bootstrap-v${version}`);
const baseline = JSON.parse(await readFile(path.join(repo, 'vcpkg.json'), 'utf8'))['builtin-baseline'];
const targets = {
  'windows-x86_64': 'cpp-native-windows-2025-0/package',
  'linux-x86_64': 'cpp-native-ubuntu-24.04-0/package',
  'linux-aarch64': 'cpp-native-ubuntu-24.04-arm-0/package',
  'macos-aarch64': 'cpp-native-macos-15-0/package',
  'macos-x86_64': 'cpp-native-macos-15-intel-0/package',
  'linux-musl-x86_64': 'cpp-musl-alpine-3.23',
  ...Object.fromEntries(['arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86'].map(abi => [`android-${abi}`, `cpp-android-${abi}`])),
};
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
async function fileDigest(file) {
  const hash=createHash('sha256');
  for await(const bytes of createReadStream(file,{highWaterMark:128*1024}))hash.update(bytes);
  return hash.digest('hex');
}
const machines = { x86_64: 62, aarch64: 183, 'arm64-v8a': 183, 'armeabi-v7a': 40, x86: 3 };
function assertArchitecture(bytes, target) {
  if (target.startsWith('windows-')) {
    assert.equal(bytes.subarray(0, 2).toString(), 'MZ');
    const pe = bytes.readUInt32LE(0x3c);
    assert.equal(bytes.readUInt32LE(pe), 0x4550); assert.equal(bytes.readUInt16LE(pe + 4), 0x8664);
  } else if (target.startsWith('macos-')) {
    assert.equal(bytes.readUInt32LE(0), 0xfeedfacf);
    assert.equal(bytes.readUInt32LE(4), target.endsWith('aarch64') ? 0x100000c : 0x1000007);
  } else {
    assert.equal(bytes.subarray(0, 4).toString('hex'), '7f454c46'); assert.equal(bytes[5], 1);
    const architecture = Object.keys(machines).find(arch => target.endsWith(`-${arch}`));
    assert.equal(bytes.readUInt16LE(18), machines[architecture]);
  }
}
await mkdir(output, { recursive: true });
assert.deepEqual(await readdir(output), [], 'Refuse to mix release assets with an earlier packaging attempt');
const manifests = [];
const offlineBundles = [];
let offlineAssets = 0;
const scratch = await mkdtemp(path.join(os.tmpdir(), 'devbox-cpp-release-'));
try {
  const universalDirectory = path.join(scratch, 'universal');
  await mkdir(universalDirectory);
  for (const [target, directory] of Object.entries(targets)) {
    const source = path.join(input, directory);
    const manifest = JSON.parse(await readFile(path.join(source, 'build-manifest.json'), 'utf8'));
    assert.equal(manifest.schema, 1); assert.equal(manifest.target, target);
    assert.equal(manifest.implementation, 'cpp'); assert.equal(manifest.setupVersion, version);
    if (process.env.GITHUB_REF_TYPE === 'tag') assert.equal(manifest.releaseReady, true, 'Incomplete candidates cannot be published');
    assert.equal(manifest.gitSha, expectedSha); assert.equal(manifest.sourceTree, expectedTree);
    assert.equal(manifest.dependencyBaseline, baseline);
    assert.equal(manifest.build.gitSha, expectedSha); assert.equal(manifest.build.sourceTree, expectedTree);
    assert.equal(manifest.build.sourceDirty, false); assert.equal(manifest.build.sanitizers, false);
    assert.deepEqual(manifest.binaries.map(binary => binary.name).sort(), ['devbox-mcp', 'devbox-setup', 'devbox-tui']);
    const bundle = path.join(scratch, target); await mkdir(bundle);
    const extension = target.startsWith('windows-') ? '.exe' : '';
    for (const binary of manifest.binaries) {
      const expectedFile = `${target.startsWith('android-') ? '' : 'bin/'}${binary.name}${extension}`;
      assert.equal(binary.file, expectedFile);
      const bytes = await readFile(path.join(source, expectedFile));
      assert.equal(bytes.length, binary.bytes); assert.equal(digest(bytes), binary.sha256);
      assertArchitecture(bytes, target);
      const imports=runtimeImports(bytes);assertCoreImports(imports);assert.deepEqual(binary.runtime,imports);
      const standalone = path.join(output, `${binary.name}-${target}${extension}`);
      await writeFile(standalone, bytes, { flag: 'wx', mode: 0o755 });
      await copyFile(standalone, path.join(bundle, binary.name + extension));
      await chmod(path.join(bundle, binary.name + extension), 0o755);
    }
    const optionalNames=[];
    assert(Array.isArray(manifest.optionalTools)&&manifest.optionalTools.length<=1);
    for(const tool of manifest.optionalTools) {
      assert.equal(tool.name,'ripgrep');assert.equal(tool.releaseQualified,true);assert.equal(tool.pcre2,false);
      assert.equal(tool.file,`bin/devbox-rg${extension}`);assert.equal(tool.license,'share/devbox/ripgrep-NOTICES.txt');
      assert.equal(tool.sha256,manifest.build.bundledRipgrep.sha256);
      const bytes=await readFile(path.join(source,tool.file));
      assert.equal(bytes.length,tool.bytes);assert.equal(digest(bytes),tool.sha256);assertArchitecture(bytes,target);
      assert.deepEqual(runtimeImports(bytes),tool.runtime);assertCoreImports(tool.runtime);
      const notice=await readFile(path.join(source,tool.license));assert.equal(digest(notice),tool.noticeSha256);
      await writeFile(path.join(bundle,`devbox-rg${extension}`),bytes,{flag:'wx',mode:0o755});
      await writeFile(path.join(bundle,'ripgrep-NOTICES.txt'),notice,{flag:'wx'});
      optionalNames.push(`devbox-rg${extension}`,'ripgrep-NOTICES.txt');
    }
    assert.equal(manifest.assurance?.schema,1,'Resolved dependency assurance is mandatory');
    assert.deepEqual(manifest.assurance.files.map(x=>x.file).sort(),['THIRD_PARTY_NOTICES.txt','dependency-inventory.json','sbom.spdx.json'].sort());
    for(const item of manifest.assurance.files) {
      const bytes=await readFile(path.join(source,item.file));
      assert.equal(bytes.length,item.bytes); assert.equal(digest(bytes),item.sha256);
      await writeFile(path.join(bundle,item.file),bytes,{flag:'wx'});
      const sidecar=item.file==='sbom.spdx.json'?`sbom-${target}.spdx.json`:item.file==='THIRD_PARTY_NOTICES.txt'?`THIRD_PARTY_NOTICES-${target}.txt`:`dependency-inventory-${target}.json`;
      await writeFile(path.join(output,sidecar),bytes,{flag:'wx'});
    }
    await writeFile(path.join(bundle, 'build-manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
    const archive = path.join(output, `devbox-${target}.${extension ? 'zip' : 'tar.gz'}`);
    const names = [...manifest.binaries.map(binary => binary.name + extension), 'build-manifest.json', ...manifest.assurance.files.map(x=>x.file),...optionalNames];
    if (extension) await run('zip', ['-j', archive, ...names.map(name => path.join(bundle, name))]);
    else await run('tar', ['-czf', archive, '-C', bundle, ...names]);
    const extracted = path.join(scratch, `${target}-extracted`); await mkdir(extracted);
    if (extension) await run('unzip', ['-q', archive, '-d', extracted]);
    else await run('tar', ['-xzf', archive, '-C', extracted]);
    for (const name of names) assert.equal(digest(await readFile(path.join(extracted, name))), digest(await readFile(path.join(bundle, name))));
    // Core is a separate distribution of the exact qualified server binary. No
    // installer, TUI, interpreter, browser or desktop package is bundled with it.
    const coreManifest = {...manifest, profile: 'core',optionalTools:[], binaries: manifest.binaries.filter(row => row.name === 'devbox-mcp')};
    const coreDirectory = path.join(scratch, `${target}-core`); await mkdir(coreDirectory);
    const coreNames = ['devbox-mcp' + extension, 'build-manifest.json', 'CORE_SERVER.md', ...manifest.assurance.files.map(row => row.file)];
    for (const name of coreNames.filter(name => !['build-manifest.json', 'CORE_SERVER.md'].includes(name)))
      await copyFile(path.join(bundle, name), path.join(coreDirectory, name));
    // The lean archive has its own inventory and assurance digests. Cargo
    // components belong to the optional executable, not the C++ core binary.
    if(optionalNames.length) {
      const inventory=JSON.parse(await readFile(path.join(coreDirectory,'dependency-inventory.json'),'utf8'));
      const omitted=new Set(inventory.dependencies.flatMap((item,index)=>item.bundledTool?[`SPDXRef-package-${index}`]:[]));
      inventory.dependencies=inventory.dependencies.filter(item=>!item.bundledTool);
      inventory.distributionProfile='core';inventory.omittedOptionalExecutables=['ripgrep'];
      const bom=JSON.parse(await readFile(path.join(coreDirectory,'sbom.spdx.json'),'utf8'));
      bom.packages=bom.packages.filter(item=>!omitted.has(item.SPDXID));
      bom.documentDescribes=bom.documentDescribes.filter(id=>!omitted.has(id));
      bom.relationships=bom.relationships.filter(item=>!omitted.has(item.spdxElementId)&&!omitted.has(item.relatedSpdxElement));
      const notices=(await readFile(path.join(coreDirectory,'THIRD_PARTY_NOTICES.txt'),'utf8')).split('===== Optional ripgrep ')[0];
      const documents={'dependency-inventory.json':JSON.stringify(inventory,null,2)+'\n','sbom.spdx.json':JSON.stringify(bom,null,2)+'\n','THIRD_PARTY_NOTICES.txt':notices};
      coreManifest.assurance={...manifest.assurance,dependencyCount:inventory.dependencies.length,files:[]};
      for(const [file,body] of Object.entries(documents)) {
        await writeFile(path.join(coreDirectory,file),body);
        coreManifest.assurance.files.push({file,bytes:Buffer.byteLength(body),sha256:digest(body)});
      }
    }
    await chmod(path.join(coreDirectory, 'devbox-mcp' + extension), 0o755);
    await writeFile(path.join(coreDirectory, 'build-manifest.json'), JSON.stringify(coreManifest, null, 2) + '\n');
    await copyFile(path.join(repo, 'docs/CORE_SERVER.md'), path.join(coreDirectory, 'CORE_SERVER.md'));
    const coreArchive = path.join(output, `devbox-core-${target}.${extension ? 'zip' : 'tar.gz'}`);
    if (extension) await run('zip', ['-j', coreArchive, ...coreNames.map(name => path.join(coreDirectory, name))]);
    else await run('tar', ['-czf', coreArchive, '-C', coreDirectory, ...coreNames]);
    const coreExtracted = path.join(scratch, `${target}-core-extracted`); await mkdir(coreExtracted);
    if (extension) await run('unzip', ['-q', coreArchive, '-d', coreExtracted]);
    else await run('tar', ['-xzf', coreArchive, '-C', coreExtracted]);
    assert.deepEqual((await readdir(coreExtracted)).sort(), [...coreNames].sort());
    for (const name of coreNames)
      assert.equal(digest(await readFile(path.join(coreExtracted, name))), digest(await readFile(path.join(coreDirectory, name))));
    if(!target.startsWith('android-')&&!target.startsWith('linux-musl-')) {
      const directory=path.join(source,'offline');
      const offline=JSON.parse(await readFile(path.join(directory,'offline-manifest.json'),'utf8'));
      const receipt=JSON.parse(await readFile(path.join(directory,'offline-test-result.json'),'utf8'));
      assert.equal(offline.schema,1);assert.equal(offline.source,expectedSha);assert.equal(offline.tree,expectedTree);
      assert.equal(offline.target,target);assert.equal(offline.version,version);
      assert.equal(offline.payload.source,expectedSha);assert.equal(offline.payload.tree,expectedTree);
      assert.equal(offline.payload.target,target);
      assert.equal(receipt.ok,true);assert.equal(receipt.source,expectedSha);assert.equal(receipt.target,target);
      assert.equal(receipt.policy,'bundled-qualified');assert.deepEqual(receipt.files,offline.files);
      const mac=target.startsWith('macos-');
      const expectedFiles=['OFFLINE.md',...(mac?
        ['Devbox.app/Contents/MacOS/devbox-offline','Devbox.app/Contents/Resources/devbox.payload','Devbox.app/Contents/Info.plist']:
        ['devbox-offline'+(extension||'.run')])];
      assert.deepEqual(offline.files.map(row=>row.file).sort(),expectedFiles.sort());
      assert.equal(offline.installer,mac?'Devbox.app/Contents/MacOS/devbox-offline':'devbox-offline'+(extension||'.run'));
      for(const binary of manifest.binaries) {
        const rows=offline.payload.files.filter(row=>row.name===binary.name+extension);
        assert.equal(rows.length,1);assert.equal(rows[0].sha256,binary.sha256);assert.equal(rows[0].bytes,binary.bytes);
      }
      for(const tool of manifest.optionalTools) {
        const row=offline.payload.files.find(item=>item.name===path.basename(tool.file));
        assert(row&&row.sha256===tool.sha256&&row.bytes===tool.bytes);
      }
      assert(offline.payload.files.every(item=>!/^(?:node|python(?:\d+(?:\.\d+)?)?|docker)(?:\.exe)?$/iu.test(item.name)),
        'Language interpreters and Docker are not release payloads');
      const offlineDirectory=path.join(scratch,target+'-offline');await mkdir(offlineDirectory);
      for(const item of offline.files) {
        const bytes=await readFile(path.join(directory,item.file));
        assert.equal(bytes.length,item.bytes);assert.equal(digest(bytes),item.sha256);
        if(item.file===offline.installer) {
          assertArchitecture(bytes,target);
          const imports=runtimeImports(bytes);assertCoreImports(imports);assert.deepEqual(imports,offline.runtime);
        }
        const destination=path.join(offlineDirectory,item.file);
        await mkdir(path.dirname(destination),{recursive:true});
        await writeFile(destination,bytes,{flag:'wx',mode:item.file===offline.installer?0o755:0o644});
      }
      await writeFile(path.join(offlineDirectory,'offline-manifest.json'),JSON.stringify(offline,null,2)+'\n');
      const sidecar=`offline-manifest-${target}.json`;
      await writeFile(path.join(output,sidecar),JSON.stringify({...offline,qualification:receipt},null,2)+'\n',{flag:'wx'});
      ++offlineAssets;
      if(!mac) {
        await copyFile(path.join(offlineDirectory,offline.installer),path.join(output,`devbox-offline-${target}${extension||'.run'}`));
        ++offlineAssets;
      }
      const offlineArchive=path.join(output,`devbox-offline-${target}.${mac?'app.zip':extension?'zip':'tar.gz'}`);
      const offlineNames=[...(mac?['Devbox.app']:['devbox-offline'+(extension||'.run')]),'OFFLINE.md','offline-manifest.json'];
      if(mac||extension) {
        const args=['-q','-r',offlineArchive,...offlineNames];
        await runCheckedProcess('zip',args,{cwd:offlineDirectory,timeoutMs:120000,label:'Offline archive packaging'});
      } else await run('tar',['-czf',offlineArchive,'-C',offlineDirectory,...offlineNames]);
      ++offlineAssets;
      const roundtrip=path.join(scratch,target+'-offline-extracted');await mkdir(roundtrip);
      if(mac||extension)await run('unzip',['-q',offlineArchive,'-d',roundtrip]);
      else await run('tar',['-xzf',offlineArchive,'-C',roundtrip]);
      for(const item of offline.files)
        assert.equal(digest(await readFile(path.join(roundtrip,item.file))),item.sha256);
      assert.deepEqual(JSON.parse(await readFile(path.join(roundtrip,'offline-manifest.json'),'utf8')),offline);
      // Reuse the already-verified directory without copying all payload bytes.
      await rename(offlineDirectory,path.join(universalDirectory,target));
      offlineBundles.push(offline);
    }
    manifests.push(manifest);
  }
  assert.deepEqual(offlineBundles.map(row=>row.target).sort(),
    ['windows-x86_64','linux-x86_64','linux-aarch64','macos-x86_64','macos-aarch64'].sort());
  const universalManifest = {
    schema:1,source:expectedSha,tree:expectedTree,version,
    targets:offlineBundles.map(row=>({
      target:row.target,installer:`${row.target}/${row.installer}`,
      manifest:`${row.target}/offline-manifest.json`,
      manifestSha256:digest(Buffer.from(JSON.stringify(row,null,2)+'\n'))
    }))
  };
  const universalText=JSON.stringify(universalManifest,null,2)+'\n';
  await writeFile(path.join(universalDirectory,'offline-manifest-universal.json'),universalText,{flag:'wx'});
  await writeFile(path.join(output,'offline-manifest-universal.json'),universalText,{flag:'wx'});
  for(const name of ['OFFLINE.md','CORE_SERVER.md'])
    await copyFile(path.join(repo,'docs',name),path.join(universalDirectory,name));
  await writeFile(path.join(universalDirectory,'README.md'),
    '# Devbox native offline desktop bundle\n\n'+
    'Choose the directory matching your operating system and CPU architecture. '+
    'It contains the same native C++23 offline installer as the individual release download.\n\n'+
    'Windows: run `windows-x86_64/devbox-offline.exe --user` from a normal terminal. '+
    'Linux: run the matching `devbox-offline.run --user`. '+
    'macOS: use `Devbox.app/Contents/MacOS/devbox-offline --user` in the matching directory.\n\n'+
    'Use `--portable PATH --no-start` for a new user-owned installation. '+
    'See [offline installation](OFFLINE.md) for configuration, trust and filesystem requirements. '+
    'Verify the release checksum and GitHub attestation before transferring this archive offline. '+
    'This package contains five target-specific installers; each executable runs only on its matching OS and CPU.\n',
    {flag:'wx'});
  const universalArchive=path.join(output,'devbox-offline-universal.zip');
  await runCheckedProcess('zip',['-1','-q','-r',universalArchive,'.'],
    {cwd:universalDirectory,timeoutMs:120000,label:'Universal offline desktop packaging'});
  const universalRoundtrip=path.join(scratch,'universal-extracted');
  await mkdir(universalRoundtrip);
  await run('unzip',['-q',universalArchive,'-d',universalRoundtrip]);
  assert.deepEqual((await readdir(universalRoundtrip)).sort(),
    [...offlineBundles.map(row=>row.target),'README.md','OFFLINE.md','CORE_SERVER.md','offline-manifest-universal.json'].sort());
  assert.deepEqual(JSON.parse(await readFile(path.join(universalRoundtrip,'offline-manifest-universal.json'),'utf8')),
    universalManifest);
  for(const row of offlineBundles) {
    const restored=path.join(universalRoundtrip,row.target);
    assert.equal(digest(await readFile(path.join(restored,'offline-manifest.json'))),
      universalManifest.targets.find(item=>item.target===row.target).manifestSha256);
    for(const item of row.files)
      assert.equal(digest(await readFile(path.join(restored,item.file))),item.sha256);
  }
  offlineAssets+=2;
  await writeFile(path.join(output, 'build-provenance.json'), `${JSON.stringify({ schema: 1, implementation: 'cpp',
    gitSha: expectedSha, sourceTree: expectedTree, version, dependencyBaseline: baseline, targets: manifests }, null, 2)}\n`);
  const inventories=await Promise.all(Object.keys(targets).map(target=>readFile(path.join(output,`dependency-inventory-${target}.json`),'utf8').then(JSON.parse)));
  const vulnerabilities=await vulnerabilityReport(inventories);
  await writeFile(path.join(output,'vulnerability-report.json'),JSON.stringify(vulnerabilities,null,2)+'\n',{flag:'wx'});
  assert.equal(vulnerabilities.blockingFindings.length,0,'Affected or unresolved vulnerability findings block promotion; remediate and rerun');
  const receipt={schema:1,sourceSha:expectedSha,sourceTree:expectedTree,workflowRunId:process.env.GITHUB_RUN_ID||null,
    qualification:'complete_required_workflow_dependency_graph',
    requiredJobs:['native','android','termux','distributions','alpine','security'],
    targets:manifests.map(m=>({target:m.target,binaries:m.binaries,assurance:m.assurance})),
    promotionRequires:'verified GitHub workflow attestation and matching source/schema/binary digest'};
  await writeFile(path.join(output,'qualification-receipt.json'),JSON.stringify(receipt,null,2)+'\n',{flag:'wx'});
  const names = (await readdir(output)).sort();
  const sums=[];
  for(const name of names)sums.push(`${await fileDigest(path.join(output,name))}  ${name}\n`);
  await writeFile(path.join(output, 'SHA256SUMS'), sums.join(''));
  assert.equal(names.length, Object.keys(targets).length * 8 + 3 + offlineAssets, 'Every target requires binaries, full/core archives, resolved dependency sidecars and global qualification evidence');
  console.log(JSON.stringify({ ok: true, source: expectedSha, version, targets: Object.keys(targets), verifiedFiles: names.length, output }));
} finally {
  await rm(scratch, { recursive: true, force: true });
}
