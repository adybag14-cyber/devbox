import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
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

console.log(JSON.stringify({
  ok: true,
  toolCount: registered.length,
  tools: registered,
}, null, 2));
