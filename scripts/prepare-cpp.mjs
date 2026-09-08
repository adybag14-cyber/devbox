import path from "node:path";
import { fileURLToPath } from "node:url";
import { prepareCppImplementation } from "../src/cpp-implementation.js";
import { runCheckedProcess } from "../src/mcp-implementation.js";
const args = process.argv.slice(2);
let root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
for (let i = 0; i < args.length; i += 1) {
  if (args[i] === "--root" && args[i + 1]) root = path.resolve(args[++i]);
  else throw new Error(`Unknown or incomplete argument: ${args[i]}`);
}
try {
  const spec = await prepareCppImplementation(root, { runProcess: runCheckedProcess });
  process.stdout.write(`${JSON.stringify(spec.candidate)}\n`);
} catch (error) { process.stderr.write(`${error.stack ?? error}\n`); process.exitCode = 1; }
