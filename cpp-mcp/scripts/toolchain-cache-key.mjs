import {createHash} from 'node:crypto';
import {mkdir,readFile,writeFile,appendFile} from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {runCheckedProcess} from '../../src/mcp-implementation.js';
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const probe=path.join(repo,'.cpp-build/toolchain-identity');await mkdir(probe,{recursive:true});
await writeFile(path.join(probe,'CMakeLists.txt'),[
  'cmake_minimum_required(VERSION 3.24)',
  'project(devbox_dependency_toolchain_identity LANGUAGES CXX)',
  'file(WRITE "${CMAKE_BINARY_DIR}/compiler.txt"',
  '"compiler=${CMAKE_CXX_COMPILER}\nversion=${CMAKE_CXX_COMPILER_VERSION}\nid=${CMAKE_CXX_COMPILER_ID}\nfrontend=${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}\ngenerator=${CMAKE_GENERATOR}\nplatform=${CMAKE_GENERATOR_PLATFORM}\ntoolset=${CMAKE_VS_PLATFORM_TOOLSET_VERSION}\nsdk=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}\nosx_sysroot=${CMAKE_OSX_SYSROOT}\nsystem=${CMAKE_SYSTEM_NAME}\nsystem_version=${CMAKE_SYSTEM_VERSION}\nprocessor=${CMAKE_SYSTEM_PROCESSOR}\nflags=${CMAKE_CXX_FLAGS}\n")'
].join('\n'));
const abi=process.env.DEVBOX_CACHE_ANDROID_ABI;
const configure=['-S',probe,'-B',path.join(probe,'build')];
if(abi) {
  if(!['arm64-v8a','armeabi-v7a','x86','x86_64'].includes(abi)) throw new Error('Unknown Android ABI');
  configure.push('-G','Ninja','-DCMAKE_SYSTEM_NAME=Android','-DCMAKE_SYSTEM_VERSION=21',
    `-DCMAKE_ANDROID_ARCH_ABI=${abi}`,`-DCMAKE_ANDROID_NDK=${process.env.ANDROID_NDK_HOME}`,
    `-DANDROID_ABI=${abi}`,'-DANDROID_PLATFORM=android-21','-DANDROID_STL=c++_static');
}
await runCheckedProcess('cmake',configure,{cwd:repo,timeoutMs:120000,label:'Dependency cache compiler/SDK identity'});
const compiler=await readFile(path.join(probe,'build/compiler.txt'),'utf8');
const manifest=await readFile(path.join(repo,'vcpkg.json'),'utf8');
// Changes to our sanitizer triplet must invalidate the outer archive cache as
// well as vcpkg's per-package ABI validation.
const securityTriplet=await readFile(path.join(repo,'cpp-mcp/triplets/x64-windows-static-asan.cmake'),'utf8');
const cmake=(await runCheckedProcess('cmake',['--version'],{timeoutMs:10000,label:'CMake identity'})).stdout.split('\n')[0];
let sdk=abi?await readFile(path.join(process.env.ANDROID_NDK_HOME,'source.properties'),'utf8'):'';
if(process.platform==='darwin') sdk=(await runCheckedProcess('xcrun',['--show-sdk-version'],{timeoutMs:10000,label:'Apple SDK identity'})).stdout.trim();
const identity={schema:1,compiler,cmake,sdk,platform:process.platform,arch:process.arch,
  configuration:process.env.DEVBOX_CACHE_CONFIGURATION||'Release',
  instrumentation:process.env.DEVBOX_CACHE_INSTRUMENTATION||'none',
  cFlags:process.env.CFLAGS||'',cxxFlags:process.env.CXXFLAGS||'',
  triplet:process.env.DEVBOX_CACHE_TRIPLET||`${process.arch==='arm64'?'arm64':'x64'}-${process.platform==='win32'?'windows-static':process.platform==='darwin'?'osx':'linux'}`,
  runtime:abi?'c++_static':process.platform==='win32'?'MultiThreaded':'platform',cxxStandard:23,manifest,securityTriplet,
  buildDefinitions:await Promise.all(['CMakeLists.txt','cpp-mcp/CMakeLists.txt','cpp-mcp/scripts/build-android.mjs',
    'cpp-mcp/scripts/ci-native.mjs'].map(file=>readFile(path.join(repo,file),'utf8')))};
const bytes=JSON.stringify(identity);const key=createHash('sha256').update(bytes).digest('hex');
await writeFile(path.join(probe,'identity.json'),JSON.stringify({...identity,key},null,2)+'\n');
if(process.env.GITHUB_OUTPUT) await appendFile(process.env.GITHUB_OUTPUT,`key=${key}\n`);
console.log(JSON.stringify({key,triplet:identity.triplet,compilerIdentityRecorded:true}));
