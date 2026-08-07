import os from "node:os";
import path from "node:path";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";

import { config } from "./config.js";
import { HostCommandError, assertHostExecEnabled } from "./host-tools.js";
import { captureCommand, finalizeImageCapture, findExecutable, getPosixProcessTreePids, isPngBuffer } from "./screen-capture-utils.js";

const windowQuerySwift = String.raw`
import Foundation
import CoreGraphics

func emit(_ value: [String: Any]) {
    let data = try! JSONSerialization.data(withJSONObject: value, options: [])
    FileHandle.standardOutput.write(data)
}

func rectValue(_ rect: CGRect) -> [String: Any] {
    return [
        "left": Int(floor(rect.minX)),
        "top": Int(floor(rect.minY)),
        "width": Int(ceil(rect.width)),
        "height": Int(ceil(rect.height))
    ]
}

let args = CommandLine.arguments
let mode = args.count > 1 ? args[1] : ""
if mode == "display" {
    var count: UInt32 = 0
    CGGetActiveDisplayList(0, nil, &count)
    if count == 0 { fputs("No active macOS displays.\n", stderr); exit(2) }
    var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
    CGGetActiveDisplayList(count, &ids, &count)
    var union = CGRect.null
    for id in ids.prefix(Int(count)) { union = union.union(CGDisplayBounds(id)) }
    var result = rectValue(union)
    result["display_count"] = Int(count)
    emit(result)
    exit(0)
}

let pids = Set((args.count > 2 ? args[2] : "").split(separator: ",").compactMap { Int($0) })
let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
let windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] ?? []
var best: [String: Any]? = nil
var bestArea: Double = 0
for info in windows {
    guard let ownerPid = info[kCGWindowOwnerPID as String] as? Int, pids.contains(ownerPid) else { continue }
    let layer = info[kCGWindowLayer as String] as? Int ?? -1
    let alpha = info[kCGWindowAlpha as String] as? Double ?? 1.0
    guard layer == 0, alpha > 0 else { continue }
    guard let boundsValues = info[kCGWindowBounds as String] as? [String: Any],
          let rect = CGRect(dictionaryRepresentation: boundsValues as CFDictionary),
          rect.width >= 32, rect.height >= 32 else { continue }
    let area = rect.width * rect.height
    guard area > bestArea else { continue }
    bestArea = area
    var candidate = rectValue(rect)
    candidate["window_id"] = info[kCGWindowNumber as String] as? Int ?? 0
    candidate["window_owner_pid"] = ownerPid
    candidate["window_owner_name"] = info[kCGWindowOwnerName as String] as? String ?? ""
    candidate["window_title"] = info[kCGWindowName as String] as? String ?? ""
    best = candidate
}

guard let selected = best else {
    fputs("No on-screen CoreGraphics window matched the requested process tree.\n", stderr)
    exit(3)
}
emit(selected)
`;

const assertMacOS = () => {
  assertHostExecEnabled();
  if (!config.platform.isMacOS || process.platform !== "darwin") {
    throw new HostCommandError("macOS screen capture is available only on a macOS host.");
  }
};

const macPermissionHint = (error) =>
  new HostCommandError(
    "macOS screen capture failed. Grant Screen Recording permission to the terminal/Node host in System Settings > Privacy & Security > Screen Recording, then restart that host process. " +
      (error instanceof Error ? error.message : String(error)),
    { exitCode: error?.exitCode, stdout: error?.stdout, stderr: error?.stderr },
  );

const runWindowQuery = async ({ mode, pids = [], tempDir, timeoutMs }) => {
  const swift = await findExecutable(["swift", "/usr/bin/swift"]);
  if (!swift) {
    if (mode === "display") return null;
    throw new HostCommandError(
      "macOS PID-selected window capture requires the Swift tool from Xcode Command Line Tools. Install it with: xcode-select --install",
    );
  }
  const scriptPath = path.join(tempDir, "devbox-window-query.swift");
  await writeFile(scriptPath, windowQuerySwift, "utf8");
  const result = await captureCommand(swift, [scriptPath, mode, pids.join(",")], { cwd: tempDir, timeoutMs });
  try {
    return JSON.parse(result.stdout);
  } catch {
    throw new HostCommandError("macOS window discovery returned invalid metadata.", {
      stdout: result.stdout,
      stderr: result.stderr,
    });
  }
};

