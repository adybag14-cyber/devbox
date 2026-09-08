import path from "node:path";
import { access, copyFile, mkdir, readFile, rename, rm, writeFile } from "node:fs/promises";
import { constants } from "node:fs";
import { createHash, randomUUID } from "node:crypto";

const executableName = (platform) => platform === "win32" ? "devbox-mcp.exe" : "devbox-mcp";
export const getCppMcpBinaryPath = (root, platform = process.platform) => path.join(root, "bin", "native", executableName(platform));
const exists = async (file) => { try { await access(file, constants.R_OK); return true; } catch { return false; } };
const digest = async (file) => createHash("sha256").update(await readFile(file)).digest("hex");
const run = (runProcess, file, args, root, env, label, timeoutMs = 30000) => runProcess(file, args, { cwd: root, env, label, timeoutMs });

export const readCppSourceIdentity = async (root, { env = process.env, runProcess }) => {
  const git = String(env.GIT_EXE ?? "").trim() || "git";
  const sha = (await run(runProcess, git, ["rev-parse", "--verify", "HEAD"], root, env, "C++ source commit")).stdout.trim();
  const tree = (await run(runProcess, git, ["rev-parse", "HEAD^{tree}"], root, env, "C++ source tree")).stdout.trim();
  const dirty = (await run(runProcess, git, ["status", "--porcelain", "--untracked-files=all"], root, env, "C++ source cleanliness")).stdout.trim();
  if (!/^[0-9a-f]{40}$/u.test(sha) || !/^[0-9a-f]{40}$/u.test(tree) || dirty) {
    throw new Error("C++ MCP preflight requires a committed, clean source checkout with no untracked build inputs. The existing MCP was not stopped.");
  }
  return { GitSha: sha, SourceTree: tree, SourceDirty: false };
};

export const matchesCppSource = (info, source) => info?.implementation === "cpp"
  && info.sanitizers === false && info.sourceDirty === false && info.gitSha === source.GitSha && info.sourceTree === source.SourceTree
  && /^[0-9a-f]{64}$/u.test(String(info.sourceFingerprint ?? ""));

export const buildCppImplementation = async (root, { env = process.env, platform = process.platform, runProcess }) => {
  const cmake = String(env.CMAKE_EXE ?? "").trim() || "cmake";
  const git = String(env.GIT_EXE ?? "").trim() || "git";
  const dependencyManifest = JSON.parse(await readFile(path.join(root, "vcpkg.json"), "utf8"));
  const baseline = dependencyManifest["builtin-baseline"];
  if (!/^[0-9a-f]{40}$/u.test(String(baseline))) throw new Error("C++ dependency manifest omitted the pinned vcpkg revision.");
  const vcpkg = path.resolve(String(env.VCPKG_ROOT ?? "").trim() || path.join(root, ".cpp-build", "vcpkg"));
  const toolchain = path.join(vcpkg, "scripts", "buildsystems", "vcpkg.cmake");
  if (!await exists(toolchain)) {
    if (await exists(vcpkg)) throw new Error(`The configured vcpkg directory exists but is incomplete: ${vcpkg}`);
    await mkdir(path.dirname(vcpkg), { recursive: true });
    await run(runProcess, git, ["init", vcpkg], root, env, "Initialize private vcpkg checkout");
    await run(runProcess, git, ["-C", vcpkg, "remote", "add", "origin", "https://github.com/microsoft/vcpkg.git"], root, env, "Configure vcpkg source");
    await run(runProcess, git, ["-C", vcpkg, "fetch", "--depth", "1", "origin", baseline], root, env, "Fetch pinned vcpkg source", 300000);
    await run(runProcess, git, ["-C", vcpkg, "checkout", "--detach", "FETCH_HEAD"], root, env, "Select pinned vcpkg source");
    if (platform === "win32") {
      const shell = String(env.POWERSHELL_EXE ?? "").trim() || "powershell.exe";
      await run(runProcess, shell, ["-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", path.join(root, "scripts", "Bootstrap-DevboxVcpkg.ps1"), "-VcpkgRoot", vcpkg], root, env, "Bootstrap pinned vcpkg", 300000);
    } else {
      await run(runProcess, "sh", [path.join(vcpkg, "bootstrap-vcpkg.sh"), "-disableMetrics"], root, env, "Bootstrap pinned vcpkg", 300000);
    }
  }
  const generation = `${Date.now()}-${randomUUID()}`;
  const build = path.join(root, ".cpp-build", "managed", generation);
  const buildEnv = { ...env, VCPKG_MAX_CONCURRENCY: String(env.VCPKG_MAX_CONCURRENCY || "4") };
  const configure = ["-S", root, "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DDEVBOX_BUILD_TESTS=OFF", "-DDEVBOX_BUILD_TUI=OFF", `-DCMAKE_TOOLCHAIN_FILE=${toolchain}`, `-DVCPKG_INSTALLED_DIR=${path.join(root, ".cpp-build", "vcpkg_installed")}`];
  if (platform === "win32") configure.push("-DVCPKG_TARGET_TRIPLET=x64-windows-static");
  await run(runProcess, cmake, configure, root, buildEnv, "Configure C++ MCP", 90 * 60 * 1000);
  await run(runProcess, cmake, ["--build", build, "--config", "Release", "--target", "devbox-mcp", "--parallel", "4"], root, buildEnv, "Build C++ MCP", 90 * 60 * 1000);
  const name = executableName(platform);
  for (const file of [path.join(build, "cpp-mcp", "Release", name), path.join(build, "cpp-mcp", name)]) {
    if (await exists(file)) return file;
  }
  throw new Error("C++ MCP build completed without the expected native executable.");
};

