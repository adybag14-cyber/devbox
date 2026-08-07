import assert from "node:assert/strict";
import os from "node:os";
import path from "node:path";
import { chmod, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { spawn } from "node:child_process";
import { once } from "node:events";

const run = async (file, args) => {
  const child = spawn(file, args, { stdio: ["ignore", "pipe", "pipe"], env: process.env });
  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (chunk) => { stdout += chunk; });
  child.stderr.on("data", (chunk) => { stderr += chunk; });
  const [code] = await once(child, "exit");
  if (code !== 0) throw new Error(`${file} ${args.join(" ")} failed (${code}): ${stderr || stdout}`);
  return { stdout, stderr };
};

const makePngHeader = (width, height) => {
  const buffer = Buffer.alloc(24);
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]).copy(buffer, 0);
  buffer.write("IHDR", 12, "ascii");
  buffer.writeUInt32BE(width, 16);
  buffer.writeUInt32BE(height, 20);
  return buffer;
};

const writeExecutable = async (file, content) => {
  await writeFile(file, content, "utf8");
  await chmod(file, 0o755);
};

const tmp = await mkdtemp(path.join(os.tmpdir(), "devbox-screen-ci-"));
const bin = path.join(tmp, "bin");
const fixture = path.join(tmp, "fixture.png");
await import("node:fs/promises").then(({ mkdir }) => mkdir(bin, { recursive: true }));
await writeFile(fixture, makePngHeader(640, 480));
process.env.ENABLE_HOST_EXEC = "true";
process.env.DEVBOX_CAPTURE_FIXTURE = fixture;
process.env.DEVBOX_TEST_PID = String(process.pid);
const originalPath = process.env.PATH ?? "";
process.env.PATH = `${bin}${path.delimiter}${originalPath}`;

const copyLastArgScript = `#!/bin/sh\nfor last do :; done\ncp "$DEVBOX_CAPTURE_FIXTURE" "$last"\n`;

