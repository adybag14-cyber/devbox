import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';

// Parse the actual executable rather than relying on the compiler/linker flags.
// No ldd invocation: dependency inspection must not execute an unverified file.
export function runtimeImports(bytes) {
  const imports=[];
  const stringAt=offset=>{assert(Number.isSafeInteger(offset)&&offset>=0&&offset<bytes.length);const end=bytes.indexOf(0,offset);assert(end>=offset&&end-offset<32768);return bytes.toString('utf8',offset,end);};
  if(bytes.subarray(0,4).toString('hex')==='7f454c46') {
    assert.equal(bytes[5],1,'Only qualified little-endian ELF targets are supported');
    const wide=bytes[4]===2; assert(wide||bytes[4]===1);
    const word=offset=>wide?Number(bytes.readBigUInt64LE(offset)):bytes.readUInt32LE(offset);
    const start=word(wide?32:28), size=bytes.readUInt16LE(wide?54:42), count=bytes.readUInt16LE(wide?56:44);
    const loads=[];let dynamic;
    for(let i=0;i<count;i++) {const p=start+i*size,type=bytes.readUInt32LE(p);
      const entry={offset:word(p+(wide?8:4)),address:word(p+(wide?16:8)),size:word(p+(wide?32:16))};
      if(type===1)loads.push(entry);if(type===2)dynamic=entry;
    }
    if(!dynamic)return {format:'elf',imports};
    let strings;const names=[];
    for(let p=dynamic.offset;p<dynamic.offset+dynamic.size;p+=wide?16:8) {
      const tag=word(p),value=word(p+(wide?8:4)); if(tag===0)break;
      if(tag===5)strings=value;if(tag===1)names.push(value);
    }
    if(names.length) {const load=loads.find(row=>strings>=row.address&&strings<row.address+row.size);assert(load,'ELF string table must be file-backed');
      for(const name of names)imports.push(stringAt(load.offset+strings-load.address+name));}
    return {format:'elf',imports};
  }
  if(bytes.subarray(0,2).toString()==='MZ') {
    const pe=bytes.readUInt32LE(60);assert.equal(bytes.readUInt32LE(pe),0x4550);
    const count=bytes.readUInt16LE(pe+6),optional=pe+24,optionalSize=bytes.readUInt16LE(pe+20);
    const magic=bytes.readUInt16LE(optional);assert([0x10b,0x20b].includes(magic));
    const sections=[];
    for(let i=0;i<count;i++){const p=optional+optionalSize+i*40;sections.push({address:bytes.readUInt32LE(p+12),size:bytes.readUInt32LE(p+16),offset:bytes.readUInt32LE(p+20)});}
    const offsetOf=address=>{const section=sections.find(row=>address>=row.address&&address<row.address+row.size);assert(section,'PE import directory must be file-backed');return section.offset+address-section.address;};
    const address=bytes.readUInt32LE(optional+(magic===0x20b?112:96)+8);
    if(address)for(let p=offsetOf(address);bytes.readUInt32LE(p+12);p+=20) imports.push(stringAt(offsetOf(bytes.readUInt32LE(p+12))));
    return {format:'pe',imports};
  }
  assert.equal(bytes.readUInt32LE(0),0xfeedfacf,'Expected a qualified 64-bit Mach-O binary');
  const count=bytes.readUInt32LE(16);let p=32;
  for(let i=0;i<count;i++){const command=bytes.readUInt32LE(p),size=bytes.readUInt32LE(p+4);assert(size>=8);
    if([0xc,0x80000018,0x8000001f,0x80000023].includes(command)) imports.push(stringAt(p+bytes.readUInt32LE(p+8)));p+=size;}
  return {format:'macho',imports};
}
export function assertCoreImports(result,{instrumented=false}={}) {
  if(instrumented)return;
  if(result.format==='elf') {
    const system=/^(?:lib(?:c|m|dl|pthread|rt|log|android)\.so(?:\.\d+)*|libc\.musl-[\w_-]+\.so\.1|ld-linux[\w.-]*\.so(?:\.\d+)*)$/u;
    for(const name of result.imports)assert(system.test(name),`Core requires an unbundled non-system library: ${name}`);
  } else if(result.format==='pe') {
    const system=/^(?:api-ms-win-[\w-]+|ext-ms-win-[\w-]+|kernel32|ntdll|user32|gdi32|advapi32|crypt32|bcrypt|bcryptprimitives|secur32|sspicli|ws2_32|mswsock|iphlpapi|userenv|shell32|ole32|oleaut32|combase|dwmapi|shcore|psapi|dbghelp|windowscodecs|version|winhttp|wininet|normaliz|ncrypt|synchronization|ucrtbase)\.dll$/iu;
    for(const name of result.imports)assert(system.test(name),`Core requires an unbundled non-system DLL: ${name}`);
  } else for(const name of result.imports)assert(name.startsWith('/usr/lib/')||name.startsWith('/System/Library/Frameworks/'),`Core requires an external dylib: ${name}`);
}
export async function inspectRuntimeFile(file,options) {const value=runtimeImports(await readFile(file));assertCoreImports(value,options);return value;}
