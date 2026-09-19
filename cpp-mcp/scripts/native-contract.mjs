import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const frozen = JSON.parse(await readFile(new URL('../contract/reference-tools.json', import.meta.url), 'utf8'));
export const computerTools = JSON.parse(await readFile(new URL('../contract/computer-tools.json', import.meta.url), 'utf8'));
export const computerNames = computerTools.map(tool => tool.name).sort();
export const researchTools = JSON.parse(await readFile(new URL('../contract/research-tools.json', import.meta.url), 'utf8'));
export const researchNames = researchTools.map(tool => tool.name).sort();
const extensionNames = [...computerNames, ...researchNames];
const legacyNames = Object.values(frozen.profiles)[0].map(tool => tool.name).sort();
assert.equal(legacyNames.length, 45, 'unchanged frozen reference contract');
assert.equal(computerNames.length, 2, 'explicit C++ computer-use extension');

export function assertCppExtensions(tools) {
  const actual = tools.filter(tool => computerNames.includes(tool.name)).sort((a, b) => a.name.localeCompare(b.name));
  const expected = [...computerTools].sort((a, b) => a.name.localeCompare(b.name));
  assert.deepEqual(actual, expected, 'computer-use schemas, descriptions and security annotations match their source');
  const actualResearch = tools.filter(tool => researchNames.includes(tool.name)).sort((a,b) => a.name.localeCompare(b.name));
  assert.deepEqual(actualResearch, [...researchTools].sort((a,b) => a.name.localeCompare(b.name)), 'research schemas and security annotations match their source');
  return tools.filter(tool => !extensionNames.includes(tool.name));
}

export function assertNativeContract(tools, capabilities) {
  assert(['cpp', 'rust'].includes(capabilities.implementation), 'known native implementation');
  const cpp = capabilities.implementation === 'cpp';
  const expected = [...legacyNames, ...(cpp ? extensionNames : [])].sort();
  assert.deepEqual(tools.map(tool => tool.name).sort(), expected, 'complete native tool inventory');
  assert.deepEqual([...capabilities.tools].sort(), expected, 'capability inventory matches actual tools');
  assert.equal(capabilities.contract_version, cpp ? 4 : 2, 'implementation-specific contract version');
  if (cpp) {
    assertCppExtensions(tools);
    assert.equal(typeof capabilities.computer_use?.supported, 'boolean');
    assert.equal(capabilities.computer_use?.input_capacity, 1);
    assert.equal(capabilities.web_research?.standard_source_target, 100);
    assert.equal(capabilities.web_research?.fast_source_target, 50);
  }
  return expected.length;
}

export async function assertClientNativeContract(client, tools) {
  const listed = tools ?? (await client.listTools()).tools;
  const response = await client.callTool({ name: 'devbox_capabilities', arguments: {} });
  assert.equal(response.isError ?? false, false);
  const capabilities = response.structuredContent?.data;
  assert(capabilities, 'native capability result');
  assertNativeContract(listed, capabilities);
  return capabilities;
}
