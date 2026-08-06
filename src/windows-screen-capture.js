import { createHash } from "node:crypto";
import os from "node:os";
import path from "node:path";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";

import { config } from "./config.js";
import { HostCommandError, assertHostExecEnabled, buildWindowsPowerShellFileArgs, spawnPowerShellProcess } from "./host-tools.js";
import { spawnProcess } from "./process-utils.js";

const JPEG_START = Buffer.from([0xff, 0xd8, 0xff]);
const JPEG_END = Buffer.from([0xff, 0xd9]);

const captureScript = String.raw`
param(
  [Parameter(Mandatory = $true)][ValidateSet('display', 'pid')][string]$Mode,
  [Parameter(Mandatory = $true)][string]$OutputPath,
  [Parameter(Mandatory = $true)][ValidateRange(1, 100)][int]$Quality,
  [int]$TargetPid = 0
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$InformationPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class DevboxCaptureNative {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool SetProcessDPIAware();

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextLength(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint flags);

    public static IntPtr FindLargestVisibleWindow(uint targetPid) {
        IntPtr best = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows((hWnd, lParam) => {
            uint ownerPid;
            GetWindowThreadProcessId(hWnd, out ownerPid);
            if (ownerPid != targetPid || !IsWindowVisible(hWnd) || IsIconic(hWnd)) {
                return true;
            }

            RECT rect;
            if (!GetWindowRect(hWnd, out rect)) {
                return true;
            }

            long width = Math.Max(0, rect.Right - rect.Left);
            long height = Math.Max(0, rect.Bottom - rect.Top);
            long area = width * height;
            if (area > bestArea) {
                best = hWnd;
                bestArea = area;
            }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    public static string ReadWindowTitle(IntPtr hWnd) {
        int length = GetWindowTextLength(hWnd);
        StringBuilder text = new StringBuilder(Math.Max(1, length + 1));
        GetWindowText(hWnd, text, text.Capacity);
        return text.ToString();
    }
}
'@

[DevboxCaptureNative]::SetProcessDPIAware() | Out-Null

function Save-Jpeg {
  param(
    [Parameter(Mandatory = $true)][System.Drawing.Bitmap]$Bitmap,
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][int]$JpegQuality
  )

  $codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
    Where-Object { $_.MimeType -eq 'image/jpeg' } |
    Select-Object -First 1
  if ($null -eq $codec) {
    throw 'The Windows JPEG encoder is unavailable.'
  }

  $encoderParameters = [System.Drawing.Imaging.EncoderParameters]::new(1)
  try {
    $encoderParameters.Param[0] = [System.Drawing.Imaging.EncoderParameter]::new(
      [System.Drawing.Imaging.Encoder]::Quality,
      [long]$JpegQuality
    )
    $Bitmap.Save($Path, $codec, $encoderParameters)
  } finally {
    $encoderParameters.Dispose()
  }
}

$bitmap = $null
$graphics = $null
try {
  if ($Mode -eq 'display') {
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    if ($bounds.Width -le 0 -or $bounds.Height -le 0) {
      throw 'Windows reported an empty virtual display.'
    }

    $bitmap = [System.Drawing.Bitmap]::new($bounds.Width, $bounds.Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bounds.Size, [System.Drawing.CopyPixelOperation]::SourceCopy)
    Save-Jpeg -Bitmap $bitmap -Path $OutputPath -JpegQuality $Quality

    $metadata = @{
      capture_mode = 'full_display'
      left = $bounds.Left
      top = $bounds.Top
      width = $bounds.Width
      height = $bounds.Height
      quality = $Quality
    }
  } else {
    if ($TargetPid -le 0) {
      throw 'TargetPid must be a positive Windows process ID.'
    }

    $process = Get-Process -Id $TargetPid -ErrorAction Stop
    $window = [DevboxCaptureNative]::FindLargestVisibleWindow([uint32]$TargetPid)
    if ($window -eq [IntPtr]::Zero) {
      throw "Process $TargetPid ($($process.ProcessName)) has no visible, non-minimized top-level window to capture."
    }

    $rect = [DevboxCaptureNative+RECT]::new()
    if (-not [DevboxCaptureNative]::GetWindowRect($window, [ref]$rect)) {
      throw "Could not read the window bounds for process $TargetPid."
    }

    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
      throw "Process $TargetPid has invalid window bounds ($width x $height)."
    }

    $bitmap = [System.Drawing.Bitmap]::new($width, $height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    try {
      $printed = [DevboxCaptureNative]::PrintWindow($window, $hdc, 2)
    } finally {
      $graphics.ReleaseHdc($hdc)
    }

    if (-not $printed) {
      $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, [System.Drawing.Size]::new($width, $height), [System.Drawing.CopyPixelOperation]::SourceCopy)
    }
    Save-Jpeg -Bitmap $bitmap -Path $OutputPath -JpegQuality $Quality

    $metadata = @{
      capture_mode = 'program_pid'
      pid = $TargetPid
      process_name = $process.ProcessName
      window_handle = $window.ToInt64()
      window_title = [DevboxCaptureNative]::ReadWindowTitle($window)
      capture_method = $(if ($printed) { 'PrintWindow' } else { 'CopyFromScreen' })
      left = $rect.Left
      top = $rect.Top
      width = $width
      height = $height
      quality = $Quality
    }
  }

  [Console]::Out.Write(($metadata | ConvertTo-Json -Compress))
} finally {
  if ($null -ne $graphics) { $graphics.Dispose() }
  if ($null -ne $bitmap) { $bitmap.Dispose() }
}
`;

