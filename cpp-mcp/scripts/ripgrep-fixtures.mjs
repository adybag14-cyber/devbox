// Build-time test fixtures only. No runtime source or dependency is changed.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {lstat,readFile,realpath,writeFile} from 'node:fs/promises';
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
