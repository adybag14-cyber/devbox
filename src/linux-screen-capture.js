import os from "node:os";
import path from "node:path";
import { mkdtemp, readFile, rm } from "node:fs/promises";

import { config } from "./config.js";
import { HostCommandError, assertHostExecEnabled } from "./host-tools.js";
import { captureCommand, finalizeImageCapture, findExecutable, getPosixProcessTreePids, isPngBuffer } from "./screen-capture-utils.js";

export const detectLinuxDisplaySession = (env = process.env) => {
  const sessionType = String(env.XDG_SESSION_TYPE ?? "").trim().toLowerCase();
  const wayland = Boolean(env.WAYLAND_DISPLAY) || sessionType === "wayland";
  const x11 = Boolean(env.DISPLAY) || sessionType === "x11";
  return { wayland, x11, sessionType: wayland ? "wayland" : x11 ? "x11" : sessionType || "headless" };
};

export const parseWmctrlWindows = (text) => {
  const windows = [];
  for (const line of String(text ?? "").split(/\r?\n/u)) {
    const match = line.match(/^\s*(0x[0-9a-f]+)\s+(-?\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+\S+\s*(.*)$/iu);
    if (!match) continue;
    windows.push({
      id: match[1],
      desktop: Number(match[2]),
      pid: Number(match[3]),
      left: Number(match[4]),
      top: Number(match[5]),
      width: Number(match[6]),
      height: Number(match[7]),
      title: match[8] ?? "",
    });
  }
  return windows;
};

export const parseXwininfoGeometry = (text) => {
  const read = (pattern) => Number(String(text).match(pattern)?.[1]);
  const left = read(/Absolute upper-left X:\s*(-?\d+)/u);
  const top = read(/Absolute upper-left Y:\s*(-?\d+)/u);
  const width = read(/Width:\s*(\d+)/u);
  const height = read(/Height:\s*(\d+)/u);
  if (![left, top, width, height].every(Number.isFinite)) return null;
  return { left, top, width, height };
};

export const selectLargestWindow = (windows, candidatePids) => {
  const pids = new Set(candidatePids.map(Number));
  return windows
    .filter((window) => pids.has(Number(window.pid)) && window.width >= 32 && window.height >= 32)
    .sort((a, b) => b.width * b.height - a.width * a.height)[0] ?? null;
};

const walkSwayTree = (node, candidatePids, output = []) => {
  if (!node || typeof node !== "object") return output;
  if (candidatePids.has(Number(node.pid)) && node.rect?.width >= 32 && node.rect?.height >= 32 && node.visible !== false) {
    output.push({
      id: node.id,
      pid: Number(node.pid),
      left: Number(node.rect.x),
      top: Number(node.rect.y),
      width: Number(node.rect.width),
      height: Number(node.rect.height),
      title: node.name ?? "",
      backend: "sway",
    });
  }
  for (const child of [...(node.nodes ?? []), ...(node.floating_nodes ?? [])]) walkSwayTree(child, candidatePids, output);
  return output;
};

export const parseSwayWindows = (text, candidatePids) => {
  const root = JSON.parse(text);
  return walkSwayTree(root, new Set(candidatePids.map(Number)));
};

export const parseHyprlandWindows = (text, candidatePids) => {
  const pids = new Set(candidatePids.map(Number));
  const clients = JSON.parse(text);
  return clients
    .filter((client) => pids.has(Number(client.pid)) && client.mapped !== false && client.hidden !== true)
    .map((client) => ({
      id: client.address,
      pid: Number(client.pid),
      left: Number(client.at?.[0] ?? 0),
      top: Number(client.at?.[1] ?? 0),
      width: Number(client.size?.[0] ?? 0),
      height: Number(client.size?.[1] ?? 0),
      title: client.title ?? "",
      backend: "hyprland",
    }));
};

const assertLinux = () => {
  assertHostExecEnabled();
  if (!config.platform.isLinux || config.platform.isTermux || process.platform !== "linux") {
    throw new HostCommandError("Linux desktop capture is available only on a non-Termux Linux host.");
  }
};

const noDisplayError = (session) =>
  new HostCommandError(
    `No capturable Linux graphical session was detected (session=${session.sessionType}). Set DISPLAY for X11 or WAYLAND_DISPLAY for Wayland and run Devbox inside the logged-in desktop session.`,
  );

const missingDisplayBackendError = (session, cause = null) => {
  const baseMessage = session.wayland
    ? "No supported Wayland screenshot backend was found. Install grim (wlroots), gnome-screenshot (GNOME), or spectacle (KDE)."
    : "No supported X11 screenshot backend was found. Install maim, scrot, gnome-screenshot, spectacle, or ImageMagick's import command.";
  if (!cause) return new HostCommandError(baseMessage);
  const causeMessage = cause instanceof Error ? cause.message : String(cause);
  return new HostCommandError(`${baseMessage} Installed backend failure: ${causeMessage}`, {
    exitCode: cause?.exitCode,
    stdout: cause?.stdout,
    stderr: cause?.stderr,
  });
};

const capturePng = async (file, args, pngPath, { cwd, timeoutMs }) => {
  await captureCommand(file, args, { cwd, timeoutMs });
  const image = await readFile(pngPath);
  if (!isPngBuffer(image)) throw new HostCommandError(`${path.basename(file)} did not produce a valid PNG image.`);
  return image;
};

const captureLinuxFullPng = async ({ session, pngPath, tempDir, timeoutMs }) => {
  const candidates = session.wayland
    ? [
        ["grim", (file) => [file], "grim(full-display)"],
        ["gnome-screenshot", (file) => ["-f", file], "gnome-screenshot(full-display)"],
        ["spectacle", (file) => ["-b", "-n", "-f", "-o", file], "spectacle(full-display)"],
      ]
    : [
        ["maim", (file) => [file], "maim(root)"],
        ["scrot", (file) => [file], "scrot(root)"],
        ["gnome-screenshot", (file) => ["-f", file], "gnome-screenshot(full-display)"],
        ["spectacle", (file) => ["-b", "-n", "-f", "-o", file], "spectacle(full-display)"],
        ["import", (file) => ["-window", "root", file], "ImageMagick-import(root)"],
      ];

  let lastError = null;
  for (const [name, argsFor, method] of candidates) {
    const executable = await findExecutable([name]);
    if (!executable) continue;
    try {
      return { image: await capturePng(executable, argsFor(pngPath), pngPath, { cwd: tempDir, timeoutMs }), method };
    } catch (error) {
      // Try another compositor-compatible backend before surfacing failure.
      lastError = error;
    }
  }
  throw missingDisplayBackendError(session, lastError);
};

const discoverX11Window = async ({ candidatePids, timeoutMs }) => {
  const wmctrl = await findExecutable(["wmctrl"]);
  if (wmctrl) {
    try {
      const result = await captureCommand(wmctrl, ["-lpG"], { timeoutMs });
      const selected = selectLargestWindow(parseWmctrlWindows(result.stdout), candidatePids);
      if (selected) return { ...selected, discovery: "wmctrl" };
    } catch {
      // Continue through the remaining X11 discovery backends.
    }
  }

  const xdotool = await findExecutable(["xdotool"]);
  const xwininfo = await findExecutable(["xwininfo"]);
  if (xdotool && xwininfo) {
    const windows = [];
    for (const pid of candidatePids) {
      let ids;
      try {
        ids = (await captureCommand(xdotool, ["search", "--onlyvisible", "--pid", String(pid)], { timeoutMs })).stdout
          .split(/\r?\n/u)
          .map((value) => value.trim())
          .filter(Boolean);
      } catch {
        ids = [];
      }
      for (const id of ids) {
        try {
          const info = await captureCommand(xwininfo, ["-id", id], { timeoutMs });
          const geometry = parseXwininfoGeometry(info.stdout);
          if (geometry) windows.push({ id, pid, title: "", ...geometry });
        } catch {
        }
      }
    }
    const selected = selectLargestWindow(windows, candidatePids);
    if (selected) return { ...selected, discovery: "xdotool+xwininfo" };
  }

  const xprop = await findExecutable(["xprop"]);
  if (xprop && xwininfo) {
    let ids = [];
    try {
      const root = await captureCommand(xprop, ["-root", "_NET_CLIENT_LIST_STACKING"], { timeoutMs });
      ids = root.stdout.match(/0x[0-9a-f]+/giu) ?? [];
    } catch {
      ids = [];
    }
    const windows = [];
    for (const id of ids) {
      try {
        const props = await captureCommand(xprop, ["-id", id, "_NET_WM_PID", "_NET_WM_NAME", "WM_NAME"], { timeoutMs });
        const pid = Number(props.stdout.match(/_NET_WM_PID\([^)]*\)\s*=\s*(\d+)/u)?.[1]);
        if (!candidatePids.includes(pid)) continue;
        const info = await captureCommand(xwininfo, ["-id", id], { timeoutMs });
        const geometry = parseXwininfoGeometry(info.stdout);
        if (!geometry) continue;
        const title = props.stdout.match(/(?:_NET_WM_NAME|WM_NAME)\([^)]*\)\s*=\s*"([^"]*)"/u)?.[1] ?? "";
        windows.push({ id, pid, title, ...geometry });
      } catch {
      }
    }
    const selected = selectLargestWindow(windows, candidatePids);
    if (selected) return { ...selected, discovery: "xprop+xwininfo" };
  }

  return null;
};

