import test from "node:test";
import assert from "node:assert/strict";
import path from "node:path";
import os from "node:os";
import { chmod, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import { runCheckedProcess, resolveMcpImplementation } from "../src/mcp-implementation.js";
import { getCppMcpBinaryPath, readCppSourceIdentity, prepareCppImplementation, promoteCppImplementation } from "../src/cpp-implementation.js";

const fixture = async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), "devbox-cpp-preflight-"));
  const git = async (...args) => runCheckedProcess("git", args, { cwd: root, label: "C++ fixture Git" });
  await git("init", "--quiet");
  await git("config", "user.name", "Devbox C++ fixture");
  await git("config", "user.email", "fixture@example.invalid");
  await writeFile(path.join(root, ".gitignore"), "run/\nbin/native/\n.cpp-build/\n");
  await git("add", ".gitignore");
  await git("commit", "--quiet", "-m", "Fixture source");
  const source = await readCppSourceIdentity(root, { runProcess: runCheckedProcess });
  const binary = getCppMcpBinaryPath(root);
  await mkdir(path.dirname(binary), { recursive: true });
  await writeFile(binary, "native candidate fixture");
  if (process.platform !== "win32") await chmod(binary, 0o755);
  const hash = createHash("sha256").update(await readFile(binary)).digest("hex");
  const info = { implementation: "cpp", sanitizers: false, sourceDirty: false, gitSha: source.GitSha, sourceTree: source.SourceTree, sourceFingerprint: "a".repeat(64), binarySha256: hash };
  const report = { implementation: "cpp", complete: true, cutover_allowed: true, implemented_tools: 45, target_tools: 45 };
  const calls = [];
  const runner = async (file, args, options) => {
    calls.push({ file, args });
    if (args[0] === "--build-info") return { stdout: JSON.stringify(info), exitCode: 0 };
    if (args[0] === "--parity-report") return { stdout: JSON.stringify(report), exitCode: 0 };
    return runCheckedProcess(file, args, options);
  };
  return { root, git, source, binary, hash, info, report, calls, runner, close: () => rm(root, { recursive: true, force: true, maxRetries: 20, retryDelay: 50 }) };
};

test("C++ preflight stages a verified immutable executable and promotes only after startup", async () => {
  const f = await fixture();
  try {
    assert.equal(resolveMcpImplementation({ DEVBOX_MCP_IMPLEMENTATION: " Cpp " }), "cpp");
    const spec = await prepareCppImplementation(f.root, { runProcess: f.runner, build: async () => { throw new Error("Unexpected rebuild"); } });
    assert.equal(spec.implementation, "cpp");
    assert.match(path.basename(spec.file), /^devbox-cpp-mcp-[a-f0-9]{12}-[a-f0-9]{16}(?:\.exe)?$/u);
    assert.equal(await readFile(spec.file, "utf8"), "native candidate fixture");
    assert.equal(spec.env.DEVBOX_PROJECT_ROOT, f.root);
    assert.equal(spec.env.DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE, "1");
    await assert.rejects(readFile(spec.candidate.CandidateManifestPath), { code: "ENOENT" });
    await promoteCppImplementation(spec, 12345);
    const current = JSON.parse(await readFile(spec.candidate.CandidateManifestPath, "utf8"));
    assert.equal(current.Sha256, f.hash);
    assert.equal(current.ProcessId, 12345);
    const second = await prepareCppImplementation(f.root, { runProcess: f.runner, build: async () => { throw new Error("Unexpected rebuild"); } });
    assert.equal(second.file, spec.file);
    assert.equal(second.candidate.Reused, true);
    await promoteCppImplementation(second, 12346);
    const restarted = JSON.parse(await readFile(spec.candidate.CandidateManifestPath, "utf8"));
    assert.equal(restarted.FirstPromotedAtUtc, current.FirstPromotedAtUtc);
    assert.equal(restarted.ProcessId, 12346);
    assert(!f.calls.some((call) => /cargo|rustc/u.test(call.file)));
  } finally { await f.close(); }
});

test("uncertified C++ replacement preserves the existing promotion manifest", async () => {
  const f = await fixture();
  try {
    const manifest = path.join(f.root, "run", "bin", "current-cpp.json");
    await mkdir(path.dirname(manifest), { recursive: true });
    const previous = '{"priorCandidate":"unchanged"}\n';
    await writeFile(manifest, previous);
    f.report.complete = false;
    f.report.cutover_allowed = false;
    await assert.rejects(prepareCppImplementation(f.root, { env: { ...process.env, CPP_MCP_EXE: f.binary }, runProcess: f.runner }), /incomplete or uncertified/u);
    assert.equal(await readFile(manifest, "utf8"), previous);
  } finally { await f.close(); }
});

test("C++ preflight rejects dirty source, mismatched hashes, and a source change during validation", async () => {
  const f = await fixture();
  try {
    const env = { ...process.env, CPP_MCP_EXE: f.binary };
    await writeFile(path.join(f.root, "untracked.cpp"), "// not committed\n");
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /clean source checkout/u);
    await rm(path.join(f.root, "untracked.cpp"));
    f.info.binarySha256 = "f".repeat(64);
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /provenance did not match/u);
    f.info.binarySha256 = f.hash;
    f.info.sourceDirty = true;
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /provenance did not match/u);
    f.info.sourceDirty = false;
    f.info.sanitizers = true;
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /Instrumented C\+\+ test builds/u);
    delete f.info.sanitizers;
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: f.runner }), /provenance did not match/u);
    f.info.sanitizers = false;
    const runner = async (...args) => {
      const result = await f.runner(...args);
      if (args[1][0] === "--parity-report") {
        await writeFile(path.join(f.root, "new-source.txt"), "changed");
        await f.git("add", "new-source.txt");
        await f.git("commit", "--quiet", "-m", "Concurrent source change");
      }
      return result;
    };
    await assert.rejects(prepareCppImplementation(f.root, { env, runProcess: runner }), /Source changed during C\+\+ preflight/u);
  } finally { await f.close(); }
});

test("C++ promotion refuses an executable modified after preflight", async () => {
  const f = await fixture();
  try {
    const spec = await prepareCppImplementation(f.root, { runProcess: f.runner });
    await writeFile(spec.file, "tampered staged executable");
    await assert.rejects(promoteCppImplementation(spec, 1), /changed before promotion/u);
    await assert.rejects(readFile(spec.candidate.CandidateManifestPath), { code: "ENOENT" });
  } finally { await f.close(); }
});
