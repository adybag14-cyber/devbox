// Build-time test fixtures only. No runtime source or dependency is changed.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {lstat,mkdir,readFile,realpath,stat,utimes,writeFile} from 'node:fs/promises';
import path from 'node:path';

const allowed=new Set(['tests/util.rs','tests/misc.rs','tests/feature.rs']);
export const fixtureHash=bytes=>createHash('sha256').update(bytes).digest('hex');
const lf=text=>text.replaceAll('\r\n','\n');
export async function applyRipgrepFixtures(source,expectedSource,descriptor) {
  const raw=descriptor??await readFile(new URL('../patches/ripgrep-atime-fixtures.json',import.meta.url));
  assert(Buffer.byteLength(raw)<=128*1024,'Bounded fixture descriptor required');
  const spec=JSON.parse(Buffer.from(raw).toString('utf8'));
  assert.equal(spec.schema,1);assert.equal(spec.scope,'test-fixtures-only');
  assert.equal(spec.upstreamSource,expectedSource);
  assert.equal(spec.files.length,3);assert.deepEqual(new Set(spec.files.map(file=>file.path)),allowed);
  const root=await realpath(source),prepared=[];
  // Validate every original file and every complete replacement before any edit.
  for(const file of spec.files) {
    assert(allowed.has(file.path));
    assert.match(file.beforeSha256,/^[a-f0-9]{64}$/u);assert.match(file.afterSha256,/^[a-f0-9]{64}$/u);
    const target=path.join(root,...file.path.split('/'));
    const info=await lstat(target);
    assert(info.isFile()&&!info.isSymbolicLink()&&info.nlink===1&&info.size<512*1024,'Bounded unaliased regular fixture required');
    assert.equal(await realpath(target),target,'Fixture aliases are refused');
    const original=await readFile(target),text=original.toString('utf8');
    let changed=lf(text);
    assert.equal(fixtureHash(changed),file.beforeSha256,`Upstream fixture changed: ${file.path}`);
    for(const edit of file.replacements) {
      assert(typeof edit.before==='string'&&edit.before.length>0&&typeof edit.after==='string');
      assert.equal(changed.split(edit.before).length,2,`Fixture edit must match once: ${file.path}`);
      changed=changed.replace(edit.before,()=>edit.after);
    }
    assert.equal(fixtureHash(changed),file.afterSha256,`Fixture result differs: ${file.path}`);
    const bytes=Buffer.from(text.includes('\r\n')?changed.replaceAll('\n','\r\n'):changed);
    prepared.push({target,original,bytes,path:file.path,beforeSha256:file.beforeSha256,afterSha256:file.afterSha256});
  }
  const written=[];
  const restore=async()=>{
    // Do not overwrite an edit made outside this qualification attempt.
    for(const file of written)assert.deepEqual(await readFile(file.target),file.bytes,`Fixture changed during qualification: ${file.path}`);
    for(const file of written)await writeFile(file.target,file.original);
  };
  try {
    for(const file of prepared){await writeFile(file.target,file.bytes);written.push(file);}
  } catch(error) {await restore();throw error;}
  return {
    metadata:{schema:1,scope:spec.scope,upstreamSource:spec.upstreamSource,descriptorSha256:fixtureHash(raw),
      files:prepared.map(({path,beforeSha256,afterSha256})=>({path,beforeSha256,afterSha256}))},
    restore,
  };
}

export async function qualifyRipgrepAccessOrder(directory,invoke) {
  await mkdir(directory);await mkdir(path.join(directory,'dir'));
  const names=['a','dir/c','b','dir/d'];
  for(const name of names)await writeFile(path.join(directory,name),'test\n');
  const reports=[];
  // Deliberately disagree with both creation order and pathname order. Check
  // subsecond values too, so whole-second upstream fixtures cannot conceal a
  // regression in the runtime's timestamp comparison.
  for(const [spacingMs,ranks] of [[100,[3,0,2,1]],[2000,[1,3,0,2]]]) {
    for(let i=0;i<names.length;i++)await utimes(path.join(directory,names[i]),
      new Date(1_000_000_000_000+ranks[i]*spacingMs),new Date(1_000_001_000_000));
    const before=await Promise.all(names.map(async name=>({name,atime:(await stat(path.join(directory,name),{bigint:true})).atimeNs})));
    assert.equal(new Set(before.map(row=>row.atime.toString())).size,names.length,'Qualification filesystem must preserve distinct requested access times');
    const sorted=[...before].sort((a,b)=>a.atime<b.atime?-1:a.atime>b.atime?1:0).map(row=>row.name);
    for(const reverse of [false,true]) {
      const expected=reverse?[...sorted].reverse():sorted;
      const output=await invoke(['--no-config','--no-ignore','--path-separator','/',reverse?'--sortr':'--sort','accessed','--files'],directory);
      assert.deepEqual(output.trim().split(/\r?\n/u),expected,'ripgrep must sort by the observed access times');
      const after=await Promise.all(names.map(async name=>(await stat(path.join(directory,name),{bigint:true})).atimeNs));
      assert.deepEqual(after,before.map(row=>row.atime),'Access times changed during the controlled --files probe');
      reports.push({spacingMs,reverse,order:expected,accessTimes:before.map(row=>({name:row.name,nanoseconds:row.atime.toString()}))});
    }
  }
  return reports;
}