export const isJpegBuffer = (buffer) =>
  Buffer.isBuffer(buffer) &&
  buffer.length >= 5 &&
  buffer.subarray(0, JPEG_START.length).equals(JPEG_START) &&
  buffer.subarray(buffer.length - JPEG_END.length).equals(JPEG_END);

const captureWindowsJpeg = async ({ mode, pid = 0, quality = 85, timeoutMs = 30000 }) => {
  assertHostExecEnabled();
  if (!config.platform.isWindows || process.platform !== "win32") {
    throw new HostCommandError("Windows display capture is available only on a Windows host.");
  }

  const tempDir = await mkdtemp(path.join(os.tmpdir(), "docker-chatgpt-devbox-capture-"));
  const scriptPath = path.join(tempDir, "capture.ps1");
  const jpegPath = path.join(tempDir, "capture.jpg");

  try {
    await writeFile(scriptPath, captureScript, "utf8");
    const result = await spawnPowerShellProcess(
      [
        ...buildWindowsPowerShellFileArgs(scriptPath),
        "-Mode",
        mode,
        "-OutputPath",
        jpegPath,
        "-Quality",
        String(quality),
        "-TargetPid",
        String(pid),
      ],
      {
        cwd: tempDir,
        timeoutMs,
      },
    );

    const jpeg = await readFile(jpegPath);
    if (!isJpegBuffer(jpeg)) {
      throw new HostCommandError("Windows capture did not produce a valid JPEG byte stream.", {
        stdout: result.stdout,
        stderr: result.stderr,
      });
    }

    let metadata;
    try {
      metadata = JSON.parse(result.stdout || "{}");
    } catch (error) {
      throw new HostCommandError("Windows capture returned invalid metadata.", {
        stdout: result.stdout,
        stderr: result.stderr,
        data: { cause: error instanceof Error ? error.message : String(error) },
      });
    }

    return {
      jpeg,
      metadata: {
        ...metadata,
        mime_type: "image/jpeg",
        bytes: jpeg.length,
        sha256: createHash("sha256").update(jpeg).digest("hex"),
      },
    };
  } catch (error) {
    if (error instanceof HostCommandError) {
      throw error;
    }
    throw new HostCommandError(error instanceof Error ? error.message : "Windows display capture failed.", {
      exitCode: error?.exitCode,
      stdout: error?.stdout,
      stderr: error?.stderr,
    });
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
};

export const captureFullDisplayJpeg = ({ quality = 85, timeoutMs = 30000 } = {}) =>
  captureWindowsJpeg({ mode: "display", quality, timeoutMs });

export const captureProgramWindowJpeg = ({ pid, quality = 85, timeoutMs = 30000 }) => {
  if (!Number.isInteger(pid) || pid <= 0) {
    throw new HostCommandError("pid must be a positive Windows process ID.");
  }
  return captureWindowsJpeg({ mode: "pid", pid, quality, timeoutMs });
};