export const prepareCppImplementation = async (root, {
  env = process.env, platform = process.platform, runProcess, build = buildCppImplementation,
} = {}) => {
  root = path.resolve(root);
  if (typeof runProcess !== "function") throw new Error("C++ preparation requires a bounded process runner.");
  const source = await readCppSourceIdentity(root, { env, runProcess });
  const versioned = path.join(root, "run", "bin");
  const currentManifest = path.join(versioned, "current-cpp.json");
  const candidates = [];
  const explicit = String(env.CPP_MCP_EXE ?? "").trim();
  if (explicit) candidates.push(path.resolve(root, explicit));
  if (!explicit) {
    try {
      const current = JSON.parse(await readFile(currentManifest, "utf8"));
      const file = path.resolve(String(current.FilePath));
      if (path.dirname(file) === versioned && /^[a-f0-9]{64}$/iu.test(String(current.Sha256))
          && await exists(file) && await digest(file) === String(current.Sha256).toLowerCase()) candidates.push(file);
    } catch {}
    candidates.push(getCppMcpBinaryPath(root, platform));
  }
  const childEnv = { ...env, DEVBOX_PROJECT_ROOT: root, DEVBOX_MCP_RUNTIME_ENV_AUTHORITATIVE: "1" };
  const inspect = async (file) => {
    const info = JSON.parse((await run(runProcess, file, ["--build-info"], root, childEnv, "C++ candidate provenance")).stdout);
    if (info.sanitizers === true) throw new Error("Instrumented C++ test builds cannot replace a managed runtime. The existing MCP was not stopped.");
    const hash = await digest(file);
    if (!matchesCppSource(info, source) || info.binarySha256 !== hash) throw new Error("C++ candidate provenance did not match the committed checkout and executable hash.");
    const report = JSON.parse((await run(runProcess, file, ["--parity-report"], root, childEnv, "C++ completion gate")).stdout);
    if (report.implementation !== "cpp" || report.complete !== true || report.cutover_allowed !== true || report.implemented_tools !== 45 || report.target_tools !== 45) {
      throw new Error("C++ replacement is incomplete or uncertified. The existing MCP was not stopped.");
    }
    return { info, hash };
  };
  let selected, inspected, lastError;
  for (const file of candidates) {
    if (!await exists(file)) continue;
    try { inspected = await inspect(file); selected = file; break; }
    catch (error) { lastError = error; if (explicit || /incomplete or uncertified/u.test(error.message)) throw error; }
  }
  if (!selected) {
    if (explicit) throw lastError ?? new Error(`Configured CPP_MCP_EXE does not exist: ${explicit}`);
    selected = await build(root, { env, platform, runProcess });
    inspected = await inspect(selected);
  }
  const after = await readCppSourceIdentity(root, { env, runProcess });
  if (after.GitSha !== source.GitSha || after.SourceTree !== source.SourceTree) throw new Error("Source changed during C++ preflight. The existing MCP was not stopped.");
  await mkdir(versioned, { recursive: true });
  const extension = platform === "win32" ? ".exe" : "";
  const file = path.join(versioned, `devbox-cpp-mcp-${source.GitSha.slice(0, 12)}-${inspected.hash.slice(0, 16)}${extension}`);
  if (!await exists(file)) {
    const staged = path.join(versioned, `.cpp-candidate-${randomUUID()}`);
    try {
      await copyFile(selected, staged, constants.COPYFILE_EXCL);
      if (await digest(staged) !== inspected.hash) throw new Error("C++ candidate staging hash mismatch");
      await rename(staged, file);
    } finally { await rm(staged, { force: true }).catch(() => {}); }
  }
  if (await digest(file) !== inspected.hash) throw new Error("Existing immutable C++ candidate hash mismatch");
  await access(file, constants.X_OK);
  const generation = `${Date.now()}-${randomUUID()}`;
  const candidate = {
    Implementation: "cpp", FilePath: file, ArgumentList: [], Generation: generation,
    ...source, SourceFingerprint: inspected.info.sourceFingerprint, Sha256: inspected.hash,
    CandidateManifestPath: currentManifest, Reused: file === selected,
  };
  return { implementation: "cpp", file, args: [], env: { ...childEnv, DEVBOX_DEPLOYMENT_GENERATION: generation }, candidate };
};

export const promoteCppImplementation = async (spec, pid) => {
  if (spec?.implementation !== "cpp" || !spec.candidate) return;
  const candidate = spec.candidate;
  if (await digest(candidate.FilePath) !== candidate.Sha256) throw new Error("C++ executable changed before promotion.");
  const manifest = candidate.CandidateManifestPath;
  const now = new Date().toISOString();
  let previous;
  try { previous = JSON.parse(await readFile(manifest, "utf8")); } catch {}
  const same = previous?.Sha256 === candidate.Sha256 && previous?.FilePath === candidate.FilePath;
  const value = { ...candidate, ProcessId: pid, StartedAtUtc: now, LastStartedAtUtc: now,
    PromotedAtUtc: same ? previous.PromotedAtUtc ?? now : now,
    FirstPromotedAtUtc: same ? previous.FirstPromotedAtUtc ?? previous.PromotedAtUtc ?? now : now };
  const staged = `${manifest}.${randomUUID()}.tmp`;
  try { await writeFile(staged, `${JSON.stringify(value, null, 2)}\n`, { flag: "wx" }); await rename(staged, manifest); }
  finally { await rm(staged, { force: true }).catch(() => {}); }
};