const discoverWaylandWindow = async ({ candidatePids, timeoutMs }) => {
  const swaymsg = await findExecutable(["swaymsg"]);
  if (swaymsg) {
    try {
      const result = await captureCommand(swaymsg, ["-t", "get_tree", "-r"], { timeoutMs });
      const selected = selectLargestWindow(parseSwayWindows(result.stdout, candidatePids), candidatePids);
      if (selected) return { ...selected, discovery: "swaymsg" };
    } catch {
    }
  }
  const hyprctl = await findExecutable(["hyprctl"]);
  if (hyprctl) {
    try {
      const result = await captureCommand(hyprctl, ["clients", "-j"], { timeoutMs });
      const selected = selectLargestWindow(parseHyprlandWindows(result.stdout, candidatePids), candidatePids);
      if (selected) return { ...selected, discovery: "hyprctl" };
    } catch {
    }
  }
  return null;
};

const captureLinuxWindowPng = async ({ window, session, pngPath, tempDir, timeoutMs }) => {
  if (session.wayland && (window.backend === "sway" || window.backend === "hyprland")) {
    const grim = await findExecutable(["grim"]);
    if (grim) {
      const geometry = `${window.left},${window.top} ${window.width}x${window.height}`;
      return {
        image: await capturePng(grim, ["-g", geometry, pngPath], pngPath, { cwd: tempDir, timeoutMs }),
        method: `grim(${window.backend}-window-bounds)`,
        visibleRegionFallback: true,
      };
    }
  }

  // XWayland windows can still use these X11 paths inside a Wayland session,
  // but native compositor node/address IDs are not X11 window IDs.
  const compositorWindow = window.backend === "sway" || window.backend === "hyprland";
  const maim = await findExecutable(["maim"]);
  if (maim && window.id && !compositorWindow) {
    try {
      return {
        image: await capturePng(maim, ["-i", String(window.id), pngPath], pngPath, { cwd: tempDir, timeoutMs }),
        method: "maim(window-id)",
        visibleRegionFallback: false,
      };
    } catch {
    }
  }
  const importTool = await findExecutable(["import"]);
  if (importTool && window.id && !compositorWindow) {
    try {
      return {
        image: await capturePng(importTool, ["-window", String(window.id), pngPath], pngPath, { cwd: tempDir, timeoutMs }),
        method: "ImageMagick-import(window-id)",
        visibleRegionFallback: false,
      };
    } catch {
    }
  }

  if (session.wayland) {
    throw new HostCommandError(
      "This Wayland compositor does not expose a non-interactive PID-selected window screenshot path. Devbox supports wlroots/Sway/Hyprland via grim and XWayland windows via X11 tools; GNOME/KDE may require a user-approved xdg-desktop-portal screenshot instead.",
    );
  }
  throw new HostCommandError("The window was found on X11, but no window-capable capture backend is installed. Install maim or ImageMagick.");
};

