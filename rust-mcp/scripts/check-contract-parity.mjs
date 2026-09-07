import assert from "node:assert/strict";
import { readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const rustMcpRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const projectRoot = path.resolve(rustMcpRoot, "..");
const serverSource = await readFile(path.join(projectRoot, "src", "server.js"), "utf8");
const target = JSON.parse(await readFile(path.join(rustMcpRoot, "parity", "target-tools.json"), "utf8"));

const registered = [...serverSource.matchAll(/server\.registerTool\(\s*["']([^"']+)["']/g)]
  .map((match) => match[1]);

assert.equal(registered.length, 37, `Expected the JavaScript MCP to register 37 tools, found ${registered.length}. If the JS contract intentionally changed, update this guard and the Rust parity target together.`);
assert.equal(new Set(registered).size, registered.length, "JavaScript MCP tool names must be unique.");
assert.equal(new Set(target).size, target.length, "Rust target tool names must be unique.");
assert.deepEqual(target, registered, "Rust target-tools.json has drifted from the JavaScript MCP registerTool order/names.");

const nativeSource = await readFile(path.join(rustMcpRoot, "src", "agent_tools.rs"), "utf8");
const native = [...nativeSource.matchAll(/#\[tool\([\s\S]*?name\s*=\s*"([^"]+)"/g)].map(match => match[1]).sort();
assert.equal(native.length, 8, "Native agent tool contract must explicitly contain eight tools");
assert.equal(new Set([...target, ...native]).size, target.length + native.length);
const nativePath = path.join(rustMcpRoot, "parity", "native-tools.json");
if (process.argv.includes("--write-native")) await writeFile(nativePath, JSON.stringify(native, null, 2) + "\n");
assert.deepEqual(JSON.parse(await readFile(nativePath, "utf8")), native, "Run check-contract-parity.mjs --write-native after an intentional native tool change");

console.log(JSON.stringify({
  ok: true,
  toolCount: registered.length,
  tools: registered,
}, null, 2));
