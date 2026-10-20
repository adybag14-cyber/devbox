import test from "node:test";
import assert from "node:assert/strict";

import {
  collectProcessTreePids,
  getPngDimensions,
  isPngBuffer,
  parsePsProcessTable,
} from "../src/screen-capture-utils.js";
import {
  detectLinuxDisplaySession,
  parseHyprlandWindows,
  parseSwayWindows,
  parseWmctrlWindows,
  parseXwininfoGeometry,
  selectLargestWindow,
} from "../src/linux-screen-capture.js";
import { macOSWindowQuerySwift } from "../src/macos-screen-capture.js";
import { resolveScreenCaptureBackend } from "../src/screen-capture.js";
import { windowsProcessTreeScript } from "../src/windows-screen-capture.js";
import { runCheckedProcess } from "../src/mcp-implementation.js";

const pngHeader = (width, height) => {
  const buffer = Buffer.alloc(24);
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]).copy(buffer, 0);
  buffer.write("IHDR", 12, "ascii");
  buffer.writeUInt32BE(width, 16);
  buffer.writeUInt32BE(height, 20);
  return buffer;
};
test("Windows capture excludes older processes attached to a recycled parent PID", {skip:process.platform!=="win32"}, async()=>{
  const script=String.raw`
$ErrorActionPreference='Stop'
function Get-CimInstance {
  @(
    [pscustomobject]@{ProcessId=100;ParentProcessId=1;CreationDate=[datetime]'2026-10-20T12:00:00Z'},
    [pscustomobject]@{ProcessId=200;ParentProcessId=100;CreationDate=[datetime]'2026-10-20T10:00:00Z'},
    [pscustomobject]@{ProcessId=201;ParentProcessId=200;CreationDate=[datetime]'2026-10-20T11:00:00Z'},
    [pscustomobject]@{ProcessId=300;ParentProcessId=100;CreationDate=[datetime]'2026-10-20T12:00:01Z'},
    [pscustomobject]@{ProcessId=301;ParentProcessId=300;CreationDate=[datetime]'2026-10-20T12:00:02Z'},
    [pscustomobject]@{ProcessId=400;ParentProcessId=100;CreationDate=$null}
  )
}
${windowsProcessTreeScript}
ConvertTo-Json -Compress -InputObject @(Get-ProcessTreePids -RootPid 100 -IncludeTree $true | Sort-Object)
`;
  const result=await runCheckedProcess("powershell.exe",["-NoLogo","-NoProfile","-NonInteractive","-EncodedCommand",Buffer.from(script,"utf16le").toString("base64")],
    {timeoutMs:15000,maxCaptureChars:4096,label:"Windows capture lifetime fixture"});
  assert.deepEqual(JSON.parse(result.stdout),[100,300,301]);
});

test("PNG validation extracts IHDR dimensions without an image dependency", () => {
  const png = pngHeader(1920, 1080);
  assert.equal(isPngBuffer(png), true);
  assert.deepEqual(getPngDimensions(png), { width: 1920, height: 1080 });
  assert.equal(isPngBuffer(Buffer.from("not png")), false);
});

test("POSIX process-tree collection includes descendants but not siblings", () => {
  const rows = parsePsProcessTable(" 10 1\n 11 10\n 12 11\n 20 1\n");
  assert.deepEqual(collectProcessTreePids(rows, 10), [10, 11, 12]);
});

test("screen capture backend follows the detected host platform", () => {
  assert.equal(resolveScreenCaptureBackend({ isWindows: true }), "windows");
  assert.equal(resolveScreenCaptureBackend({ isMacOS: true }), "macos");
  assert.equal(resolveScreenCaptureBackend({ isLinux: true, isTermux: false }), "linux");
  assert.equal(resolveScreenCaptureBackend({ isLinux: true, isTermux: true }), "termux-unsupported");
});

test("Linux session detection distinguishes Wayland, X11 and headless", () => {
  assert.deepEqual(detectLinuxDisplaySession({ WAYLAND_DISPLAY: "wayland-0" }), {
    wayland: true,
    x11: false,
    sessionType: "wayland",
  });
  assert.deepEqual(detectLinuxDisplaySession({ DISPLAY: ":0" }), {
    wayland: false,
    x11: true,
    sessionType: "x11",
  });
  assert.equal(detectLinuxDisplaySession({}).sessionType, "headless");
});

test("wmctrl parser and selector choose the largest child-process window", () => {
  const windows = parseWmctrlWindows([
    "0x01  0 100 10 20 200 100 host Parent",
    "0x02  0 101 30 40 1280 720 host GPU Child",
    "0x03  0 999 0 0 1920 1080 host Unrelated",
  ].join("\n"));
  const selected = selectLargestWindow(windows, [100, 101]);
  assert.equal(selected.id, "0x02");
  assert.equal(selected.pid, 101);
  assert.equal(selected.title, "GPU Child");
});

test("xwininfo geometry accepts negative multi-monitor coordinates", () => {
  assert.deepEqual(
    parseXwininfoGeometry("Absolute upper-left X:  -1920\nAbsolute upper-left Y:  25\nWidth: 1920\nHeight: 1080\n"),
    { left: -1920, top: 25, width: 1920, height: 1080 },
  );
});

test("Sway and Hyprland parsers resolve compositor-native PID windows", () => {
  const sway = parseSwayWindows(JSON.stringify({
    nodes: [{ pid: 42, name: "Emulator", visible: true, rect: { x: 4, y: 5, width: 800, height: 600 }, nodes: [], floating_nodes: [] }],
    floating_nodes: [],
  }), [42]);
  assert.equal(sway[0].backend, "sway");
  assert.equal(sway[0].width, 800);

  const hypr = parseHyprlandWindows(JSON.stringify([
    { pid: 42, address: "0xabc", mapped: true, hidden: false, at: [8, 9], size: [900, 700], title: "Emulator" },
  ]), [42]);
  assert.equal(hypr[0].backend, "hyprland");
  assert.equal(hypr[0].id, "0xabc");
});

test("macOS window discovery uses CoreGraphics compositor metadata", () => {
  assert.match(macOSWindowQuerySwift, /CGWindowListCopyWindowInfo/);
  assert.match(macOSWindowQuerySwift, /CGGetActiveDisplayList/);
  assert.match(macOSWindowQuerySwift, /optionOnScreenOnly/);
});