export const captureLinuxDisplay = async ({ quality = 85, timeoutMs = 30000 } = {}) => {
  assertLinux();
  const session = detectLinuxDisplaySession();
  if (!session.wayland && !session.x11) throw noDisplayError(session);
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "devbox-linux-capture-"));
  const pngPath = path.join(tempDir, "display.png");
  try {
    const capture = await captureLinuxFullPng({ session, pngPath, tempDir, timeoutMs });
    return finalizeImageCapture({
      image: capture.image,
      mimeType: "image/png",
      metadata: {
        capture_mode: "full_display",
        capture_method: capture.method,
        display_server: session.sessionType,
        requested_quality: quality,
        lossless_png: true,
      },
    });
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
};

export const captureLinuxProgram = async ({ pid, quality = 85, timeoutMs = 30000, includeProcessTree = true } = {}) => {
  assertLinux();
  if (!Number.isInteger(pid) || pid <= 0) throw new HostCommandError("pid must be a positive Linux process ID.");
  const session = detectLinuxDisplaySession();
  if (!session.wayland && !session.x11) throw noDisplayError(session);
  const candidatePids = includeProcessTree ? await getPosixProcessTreePids(pid) : [pid];
  let window = null;
  if (session.wayland) window = await discoverWaylandWindow({ candidatePids, timeoutMs });
  if (!window && session.x11) window = await discoverX11Window({ candidatePids, timeoutMs });
  if (!window) {
    if (session.wayland && !session.x11) {
      throw new HostCommandError(
        "No PID-selected window could be discovered on this Wayland compositor. Sway and Hyprland are supported directly; other compositors may intentionally hide window/PID enumeration and require an interactive desktop portal.",
      );
    }
    throw new HostCommandError(`No visible X11/XWayland window was found for PID ${pid} or its child processes.`);
  }

  const tempDir = await mkdtemp(path.join(os.tmpdir(), "devbox-linux-window-capture-"));
  const pngPath = path.join(tempDir, "window.png");
  try {
    const capture = await captureLinuxWindowPng({ window, session, pngPath, tempDir, timeoutMs });
    return finalizeImageCapture({
      image: capture.image,
      mimeType: "image/png",
      metadata: {
        capture_mode: "program_pid",
        pid,
        window_owner_pid: window.pid,
        process_tree_fallback: window.pid !== pid,
        candidate_pid_count: candidatePids.length,
        window_id: window.id,
        window_title: window.title,
        capture_method: capture.method,
        window_discovery: window.discovery,
        display_server: session.sessionType,
        left: window.left,
        top: window.top,
        width: window.width,
        height: window.height,
        requested_quality: quality,
        lossless_png: true,
        screen_fallback_may_include_occluders: capture.visibleRegionFallback,
      },
    });
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
};
