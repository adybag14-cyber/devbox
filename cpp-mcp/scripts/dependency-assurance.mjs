import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mkdir, readFile, writeFile, access, readdir} from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const digest = bytes => createHash('sha256').update(bytes).digest('hex');
export function installedPackages(text, triplet) {
  const packages = new Map();
  for (const paragraph of text.replaceAll('\r\n', '\n').split(/\n\s*\n/u)) {
    const fields = Object.fromEntries(paragraph.split('\n').filter(line => /^[A-Za-z-]+:/u.test(line))
      .map(line => {const colon=line.indexOf(':');return [line.slice(0,colon),line.slice(colon+1).trim()];}));
    if (fields.Architecture !== triplet || fields.Status !== 'install ok installed' || fields.Feature) continue;
    assert.match(fields.Package, /^[a-z0-9][a-z0-9-]*$/u);
    assert(fields.Version && fields.Abi, 'Resolved package version and ABI are required');
    assert(!packages.has(fields.Package), 'One installed base package per target');
    packages.set(fields.Package, fields);
  }
  assert(packages.size > 0 && packages.size <= 512, 'Bounded resolved dependency graph');
  return [...packages.values()].sort((a,b)=>a.Package.localeCompare(b.Package));
}
export async function collectDependencies({installed, triplet, manifest, output, sourceSha}) {
  assert.match(triplet, /^[a-z0-9-]+$/u); assert.match(sourceSha, /^[a-f0-9]{40}$/u);
  const entries = installedPackages(await readFile(path.join(installed,'vcpkg/status'),'utf8'),triplet);
  for (const dep of manifest.dependencies) assert(entries.some(x=>x.Package === (typeof dep==='string'?dep:dep.name)),
    `The resolved graph is missing direct dependency ${typeof dep==='string'?dep:dep.name}`);
  await mkdir(output,{recursive:true});
  const inventory=[];
  const notices=[];
  for (const entry of entries) {
    const root=path.join(installed,triplet,'share',entry.Package);
    const noticePackage=entry.Package;
    let notice, buildSupportOnly=false;
    try { notice=await readFile(path.join(root,'copyright'),'utf8'); }
    catch(error) {
      if(error.code!=='ENOENT' || entry.Package!=='boost-uninstall') throw error;
      const files=(await readFile(path.join(installed,'vcpkg/info',`${entry.Package}_${entry.Version}_${triplet}.list`),'utf8')).trim().split(/\r?\n/u);
      assert(files.every(file=>file===`${triplet}/` || file.startsWith(`${triplet}/share/`)), 'Unlicensed build helper cannot contain shipped code or headers');
      buildSupportOnly=true;
      notice='Build support only: no compiled code or headers from this package are bundled. Its recipe metadata is included in the inventory.';
    }
    assert(notice.trim().length>10 && Buffer.byteLength(notice)<=4*1024*1024, `Missing/bounded notice: ${entry.Package}`);
    const source=await readFile(path.join(root,'vcpkg.spdx.json'),'utf8');
    const spdx=JSON.parse(source);
    const recipe=spdx.packages?.find(p=>p.SPDXID==='SPDXRef-port');
    assert.equal(recipe?.name,entry.Package); assert(recipe.versionInfo, 'Resolved SPDX recipe version required');
    const binary=spdx.packages?.find(p=>p.SPDXID==='SPDXRef-binary');
    assert.equal(binary?.versionInfo,entry.Abi,'Installed status/SPDX ABI identity must match');
    const version=entry.Version;
    const portVersion=entry['Port-Version']||'0';
    assert.equal(recipe.versionInfo,portVersion==='0'?version:`${version}#${portVersion}`, `Installed status/SPDX version mismatch for ${entry.Package}`);
    inventory.push({name:entry.Package,version,portVersion:entry['Port-Version']||'0',abi:entry.Abi,
      license:recipe.licenseConcluded||'NOASSERTION',noticePackage,buildSupportOnly,homepage:recipe.homepage||null,
      source:recipe.downloadLocation,noticeSha256:digest(notice),resolvedSpdxSha256:digest(source),
      buildOnlyMayBeIncluded:true});
    notices.push(`===== ${entry.Package} ${version} (${triplet}) =====\n${notice.trim()}\n`);
  }
  const generatedAt=new Date().toISOString();
  const componentPresence={};
  const installedTarget=path.join(installed,triplet);
  const exists=async file=>{try{await access(file);return true;}catch(error){if(error.code==='ENOENT')return false;throw error;}};
  for(const component of ['graph','regex']) {
    const libraryFiles=[];
    for(const directory of ['lib','debug/lib','bin','debug/bin']) {
      let names;try{names=await readdir(path.join(installedTarget,directory));}catch(error){if(error.code==='ENOENT')continue;throw error;}
      assert(names.length<=16384,'Bounded component library inventory');
      for(const name of names) if(new RegExp(`^(?:lib)?boost_${component}(?:[^a-z]|$)`,'iu').test(name))libraryFiles.push(`${directory}/${name}`);
    }
    componentPresence[`boost-${component}`]={checked:true,
      packagePresent:inventory.some(item=>item.name===`boost-${component}`),
      headersPresent:await exists(path.join(installedTarget,'include/boost',component))||await exists(path.join(installedTarget,'include/boost',component+'.hpp')),
      libraryFiles};
  }
  const bom={spdxVersion:'SPDX-2.3',dataLicense:'CC0-1.0',SPDXID:'SPDXRef-DOCUMENT',
    name:`devbox-${triplet}`,documentNamespace:`https://github.com/adybag14-cyber/devbox/spdx/${sourceSha}/${triplet}`,
    creationInfo:{created:generatedAt,creators:['Tool: Devbox resolved-vcpkg-assurance']},
    documentDescribes:inventory.map((_,i)=>`SPDXRef-package-${i}`),
    packages:inventory.map((item,i)=>({SPDXID:`SPDXRef-package-${i}`,name:item.name,versionInfo:item.version,
      downloadLocation:item.source||'NOASSERTION',filesAnalyzed:false,licenseConcluded:item.license,
      licenseDeclared:'NOASSERTION',copyrightText:'NOASSERTION',
      externalRefs:[{referenceCategory:'PACKAGE-MANAGER',referenceType:'purl',
        referenceLocator:`pkg:vcpkg/${item.name}@${encodeURIComponent(item.version)}?arch=${triplet}`}],
      comment:`Resolved ABI ${item.abi}; notice SHA-256 ${item.noticeSha256}; resolved SPDX SHA-256 ${item.resolvedSpdxSha256}`})),
    relationships:inventory.map((_,i)=>({spdxElementId:'SPDXRef-DOCUMENT',relationshipType:'DESCRIBES',relatedSpdxElement:`SPDXRef-package-${i}`}))};
  const documents={'dependency-inventory.json':JSON.stringify({schema:1,sourceSha,triplet,generatedAt,
    dependencyBaseline:manifest['builtin-baseline'],scope:'resolved_target_graph_including_build_support',componentPresence,dependencies:inventory},null,2)+'\n',
    'sbom.spdx.json':JSON.stringify(bom,null,2)+'\n','THIRD_PARTY_NOTICES.txt':notices.join('\n')};
  const files=[];
  for(const [file,body] of Object.entries(documents)) {
    await writeFile(path.join(output,file),body,{flag:'wx'});files.push({file,sha256:digest(body),bytes:Buffer.byteLength(body)});
  }
  return {schema:1,triplet,dependencyCount:inventory.length,files};
}
if(process.argv[1] && path.resolve(process.argv[1])===fileURLToPath(import.meta.url)) {
  const [installed,triplet,manifestPath,output,sourceSha]=process.argv.slice(2);
  assert(installed&&triplet&&manifestPath&&output&&sourceSha,'Pass installed triplet manifest output sourceSha');
  console.log(JSON.stringify(await collectDependencies({installed:path.resolve(installed),triplet,
    manifest:JSON.parse(await readFile(manifestPath,'utf8')),output:path.resolve(output),sourceSha})));
}