const readPngCapture = async (pngPath, metadata) => {
  const image = await readFile(pngPath);
  if (!isPngBuffer(image)) {
    throw new HostCommandError("macOS screencapture did not produce a valid PNG image.");
  }
  return finalizeImageCapture({ image, mimeType: "image/png", metadata });
};

export const captureMacOSDisplay = async ({ quality = 85, timeoutMs = 30000 } = {}) => {
  assertMacOS();
  const screencapture = await findExecutable(["screencapture", "/usr/sbin/screencapture", "/usr/bin/screencapture"]);
  if (!screencapture) throw new HostCommandError("macOS /usr/sbin/screencapture was not found.");
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "devbox-macos-capture-"));
  const pngPath = path.join(tempDir, "display.png");
  try {
    const bounds = await runWindowQuery({ mode: "display", tempDir, timeoutMs }).catch(() => null);
    const args = ["-x"];
    if (bounds?.width > 0 && bounds?.height > 0) {
      args.push(`-R${bounds.left},${bounds.top},${bounds.width},${bounds.height}`);
    }
    args.push(pngPath);
    try {
      await captureCommand(screencapture, args, { cwd: tempDir, timeoutMs });
    } catch (error) {
      throw macPermissionHint(error);
    }
    return await readPngCapture(pngPath, {
      capture_mode: "full_display",
      capture_method: bounds ? "screencapture(CoreGraphics-virtual-bounds)" : "screencapture(default)",
      display_count: bounds?.display_count ?? null,
      left: bounds?.left ?? null,
      top: bounds?.top ?? null,
      requested_quality: quality,
      lossless_png: true,
    });
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
};

export const captureMacOSProgram = async ({ pid, quality = 85, timeoutMs = 30000, includeProcessTree = true } = {}) => {
  assertMacOS();
  if (!Number.isInteger(pid) || pid <= 0) throw new HostCommandError("pid must be a positive macOS process ID.");
  const screencapture = await findExecutable(["screencapture", "/usr/sbin/screencapture", "/usr/bin/screencapture"]);
  if (!screencapture) throw new HostCommandError("macOS /usr/sbin/screencapture was not found.");
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "devbox-macos-window-capture-"));
  const pngPath = path.join(tempDir, "window.png");
  try {
    const pids = includeProcessTree ? await getPosixProcessTreePids(pid) : [pid];
    const window = await runWindowQuery({ mode: "window", pids, tempDir, timeoutMs });
    let method = "screencapture(window-id)";
    try {
      await captureCommand(screencapture, ["-x", "-o", "-l", String(window.window_id), pngPath], { cwd: tempDir, timeoutMs });
    } catch (windowError) {
      // Region capture goes through the visible compositor and is useful for
      // accelerated/video surfaces if a direct window snapshot is unavailable.
      try {
        await captureCommand(
          screencapture,
          ["-x", `-R${window.left},${window.top},${window.width},${window.height}`, pngPath],
          { cwd: tempDir, timeoutMs },
        );
        method = "screencapture(visible-window-bounds-fallback)";
      } catch {
        throw macPermissionHint(windowError);
      }
    }
    return await readPngCapture(pngPath, {
      capture_mode: "program_pid",
      pid,
      window_owner_pid: window.window_owner_pid,
      process_tree_fallback: window.window_owner_pid !== pid,
      candidate_pid_count: pids.length,
      window_id: window.window_id,
      window_title: window.window_title,
      process_name: window.window_owner_name,
      capture_method: method,
      left: window.left,
      top: window.top,
      requested_quality: quality,
      lossless_png: true,
      screen_fallback_may_include_occluders: method.includes("fallback"),
    });
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
};

export { windowQuerySwift as macOSWindowQuerySwift };
