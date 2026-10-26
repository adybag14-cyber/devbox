import assert from 'node:assert/strict';
import {link,mkdtemp,mkdir,readFile,realpath,rm,writeFile} from 'node:fs/promises';
import path from 'node:path';
import {test} from 'node:test';
import {applyRipgrepFixtures,fixtureHash} from './ripgrep-fixtures.mjs';

const base=path.resolve('.cpp-build/ripgrep-fixture-tests');
await mkdir(base,{recursive:true});
const source='e89fff89ac9af12e8d4ce9d5fd07beb408ca730f';
async function fixture(body) {
  const root=await mkdtemp(path.join(base,'case-'));
  try {
    await mkdir(path.join(root,'tests'));
    const names=['tests/util.rs','tests/misc.rs','tests/feature.rs'];
    const before='fixture before\n',after='fixture after\n';
    const spec={schema:1,scope:'test-fixtures-only',upstreamSource:source,
      files:names.map(name=>({path:name,beforeSha256:fixtureHash(before),afterSha256:fixtureHash(after),
        replacements:[{before:'before',after:'after'}]}))};
    for(const name of names)await writeFile(path.join(root,name),before);
    await body({root,names,before,after,spec,bytes:()=>Buffer.from(JSON.stringify(spec))});
  } finally {
    const resolved=await realpath(root),parent=await realpath(base);
    assert.equal(path.dirname(resolved),parent);assert(path.basename(resolved).startsWith('case-'));
    await rm(resolved,{recursive:true,force:false});
  }
}
test('applies only the pinned fixtures and restores exact original bytes',()=>fixture(async f=>{
  await writeFile(path.join(f.root,'runtime.rs'),'runtime is not a fixture\n');
  const applied=await applyRipgrepFixtures(f.root,source,f.bytes());
  for(const name of f.names)assert.equal(await readFile(path.join(f.root,name),'utf8'),f.after);
  assert.equal(await readFile(path.join(f.root,'runtime.rs'),'utf8'),'runtime is not a fixture\n');
  assert.equal(applied.metadata.descriptorSha256,fixtureHash(f.bytes()));
  await applied.restore();
  for(const name of f.names)assert.equal(await readFile(path.join(f.root,name),'utf8'),f.before);
}));
test('preserves Windows line endings and the original bytes on restore',()=>fixture(async f=>{
  for(const name of f.names)await writeFile(path.join(f.root,name),f.before.replaceAll('\n','\r\n'));
  const applied=await applyRipgrepFixtures(f.root,source,f.bytes());
  for(const name of f.names)assert.equal(await readFile(path.join(f.root,name),'utf8'),f.after.replaceAll('\n','\r\n'));
  await applied.restore();
  for(const name of f.names)assert.equal(await readFile(path.join(f.root,name),'utf8'),f.before.replaceAll('\n','\r\n'));
}));
test('rejects source drift before editing any fixture',()=>fixture(async f=>{
  await writeFile(path.join(f.root,f.names[2]),'unrelated edit\n');
  await assert.rejects(applyRipgrepFixtures(f.root,source,f.bytes()),/Upstream fixture changed/u);
  assert.equal(await readFile(path.join(f.root,f.names[0]),'utf8'),f.before);
  assert.equal(await readFile(path.join(f.root,f.names[2]),'utf8'),'unrelated edit\n');
}));
test('refuses paths outside the exact reviewed test fixture set',()=>fixture(async f=>{
  f.spec.files[0].path='../runtime.rs';
  await assert.rejects(applyRipgrepFixtures(f.root,source,f.bytes()));
  assert.equal(await readFile(path.join(f.root,f.names[0]),'utf8'),f.before);
}));
test('does not overwrite an unrelated change during restoration',()=>fixture(async f=>{
  const applied=await applyRipgrepFixtures(f.root,source,f.bytes());
  await writeFile(path.join(f.root,f.names[2]),'external edit\n');
  await assert.rejects(applied.restore(),/Fixture changed during qualification/u);
  assert.equal(await readFile(path.join(f.root,f.names[0]),'utf8'),f.after);
  assert.equal(await readFile(path.join(f.root,f.names[2]),'utf8'),'external edit\n');
}));
test('rejects a mismatched upstream pin and incorrect result digest',()=>fixture(async f=>{
  await assert.rejects(applyRipgrepFixtures(f.root,'0'.repeat(40),f.bytes()));
  f.spec.files[1].afterSha256='0'.repeat(64);
  await assert.rejects(applyRipgrepFixtures(f.root,source,f.bytes()),/Fixture result differs/u);
  assert.equal(await readFile(path.join(f.root,f.names[0]),'utf8'),f.before);
}));
test('refuses a hard-linked fixture before changing another path through it',()=>fixture(async f=>{
  const other=path.join(f.root,'unrelated.rs');
  await link(path.join(f.root,f.names[1]),other);
  await assert.rejects(applyRipgrepFixtures(f.root,source,f.bytes()),/unaliased regular fixture/u);
  assert.equal(await readFile(other,'utf8'),f.before);
  assert.equal(await readFile(path.join(f.root,f.names[0]),'utf8'),f.before);
}));