try {
  if (process.platform === "linux") {
    await writeExecutable(path.join(bin, "grim"), copyLastArgScript);
    await writeExecutable(
      path.join(bin, "swaymsg"),
      `#!/bin/sh\nprintf '{"pid":%s,"name":"CI Wayland Window","visible":true,"rect":{"x":10,"y":20,"width":640,"height":480},"nodes":[],"floating_nodes":[]}\\n' "$DEVBOX_TEST_PID"\n`,
    );
    await writeExecutable(path.join(bin, "maim"), copyLastArgScript);
    await writeExecutable(
      path.join(bin, "wmctrl"),
      `#!/bin/sh\nprintf '0x01200003  0 %s 30 40 640 480 ci-host CI X11 Window\\n' "$DEVBOX_TEST_PID"\n`,
    );

    const { captureLinuxDisplay, captureLinuxProgram } = await import("../../src/linux-screen-capture.js");

    process.env.WAYLAND_DISPLAY = "wayland-ci";
    process.env.XDG_SESSION_TYPE = "wayland";
    delete process.env.DISPLAY;
    const waylandDisplay = await captureLinuxDisplay({ timeoutMs: 5000 });
    assert.equal(waylandDisplay.mimeType, "image/png");
    assert.equal(waylandDisplay.metadata.capture_method, "grim(full-display)");
    assert.equal(waylandDisplay.metadata.width, 640);
    const waylandWindow = await captureLinuxProgram({ pid: process.pid, includeProcessTree: false, timeoutMs: 5000 });
    assert.equal(waylandWindow.metadata.window_discovery, "swaymsg");
    assert.match(waylandWindow.metadata.capture_method, /^grim\(sway-window-bounds\)$/u);

    delete process.env.WAYLAND_DISPLAY;
    process.env.DISPLAY = ":99";
    process.env.XDG_SESSION_TYPE = "x11";
    const x11Display = await captureLinuxDisplay({ timeoutMs: 5000 });
    assert.equal(x11Display.metadata.capture_method, "maim(root)");
    const x11Window = await captureLinuxProgram({ pid: process.pid, includeProcessTree: false, timeoutMs: 5000 });
    assert.equal(x11Window.metadata.window_discovery, "wmctrl");
    assert.equal(x11Window.metadata.capture_method, "maim(window-id)");
    assert.equal(x11Window.metadata.window_title, "CI X11 Window");

    // A broken preferred X11 discovery utility must not prevent later fallbacks.
    await writeExecutable(path.join(bin, "wmctrl"), "#!/bin/sh\nexit 2\n");
    await writeExecutable(path.join(bin, "xdotool"), "#!/bin/sh\nprintf '18874371\\n'\n");
    await writeExecutable(
      path.join(bin, "xwininfo"),
      "#!/bin/sh\nprintf '  Absolute upper-left X: 30\\n  Absolute upper-left Y: 40\\n  Width: 640\\n  Height: 480\\n'\n",
    );
    const x11FallbackWindow = await captureLinuxProgram({ pid: process.pid, includeProcessTree: false, timeoutMs: 5000 });
    assert.equal(x11FallbackWindow.metadata.window_discovery, "xdotool+xwininfo");

    // Native compositor node IDs are not X11 window IDs. When grim is absent,
    // do not hand a Sway/Hyprland ID to maim/import and risk the wrong window.
    const constrainedPath = process.env.PATH;
    const maimMarker = path.join(tmp, "maim-invoked");
    await rm(path.join(bin, "grim"), { force: true });
    await writeExecutable(
      path.join(bin, "maim"),
      `#!/bin/sh\nprintf called > "$DEVBOX_MAIM_MARKER"\nexit 7\n`,
    );
    process.env.DEVBOX_MAIM_MARKER = maimMarker;
    process.env.PATH = bin;
    delete process.env.DISPLAY;
    process.env.WAYLAND_DISPLAY = "wayland-ci";
    process.env.XDG_SESSION_TYPE = "wayland";
    await assert.rejects(
      captureLinuxProgram({ pid: process.pid, includeProcessTree: false, timeoutMs: 5000 }),
      /Wayland compositor does not expose/u,
    );
    let maimWasInvoked = false;
    try { await readFile(maimMarker); maimWasInvoked = true; } catch {}
    assert.equal(maimWasInvoked, false);

    // If an installed display backend itself fails, preserve that cause instead
    // of incorrectly claiming that no backend is installed.
    await writeExecutable(path.join(bin, "grim"), "#!/bin/sh\nprintf 'grim-denied' >&2\nexit 7\n");
    await assert.rejects(
      captureLinuxDisplay({ timeoutMs: 5000 }),
      /grim-denied/u,
    );
    process.env.PATH = constrainedPath;
    delete process.env.DEVBOX_MAIM_MARKER;

    console.log("Linux screen capture backend E2E passed for fake Wayland and X11 compositor paths.");
  } else if (process.platform === "darwin") {
    const { captureMacOSDisplay, captureMacOSProgram, macOSWindowQuerySwift } = await import("../../src/macos-screen-capture.js");
    const swiftSource = path.join(tmp, "window-query.swift");
    await writeFile(swiftSource, macOSWindowQuerySwift, "utf8");
    await run("/usr/bin/swiftc", ["-typecheck", swiftSource]);

    await writeExecutable(
      path.join(bin, "swift"),
      `#!/bin/sh\nif [ "$2" = display ]; then\n  printf '{"left":0,"top":0,"width":640,"height":480,"display_count":1}'\nelse\n  printf '{"left":20,"top":30,"width":640,"height":480,"window_id":4242,"window_owner_pid":%s,"window_owner_name":"CI App","window_title":"CI Window"}' "$DEVBOX_TEST_PID"\nfi\n`,
    );
    await writeExecutable(path.join(bin, "screencapture"), copyLastArgScript);

    const display = await captureMacOSDisplay({ timeoutMs: 5000 });
    assert.equal(display.mimeType, "image/png");
    assert.equal(display.metadata.capture_method, "screencapture(CoreGraphics-virtual-bounds)");
    assert.equal(display.metadata.width, 640);
    const window = await captureMacOSProgram({ pid: process.pid, includeProcessTree: false, timeoutMs: 5000 });
    assert.equal(window.metadata.window_id, 4242);
    assert.equal(window.metadata.window_title, "CI Window");
    assert.equal(window.metadata.capture_method, "screencapture(window-id)");

    console.log("macOS screen capture backend E2E passed; CoreGraphics Swift helper typechecked and capture wiring executed.");
  } else {
    console.log(`Screen capture platform E2E skipped on ${process.platform}.`);
  }
} finally {
  process.env.PATH = originalPath;
  await rm(tmp, { recursive: true, force: true });
}
