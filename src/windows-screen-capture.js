import { createHash } from "node:crypto";
import os from "node:os";
import path from "node:path";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";

import { config } from "./config.js";
import { HostCommandError, assertHostExecEnabled, buildWindowsPowerShellFileArgs, spawnPowerShellProcess } from "./host-tools.js";

const JPEG_START = Buffer.from([0xff, 0xd8, 0xff]);
const JPEG_END = Buffer.from([0xff, 0xd9]);

const captureScript = String.raw`
param(
  [Parameter(Mandatory = $true)][ValidateSet('display', 'pid')][string]$Mode,
  [Parameter(Mandatory = $true)][string]$OutputPath,
  [Parameter(Mandatory = $true)][ValidateRange(1, 100)][int]$Quality,
  [int]$TargetPid = 0,
  [ValidateSet('0', '1')][string]$IncludeProcessTree = '1'
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
    private const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
    private const int DWMWA_CLOAKED = 14;

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
    public static extern bool SetProcessDpiAwarenessContext(IntPtr dpiContext);

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

    [DllImport("dwmapi.dll")]
    private static extern int DwmGetWindowAttribute(IntPtr hwnd, int dwAttribute, out RECT pvAttribute, int cbAttribute);

    [DllImport("dwmapi.dll")]
    private static extern int DwmGetWindowAttribute(IntPtr hwnd, int dwAttribute, out int pvAttribute, int cbAttribute);

    [DllImport("dwmapi.dll")]
    public static extern int DwmFlush();

    public static bool IsCloaked(IntPtr hWnd) {
        try {
            int cloaked;
            return DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, out cloaked, Marshal.SizeOf(typeof(int))) == 0 && cloaked != 0;
        } catch {
            return false;
        }
    }

    public static bool GetVisualWindowRect(IntPtr hWnd, out RECT rect) {
        try {
            if (DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, out rect, Marshal.SizeOf(typeof(RECT))) == 0 &&
                rect.Right > rect.Left && rect.Bottom > rect.Top) {
                return true;
            }
        } catch {
        }
        return GetWindowRect(hWnd, out rect);
    }

    public static IntPtr FindLargestVisibleWindow(uint[] targetPids) {
        var pids = new HashSet<uint>(targetPids ?? new uint[0]);
        IntPtr best = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows((hWnd, lParam) => {
            uint ownerPid;
            GetWindowThreadProcessId(hWnd, out ownerPid);
            if (!pids.Contains(ownerPid) || !IsWindowVisible(hWnd) || IsIconic(hWnd) || IsCloaked(hWnd)) {
                return true;
            }

            RECT rect;
            if (!GetVisualWindowRect(hWnd, out rect)) {
                return true;
            }

            long width = Math.Max(0, rect.Right - rect.Left);
            long height = Math.Max(0, rect.Bottom - rect.Top);
            long area = width * height;
            if (width >= 32 && height >= 32 && area > bestArea) {
                best = hWnd;
                bestArea = area;
            }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    public static uint ReadWindowPid(IntPtr hWnd) {
        uint pid;
        GetWindowThreadProcessId(hWnd, out pid);
        return pid;
    }

    public static string ReadWindowTitle(IntPtr hWnd) {
        int length = GetWindowTextLength(hWnd);
        StringBuilder text = new StringBuilder(Math.Max(1, length + 1));
        GetWindowText(hWnd, text, text.Capacity);
        return text.ToString();
    }
}
'@

try {
  if (-not [DevboxCaptureNative]::SetProcessDpiAwarenessContext([IntPtr](-4))) {
    [DevboxCaptureNative]::SetProcessDPIAware() | Out-Null
  }
} catch {
  [DevboxCaptureNative]::SetProcessDPIAware() | Out-Null
}

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

function Measure-BitmapFrame {
  param([Parameter(Mandatory = $true)][System.Drawing.Bitmap]$Bitmap)

  $sampleColumns = [Math]::Min(32, [Math]::Max(1, $Bitmap.Width))
  $sampleRows = [Math]::Min(32, [Math]::Max(1, $Bitmap.Height))
  $count = 0
  $sum = 0.0
  $minLuma = 255.0
  $maxLuma = 0.0
  $nearBlack = 0
  $interiorCount = 0
  $interiorSum = 0.0
  $interiorNearBlack = 0

  for ($row = 0; $row -lt $sampleRows; $row++) {
    $normalizedY = ($row + 0.5) / $sampleRows
    $y = [Math]::Min($Bitmap.Height - 1, [int](($row + 0.5) * $Bitmap.Height / $sampleRows))
    for ($column = 0; $column -lt $sampleColumns; $column++) {
      $normalizedX = ($column + 0.5) / $sampleColumns
      $x = [Math]::Min($Bitmap.Width - 1, [int](($column + 0.5) * $Bitmap.Width / $sampleColumns))
      $pixel = $Bitmap.GetPixel($x, $y)
      $luma = 0.2126 * $pixel.R + 0.7152 * $pixel.G + 0.0722 * $pixel.B
      $isNearBlack = ($pixel.R -le 8 -and $pixel.G -le 8 -and $pixel.B -le 8)
      $sum += $luma
      $minLuma = [Math]::Min($minLuma, $luma)
      $maxLuma = [Math]::Max($maxLuma, $luma)
      if ($isNearBlack) { $nearBlack++ }
      $count++

      # Ignore borders/title chrome when deciding whether the application
      # renderer itself is blank. GPU video/emulator surfaces commonly leave
      # non-black non-client pixels while the central DirectComposition client
      # surface returned by PrintWindow is entirely black.
      if ($normalizedX -ge 0.08 -and $normalizedX -le 0.92 -and $normalizedY -ge 0.18 -and $normalizedY -le 0.92) {
        $interiorSum += $luma
        if ($isNearBlack) { $interiorNearBlack++ }
        $interiorCount++
      }
    }
  }

  $mean = if ($count -gt 0) { $sum / $count } else { 0.0 }
  $nearBlackRatio = if ($count -gt 0) { $nearBlack / $count } else { 1.0 }
  $interiorMean = if ($interiorCount -gt 0) { $interiorSum / $interiorCount } else { $mean }
  $interiorNearBlackRatio = if ($interiorCount -gt 0) { $interiorNearBlack / $interiorCount } else { $nearBlackRatio }
  $suspicious = ($nearBlackRatio -ge 0.985 -and $mean -le 12.0) -or ($interiorNearBlackRatio -ge 0.94 -and $interiorMean -le 16.0)
  [pscustomobject]@{
    MeanLuma = [Math]::Round($mean, 3)
    LumaRange = [Math]::Round($maxLuma - $minLuma, 3)
    NearBlackRatio = [Math]::Round($nearBlackRatio, 4)
    InteriorMeanLuma = [Math]::Round($interiorMean, 3)
    InteriorNearBlackRatio = [Math]::Round($interiorNearBlackRatio, 4)
    Suspicious = $suspicious
  }
}

function Get-ProcessTreePids {
  param([Parameter(Mandatory = $true)][uint32]$RootPid, [bool]$IncludeTree)

  $result = [System.Collections.Generic.HashSet[uint32]]::new()
  [void]$result.Add($RootPid)
  if (-not $IncludeTree) { return [uint32[]]$result }

  try {
    $all = @(Get-CimInstance Win32_Process -ErrorAction Stop | Select-Object ProcessId, ParentProcessId)
    $queue = [System.Collections.Generic.Queue[uint32]]::new()
    $queue.Enqueue($RootPid)
    while ($queue.Count -gt 0) {
      $parent = $queue.Dequeue()
      foreach ($candidate in $all) {
        if ([uint32]$candidate.ParentProcessId -eq $parent) {
          $child = [uint32]$candidate.ProcessId
          if ($result.Add($child)) { $queue.Enqueue($child) }
        }
      }
    }
  } catch {
  }

  return [uint32[]]$result
}

function New-CaptureBitmap {
  param([int]$Width, [int]$Height)
  $bmp = [System.Drawing.Bitmap]::new($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
  $gfx = [System.Drawing.Graphics]::FromImage($bmp)
  $gfx.Clear([System.Drawing.Color]::Black)
  [pscustomobject]@{ Bitmap = $bmp; Graphics = $gfx }
}

$bitmap = $null
$graphics = $null
try {
  if ($Mode -eq 'display') {
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    if ($bounds.Width -le 0 -or $bounds.Height -le 0) {
      throw 'Windows reported an empty virtual display.'
    }

    $capture = New-CaptureBitmap -Width $bounds.Width -Height $bounds.Height
    $bitmap = $capture.Bitmap
    $graphics = $capture.Graphics
    $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bounds.Size, [System.Drawing.CopyPixelOperation]::SourceCopy)
    Save-Jpeg -Bitmap $bitmap -Path $OutputPath -JpegQuality $Quality

    $metadata = @{
      capture_mode = 'full_display'
      capture_method = 'DesktopCompositorCopy'
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
    $candidatePids = Get-ProcessTreePids -RootPid ([uint32]$TargetPid) -IncludeTree ($IncludeProcessTree -eq '1')
    $window = [DevboxCaptureNative]::FindLargestVisibleWindow($candidatePids)
    if ($window -eq [IntPtr]::Zero) {
      throw "Process $TargetPid ($($process.ProcessName)) and its visible child processes have no non-minimized, non-cloaked top-level window to capture."
    }

    $ownerPid = [int][DevboxCaptureNative]::ReadWindowPid($window)
    $rect = [DevboxCaptureNative+RECT]::new()
    if (-not [DevboxCaptureNative]::GetVisualWindowRect($window, [ref]$rect)) {
      throw "Could not read the visual window bounds for process $TargetPid."
    }

    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
      throw "Process $TargetPid has invalid window bounds ($width x $height)."
    }

    $capture = New-CaptureBitmap -Width $width -Height $height
    $bitmap = $capture.Bitmap
    $graphics = $capture.Graphics
    $captureMethod = $null
    $printAnalysis = $null
    $printFlags = $null
    $printRejected = $false

    foreach ($flags in @(2, 0)) {
      $graphics.Clear([System.Drawing.Color]::Black)
      $hdc = $graphics.GetHdc()
      try {
        $printed = [DevboxCaptureNative]::PrintWindow($window, $hdc, [uint32]$flags)
      } finally {
        $graphics.ReleaseHdc($hdc)
      }

      if (-not $printed) { continue }
      $analysis = Measure-BitmapFrame -Bitmap $bitmap
      $printAnalysis = $analysis
      $printFlags = $flags
      if (-not $analysis.Suspicious) {
        $captureMethod = $(if ($flags -eq 2) { 'PrintWindow(PW_RENDERFULLCONTENT)' } else { 'PrintWindow(default)' })
        break
      }
      $printRejected = $true
    }

    $capturedLeft = $rect.Left
    $capturedTop = $rect.Top
    $capturedWidth = $width
    $capturedHeight = $height
    $clippedToDisplay = $false

    if ($null -eq $captureMethod) {
      try { [void][DevboxCaptureNative]::DwmFlush() } catch {}
      $virtual = [System.Windows.Forms.SystemInformation]::VirtualScreen
      $capturedLeft = [Math]::Max($rect.Left, $virtual.Left)
      $capturedTop = [Math]::Max($rect.Top, $virtual.Top)
      $capturedRight = [Math]::Min($rect.Right, $virtual.Right)
      $capturedBottom = [Math]::Min($rect.Bottom, $virtual.Bottom)
      $capturedWidth = $capturedRight - $capturedLeft
      $capturedHeight = $capturedBottom - $capturedTop
      if ($capturedWidth -le 0 -or $capturedHeight -le 0) {
        throw "The window is completely outside the visible virtual desktop and PrintWindow did not return usable pixels."
      }

      $clippedToDisplay = ($capturedLeft -ne $rect.Left -or $capturedTop -ne $rect.Top -or $capturedWidth -ne $width -or $capturedHeight -ne $height)
      $graphics.Dispose()
      $bitmap.Dispose()
      $capture = New-CaptureBitmap -Width $capturedWidth -Height $capturedHeight
      $bitmap = $capture.Bitmap
      $graphics = $capture.Graphics
      $graphics.CopyFromScreen(
        $capturedLeft,
        $capturedTop,
        0,
        0,
        [System.Drawing.Size]::new($capturedWidth, $capturedHeight),
        [System.Drawing.CopyPixelOperation]::SourceCopy
      )
      $captureMethod = 'DesktopCompositorCopy(window-bounds)'
    }

    Save-Jpeg -Bitmap $bitmap -Path $OutputPath -JpegQuality $Quality

    $metadata = @{
      capture_mode = 'program_pid'
      pid = $TargetPid
      process_name = $process.ProcessName
      window_owner_pid = $ownerPid
      process_tree_fallback = ($ownerPid -ne $TargetPid)
      candidate_pid_count = @($candidatePids).Count
      window_handle = $window.ToInt64()
      window_title = [DevboxCaptureNative]::ReadWindowTitle($window)
      capture_method = $captureMethod
      print_window_rejected = $printRejected
      print_window_flags = $printFlags
      print_window_mean_luma = $(if ($null -ne $printAnalysis) { $printAnalysis.MeanLuma } else { $null })
      print_window_luma_range = $(if ($null -ne $printAnalysis) { $printAnalysis.LumaRange } else { $null })
      print_window_near_black_ratio = $(if ($null -ne $printAnalysis) { $printAnalysis.NearBlackRatio } else { $null })
      print_window_interior_mean_luma = $(if ($null -ne $printAnalysis) { $printAnalysis.InteriorMeanLuma } else { $null })
      print_window_interior_near_black_ratio = $(if ($null -ne $printAnalysis) { $printAnalysis.InteriorNearBlackRatio } else { $null })
      requested_left = $rect.Left
      requested_top = $rect.Top
      requested_width = $width
      requested_height = $height
      left = $capturedLeft
      top = $capturedTop
      width = $capturedWidth
      height = $capturedHeight
      clipped_to_display = $clippedToDisplay
      screen_fallback_may_include_occluders = ($captureMethod -like 'DesktopCompositorCopy*')
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

const captureWindowsJpeg = async ({ mode, pid = 0, quality = 85, timeoutMs = 30000, includeProcessTree = true, signal }) => {
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
        "-IncludeProcessTree",
        includeProcessTree ? "1" : "0",
      ],
      {
        cwd: tempDir,
        timeoutMs,
        signal,
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
      image: jpeg,
      mimeType: "image/jpeg",
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
      timedOut: error?.timedOut === true,
      aborted: error?.aborted === true,
      signal: error?.signal ?? null,
    });
  } finally {
    await rm(tempDir, { recursive: true, force: true, maxRetries: 3, retryDelay: 50 }).catch(() => {});
  }
};

export const captureFullDisplayJpeg = ({ quality = 85, timeoutMs = 30000, signal } = {}) =>
  captureWindowsJpeg({ mode: "display", quality, timeoutMs, signal });

export const captureProgramWindowJpeg = ({ pid, quality = 85, timeoutMs = 30000, includeProcessTree = true, signal }) => {
  if (!Number.isInteger(pid) || pid <= 0) {
    throw new HostCommandError("pid must be a positive Windows process ID.");
  }
  return captureWindowsJpeg({ mode: "pid", pid, quality, timeoutMs, includeProcessTree, signal });
};
