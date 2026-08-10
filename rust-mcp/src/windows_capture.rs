#![allow(
    unsafe_code,
    reason = "isolated Win32 capture backend; all public entry points are safe"
)]

use std::{
    collections::{HashMap, HashSet, VecDeque},
    ffi::c_void,
    mem::size_of,
    ptr::null_mut,
};

use anyhow::{Context, Result, bail};
use image::{ExtendedColorType, codecs::jpeg::JpegEncoder};
use serde_json::{Value, json};
use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE, HWND, INVALID_HANDLE_VALUE, LPARAM, RECT},
    Graphics::{
        Dwm::{DwmFlush, DwmGetWindowAttribute},
        Gdi::{
            BI_RGB, BITMAPINFO, BITMAPINFOHEADER, BLACKNESS, BitBlt, CAPTUREBLT,
            CreateCompatibleBitmap, CreateCompatibleDC, DIB_RGB_COLORS, DeleteDC, DeleteObject,
            GetDC, GetDIBits, HBITMAP, HDC, HGDIOBJ, PatBlt, RGBQUAD, ReleaseDC, SRCCOPY,
            SelectObject,
        },
    },
    Storage::Xps::PrintWindow,
    System::{
        Diagnostics::ToolHelp::{
            CreateToolhelp32Snapshot, PROCESSENTRY32W, Process32FirstW, Process32NextW,
            TH32CS_SNAPPROCESS,
        },
        Threading::{OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION, QueryFullProcessImageNameW},
    },
    UI::{
        HiDpi::{DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetProcessDpiAwarenessContext},
        WindowsAndMessaging::{
            EnumWindows, GetSystemMetrics, GetWindowRect, GetWindowTextLengthW, GetWindowTextW,
            GetWindowThreadProcessId, IsIconic, IsWindowVisible, PW_RENDERFULLCONTENT,
            SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN, SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN,
        },
    },
};

const DWMWA_EXTENDED_FRAME_BOUNDS: u32 = 9;
const DWMWA_CLOAKED: u32 = 14;
const MAX_CAPTURE_DIMENSION: u32 = 32_768;
const MAX_CAPTURE_PIXELS: u64 = 64 * 1024 * 1024;

#[derive(Debug, Clone)]
pub struct NativeCapture {
    pub image: Vec<u8>,
    pub mime_type: &'static str,
    pub metadata: Value,
}

#[derive(Debug, Clone, Copy)]
struct FrameRect {
    left: i32,
    top: i32,
    width: i32,
    height: i32,
}

impl FrameRect {
    fn from_rect(rect: RECT) -> Option<Self> {
        let width = rect.right.saturating_sub(rect.left);
        let height = rect.bottom.saturating_sub(rect.top);
        (width > 0 && height > 0).then_some(Self {
            left: rect.left,
            top: rect.top,
            width,
            height,
        })
    }

    fn right(self) -> i32 {
        self.left.saturating_add(self.width)
    }

    fn bottom(self) -> i32 {
        self.top.saturating_add(self.height)
    }
}

#[derive(Debug, Clone)]
struct WindowCandidate {
    hwnd: HWND,
    owner_pid: u32,
    rect: FrameRect,
    title: String,
}

#[derive(Debug, Clone)]
struct ProcessInfo {
    parent_pid: u32,
    executable: String,
}

#[derive(Debug, Clone)]
struct RgbFrame {
    width: u32,
    height: u32,
    pixels: Vec<u8>,
}

#[derive(Debug, Clone, Copy)]
struct FrameAnalysis {
    mean_luma: f64,
    luma_range: f64,
    near_black_ratio: f64,
    interior_mean_luma: f64,
    interior_near_black_ratio: f64,
    suspicious: bool,
}

struct ScreenDc(HDC);
impl Drop for ScreenDc {
    fn drop(&mut self) {
        unsafe {
            let _ = ReleaseDC(null_mut(), self.0);
        }
    }
}

struct MemoryDc(HDC);
impl Drop for MemoryDc {
    fn drop(&mut self) {
        unsafe {
            let _ = DeleteDC(self.0);
        }
    }
}

struct Bitmap(HBITMAP);
impl Drop for Bitmap {
    fn drop(&mut self) {
        unsafe {
            let _ = DeleteObject(self.0 as HGDIOBJ);
        }
    }
}

struct Snapshot(HANDLE);
impl Drop for Snapshot {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

struct ProcessHandle(HANDLE);
impl Drop for ProcessHandle {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

/// Capture the complete Windows virtual desktop into JPEG bytes.
///
/// # Errors
/// Returns when Windows reports no visible desktop, GDI capture fails, or JPEG encoding fails.
pub fn capture_display(quality: u8) -> Result<NativeCapture> {
    set_dpi_awareness();
    let rect = virtual_screen_rect()?;
    let frame = capture_screen_region(rect)?;
    let image = encode_jpeg(&frame, quality)?;
    Ok(NativeCapture {
        image,
        mime_type: "image/jpeg",
        metadata: json!({
            "capture_mode": "full_display",
            "capture_method": "DesktopCompositorCopy",
            "left": rect.left,
            "top": rect.top,
            "width": rect.width,
            "height": rect.height,
            "quality": quality,
        }),
    })
}

/// Capture the largest visible top-level window belonging to a PID/process tree.
///
/// The implementation mirrors the JavaScript backend: full-content `PrintWindow`, default
/// `PrintWindow`, black-frame rejection, then compositor-visible window-bounds fallback.
///
/// # Errors
/// Returns for invalid/missing processes, no eligible visible window, capture failure,
/// or JPEG encoding failure.
pub fn capture_program(pid: u32, quality: u8, include_process_tree: bool) -> Result<NativeCapture> {
    if pid == 0 {
        bail!("pid must be a positive Windows process ID.");
    }
    set_dpi_awareness();
    let processes = process_table()?;
    let root = processes
        .get(&pid)
        .with_context(|| format!("Windows process {pid} does not exist."))?;
    let process_name = query_process_image_name(pid).unwrap_or_else(|| root.executable.clone());
    let candidate_pids = collect_process_tree(&processes, pid, include_process_tree);
    let window = find_largest_visible_window(&candidate_pids)?.with_context(|| {
        format!(
            "Process {pid} ({process_name}) and its visible child processes have no non-minimized, non-cloaked top-level window to capture."
        )
    })?;

    let mut chosen_frame = None;
    let mut chosen_method = None;
    let mut last_analysis = None;
    let mut last_flags = None;
    let mut print_rejected = false;

    for flags in [PW_RENDERFULLCONTENT, 0] {
        if let Some(frame) = capture_print_window(window.hwnd, window.rect, flags)? {
            let analysis = analyze_frame(&frame);
            last_analysis = Some(analysis);
            last_flags = Some(flags);
            if !analysis.suspicious {
                chosen_method = Some(if flags == PW_RENDERFULLCONTENT {
                    "PrintWindow(PW_RENDERFULLCONTENT)"
                } else {
                    "PrintWindow(default)"
                });
                chosen_frame = Some(frame);
                break;
            }
            print_rejected = true;
        }
    }

    let requested = window.rect;
    let (frame, method, captured, clipped) = if let (Some(frame), Some(method)) =
        (chosen_frame, chosen_method)
    {
        (frame, method, requested, false)
    } else {
        unsafe {
            let _ = DwmFlush();
        }
        let visible = intersect_rect(requested, virtual_screen_rect()?).with_context(|| {
            "The window is completely outside the visible virtual desktop and PrintWindow did not return usable pixels."
        })?;
        (
            capture_screen_region(visible)?,
            "DesktopCompositorCopy(window-bounds)",
            visible,
            visible.left != requested.left
                || visible.top != requested.top
                || visible.width != requested.width
                || visible.height != requested.height,
        )
    };
    let image = encode_jpeg(&frame, quality)?;
    let analysis = last_analysis;
    Ok(NativeCapture {
        image,
        mime_type: "image/jpeg",
        metadata: json!({
            "capture_mode": "program_pid",
            "pid": pid,
            "process_name": process_name,
            "window_owner_pid": window.owner_pid,
            "process_tree_fallback": window.owner_pid != pid,
            "candidate_pid_count": candidate_pids.len(),
            "window_handle": window.hwnd as isize as i64,
            "window_title": window.title,
            "capture_method": method,
            "print_window_rejected": print_rejected,
            "print_window_flags": last_flags,
            "print_window_mean_luma": analysis.map(|value| round3(value.mean_luma)),
            "print_window_luma_range": analysis.map(|value| round3(value.luma_range)),
            "print_window_near_black_ratio": analysis.map(|value| round4(value.near_black_ratio)),
            "print_window_interior_mean_luma": analysis.map(|value| round3(value.interior_mean_luma)),
            "print_window_interior_near_black_ratio": analysis.map(|value| round4(value.interior_near_black_ratio)),
            "requested_left": requested.left,
            "requested_top": requested.top,
            "requested_width": requested.width,
            "requested_height": requested.height,
            "left": captured.left,
            "top": captured.top,
            "width": captured.width,
            "height": captured.height,
            "clipped_to_display": clipped,
            "screen_fallback_may_include_occluders": method.starts_with("DesktopCompositorCopy"),
            "quality": quality,
        }),
    })
}

fn set_dpi_awareness() {
    unsafe {
        let _ = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

fn virtual_screen_rect() -> Result<FrameRect> {
    let (left, top, width, height) = unsafe {
        (
            GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN),
        )
    };
    if width <= 0 || height <= 0 {
        bail!("Windows reported an empty virtual display.");
    }
    Ok(FrameRect {
        left,
        top,
        width,
        height,
    })
}

fn capture_screen_region(rect: FrameRect) -> Result<RgbFrame> {
    let screen = unsafe { GetDC(null_mut()) };
    if screen.is_null() {
        bail!("GetDC failed for the Windows virtual desktop.");
    }
    let screen = ScreenDc(screen);
    let (memory, bitmap) = compatible_target(screen.0, rect.width, rect.height)?;
    let old = unsafe { SelectObject(memory.0, bitmap.0 as HGDIOBJ) };
    if old.is_null() {
        bail!("SelectObject failed while preparing desktop capture.");
    }
    let copied = unsafe {
        BitBlt(
            memory.0,
            0,
            0,
            rect.width,
            rect.height,
            screen.0,
            rect.left,
            rect.top,
            SRCCOPY | CAPTUREBLT,
        )
    };
    unsafe {
        let _ = SelectObject(memory.0, old);
    }
    if copied == 0 {
        bail!("BitBlt failed while copying the Windows desktop compositor.");
    }
    bitmap_to_rgb(memory.0, bitmap.0, rect.width, rect.height)
}

fn capture_print_window(hwnd: HWND, rect: FrameRect, flags: u32) -> Result<Option<RgbFrame>> {
    let screen = unsafe { GetDC(null_mut()) };
    if screen.is_null() {
        bail!("GetDC failed while preparing PrintWindow capture.");
    }
    let screen = ScreenDc(screen);
    let (memory, bitmap) = compatible_target(screen.0, rect.width, rect.height)?;
    let old = unsafe { SelectObject(memory.0, bitmap.0 as HGDIOBJ) };
    if old.is_null() {
        bail!("SelectObject failed while preparing PrintWindow capture.");
    }
    unsafe {
        let _ = PatBlt(memory.0, 0, 0, rect.width, rect.height, BLACKNESS);
    }
    let printed = unsafe { PrintWindow(hwnd, memory.0, flags) };
    unsafe {
        let _ = SelectObject(memory.0, old);
    }
    if printed == 0 {
        return Ok(None);
    }
    Ok(Some(bitmap_to_rgb(
        memory.0,
        bitmap.0,
        rect.width,
        rect.height,
    )?))
}

fn compatible_target(screen: HDC, width: i32, height: i32) -> Result<(MemoryDc, Bitmap)> {
    validate_capture_dimensions(width, height)?;
    let memory = unsafe { CreateCompatibleDC(screen) };
    if memory.is_null() {
        bail!("CreateCompatibleDC failed during Windows capture.");
    }
    let memory = MemoryDc(memory);
    let bitmap = unsafe { CreateCompatibleBitmap(screen, width, height) };
    if bitmap.is_null() {
        bail!("CreateCompatibleBitmap failed for {width}x{height} capture.");
    }
    Ok((memory, Bitmap(bitmap)))
}

fn bitmap_to_rgb(hdc: HDC, bitmap: HBITMAP, width: i32, height: i32) -> Result<RgbFrame> {
    let (width_u32, height_u32, pixels) = validate_capture_dimensions(width, height)?;
    let bgra_bytes = pixels
        .checked_mul(4)
        .context("capture BGRA byte count overflow")?;
    let mut bgra = Vec::new();
    bgra.try_reserve_exact(bgra_bytes)
        .context("reserve Windows capture BGRA buffer")?;
    bgra.resize(bgra_bytes, 0);
    let mut info = BITMAPINFO {
        bmiHeader: BITMAPINFOHEADER {
            biSize: u32::try_from(size_of::<BITMAPINFOHEADER>()).unwrap_or(u32::MAX),
            biWidth: width,
            biHeight: -height,
            biPlanes: 1,
            biBitCount: 32,
            biCompression: BI_RGB,
            biSizeImage: 0,
            biXPelsPerMeter: 0,
            biYPelsPerMeter: 0,
            biClrUsed: 0,
            biClrImportant: 0,
        },
        bmiColors: [RGBQUAD::default(); 1],
    };
    let lines = unsafe {
        GetDIBits(
            hdc,
            bitmap,
            0,
            height_u32,
            bgra.as_mut_ptr().cast::<c_void>(),
            &raw mut info,
            DIB_RGB_COLORS,
        )
    };
    if lines != height {
        bail!("GetDIBits returned {lines} scanlines for a {height}-line capture.");
    }
    let rgb_bytes = pixels
        .checked_mul(3)
        .context("capture RGB byte count overflow")?;
    let mut rgb = Vec::new();
    rgb.try_reserve_exact(rgb_bytes)
        .context("reserve Windows capture RGB buffer")?;
    for pixel in bgra.chunks_exact(4) {
        rgb.extend_from_slice(&[pixel[2], pixel[1], pixel[0]]);
    }
    Ok(RgbFrame {
        width: width_u32,
        height: height_u32,
        pixels: rgb,
    })
}

fn validate_capture_dimensions(width: i32, height: i32) -> Result<(u32, u32, usize)> {
    let width = u32::try_from(width).context("capture width must be positive")?;
    let height = u32::try_from(height).context("capture height must be positive")?;
    if width == 0 || height == 0 {
        bail!("capture dimensions must be positive, got {width}x{height}.");
    }
    if width > MAX_CAPTURE_DIMENSION || height > MAX_CAPTURE_DIMENSION {
        bail!(
            "capture dimensions {width}x{height} exceed the maximum supported dimension {MAX_CAPTURE_DIMENSION}."
        );
    }
    let pixels = u64::from(width)
        .checked_mul(u64::from(height))
        .context("capture pixel count overflow")?;
    if pixels > MAX_CAPTURE_PIXELS {
        bail!(
            "capture dimensions {width}x{height} exceed the maximum supported pixel count {MAX_CAPTURE_PIXELS}."
        );
    }
    let pixels = usize::try_from(pixels).context("capture pixel count does not fit memory size")?;
    Ok((width, height, pixels))
}

fn encode_jpeg(frame: &RgbFrame, quality: u8) -> Result<Vec<u8>> {
    let mut output = Vec::new();
    JpegEncoder::new_with_quality(&mut output, quality.clamp(1, 100))
        .encode(
            &frame.pixels,
            frame.width,
            frame.height,
            ExtendedColorType::Rgb8,
        )
        .context("encode Windows capture JPEG")?;
    if output.len() < 5
        || !output.starts_with(&[0xff, 0xd8, 0xff])
        || !output.ends_with(&[0xff, 0xd9])
    {
        bail!("Windows capture did not produce a valid JPEG byte stream.");
    }
    Ok(output)
}

fn analyze_frame(frame: &RgbFrame) -> FrameAnalysis {
    let columns = frame.width.clamp(1, 32);
    let rows = frame.height.clamp(1, 32);
    let mut count = 0_u32;
    let mut sum = 0.0;
    let mut min_luma = 255.0_f64;
    let mut max_luma = 0.0_f64;
    let mut near_black = 0_u32;
    let mut interior_count = 0_u32;
    let mut interior_sum = 0.0;
    let mut interior_near_black = 0_u32;

    for row in 0..rows {
        let normalized_y = (f64::from(row) + 0.5) / f64::from(rows);
        let y = sample_coordinate(row, rows, frame.height);
        for column in 0..columns {
            let normalized_x = (f64::from(column) + 0.5) / f64::from(columns);
            let x = sample_coordinate(column, columns, frame.width);
            let offset = (usize::try_from(y)
                .unwrap_or(0)
                .saturating_mul(usize::try_from(frame.width).unwrap_or(0))
                .saturating_add(usize::try_from(x).unwrap_or(0)))
            .saturating_mul(3);
            let red = frame.pixels.get(offset).copied().unwrap_or_default();
            let green = frame.pixels.get(offset + 1).copied().unwrap_or_default();
            let blue = frame.pixels.get(offset + 2).copied().unwrap_or_default();
            let luma =
                0.2126 * f64::from(red) + 0.7152 * f64::from(green) + 0.0722 * f64::from(blue);
            let black = red <= 8 && green <= 8 && blue <= 8;
            sum += luma;
            min_luma = min_luma.min(luma);
            max_luma = max_luma.max(luma);
            near_black += u32::from(black);
            count += 1;
            if (0.08..=0.92).contains(&normalized_x) && (0.18..=0.92).contains(&normalized_y) {
                interior_sum += luma;
                interior_near_black += u32::from(black);
                interior_count += 1;
            }
        }
    }
    let mean = if count > 0 {
        sum / f64::from(count)
    } else {
        0.0
    };
    let near_black_ratio = if count > 0 {
        f64::from(near_black) / f64::from(count)
    } else {
        1.0
    };
    let interior_mean = if interior_count > 0 {
        interior_sum / f64::from(interior_count)
    } else {
        mean
    };
    let interior_near_black_ratio = if interior_count > 0 {
        f64::from(interior_near_black) / f64::from(interior_count)
    } else {
        near_black_ratio
    };
    FrameAnalysis {
        mean_luma: mean,
        luma_range: max_luma - min_luma,
        near_black_ratio,
        interior_mean_luma: interior_mean,
        interior_near_black_ratio,
        suspicious: (near_black_ratio >= 0.985 && mean <= 12.0)
            || (interior_near_black_ratio >= 0.94 && interior_mean <= 16.0),
    }
}

fn sample_coordinate(index: u32, samples: u32, extent: u32) -> u32 {
    let numerator = u64::from(index)
        .saturating_mul(2)
        .saturating_add(1)
        .saturating_mul(u64::from(extent));
    let denominator = u64::from(samples).saturating_mul(2).max(1);
    u32::try_from(numerator / denominator)
        .unwrap_or(u32::MAX)
        .min(extent.saturating_sub(1))
}

fn process_table() -> Result<HashMap<u32, ProcessInfo>> {
    let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
    if snapshot == INVALID_HANDLE_VALUE {
        bail!("CreateToolhelp32Snapshot failed while enumerating Windows processes.");
    }
    let snapshot = Snapshot(snapshot);
    let mut entry = PROCESSENTRY32W {
        dwSize: u32::try_from(size_of::<PROCESSENTRY32W>()).unwrap_or(u32::MAX),
        ..PROCESSENTRY32W::default()
    };
    let mut result = HashMap::new();
    let mut ok = unsafe { Process32FirstW(snapshot.0, &raw mut entry) };
    while ok != 0 {
        result.insert(
            entry.th32ProcessID,
            ProcessInfo {
                parent_pid: entry.th32ParentProcessID,
                executable: wide_array_to_string(&entry.szExeFile),
            },
        );
        ok = unsafe { Process32NextW(snapshot.0, &raw mut entry) };
    }
    Ok(result)
}

fn collect_process_tree(
    processes: &HashMap<u32, ProcessInfo>,
    root_pid: u32,
    include_tree: bool,
) -> HashSet<u32> {
    let mut result = HashSet::from([root_pid]);
    if !include_tree {
        return result;
    }
    let mut queue = VecDeque::from([root_pid]);
    while let Some(parent) = queue.pop_front() {
        for (&pid, info) in processes {
            if info.parent_pid == parent && result.insert(pid) {
                queue.push_back(pid);
            }
        }
    }
    result
}

struct EnumContext<'a> {
    targets: &'a HashSet<u32>,
    best: Option<WindowCandidate>,
    best_area: i64,
}

unsafe extern "system" fn enum_windows_callback(hwnd: HWND, lparam: LPARAM) -> i32 {
    let context = unsafe { &mut *(lparam as *mut EnumContext<'_>) };
    let mut owner_pid = 0_u32;
    unsafe {
        GetWindowThreadProcessId(hwnd, &raw mut owner_pid);
    }
    if !context.targets.contains(&owner_pid)
        || unsafe { IsWindowVisible(hwnd) } == 0
        || unsafe { IsIconic(hwnd) } != 0
        || is_cloaked(hwnd)
    {
        return 1;
    }
    let Some(rect) = visual_window_rect(hwnd) else {
        return 1;
    };
    if rect.width < 32 || rect.height < 32 {
        return 1;
    }
    let area = i64::from(rect.width).saturating_mul(i64::from(rect.height));
    if area > context.best_area {
        context.best_area = area;
        context.best = Some(WindowCandidate {
            hwnd,
            owner_pid,
            rect,
            title: window_title(hwnd),
        });
    }
    1
}

fn find_largest_visible_window(targets: &HashSet<u32>) -> Result<Option<WindowCandidate>> {
    let mut context = EnumContext {
        targets,
        best: None,
        best_area: 0,
    };
    let result = unsafe {
        EnumWindows(
            Some(enum_windows_callback),
            (&raw mut context).cast::<c_void>() as LPARAM,
        )
    };
    if result == 0 {
        bail!("EnumWindows failed while discovering the target program window.");
    }
    Ok(context.best)
}

fn is_cloaked(hwnd: HWND) -> bool {
    let mut cloaked = 0_u32;
    unsafe {
        DwmGetWindowAttribute(
            hwnd,
            DWMWA_CLOAKED,
            (&raw mut cloaked).cast::<c_void>(),
            u32::try_from(size_of::<u32>()).unwrap_or(u32::MAX),
        ) == 0
            && cloaked != 0
    }
}

fn visual_window_rect(hwnd: HWND) -> Option<FrameRect> {
    let mut rect = RECT::default();
    let dwm_ok = unsafe {
        DwmGetWindowAttribute(
            hwnd,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            (&raw mut rect).cast::<c_void>(),
            u32::try_from(size_of::<RECT>()).unwrap_or(u32::MAX),
        ) == 0
    };
    if dwm_ok && let Some(rect) = FrameRect::from_rect(rect) {
        return Some(rect);
    }
    let mut fallback = RECT::default();
    (unsafe { GetWindowRect(hwnd, &raw mut fallback) } != 0)
        .then(|| FrameRect::from_rect(fallback))
        .flatten()
}

fn window_title(hwnd: HWND) -> String {
    let length = unsafe { GetWindowTextLengthW(hwnd) };
    if length <= 0 {
        return String::new();
    }
    let capacity = usize::try_from(length).unwrap_or(0).saturating_add(1);
    let mut buffer = vec![0_u16; capacity];
    let count = unsafe {
        GetWindowTextW(
            hwnd,
            buffer.as_mut_ptr(),
            i32::try_from(buffer.len()).unwrap_or(i32::MAX),
        )
    };
    String::from_utf16_lossy(&buffer[..usize::try_from(count.max(0)).unwrap_or(0)])
}

fn query_process_image_name(pid: u32) -> Option<String> {
    let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
    if handle.is_null() {
        return None;
    }
    let handle = ProcessHandle(handle);
    let mut buffer = vec![0_u16; 32_768];
    let mut size = u32::try_from(buffer.len()).ok()?;
    let ok = unsafe { QueryFullProcessImageNameW(handle.0, 0, buffer.as_mut_ptr(), &raw mut size) };
    if ok == 0 {
        return None;
    }
    let path = String::from_utf16_lossy(&buffer[..usize::try_from(size).ok()?]);
    std::path::Path::new(&path)
        .file_stem()
        .map(|value| value.to_string_lossy().into_owned())
}

fn wide_array_to_string(value: &[u16]) -> String {
    let end = value
        .iter()
        .position(|unit| *unit == 0)
        .unwrap_or(value.len());
    String::from_utf16_lossy(&value[..end])
        .trim_end_matches(".exe")
        .to_owned()
}

fn intersect_rect(left: FrameRect, right: FrameRect) -> Option<FrameRect> {
    let x1 = left.left.max(right.left);
    let y1 = left.top.max(right.top);
    let x2 = left.right().min(right.right());
    let y2 = left.bottom().min(right.bottom());
    FrameRect::from_rect(RECT {
        left: x1,
        top: y1,
        right: x2,
        bottom: y2,
    })
}

fn round3(value: f64) -> f64 {
    (value * 1_000.0).round() / 1_000.0
}

fn round4(value: f64) -> f64 {
    (value * 10_000.0).round() / 10_000.0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn black_frame_analysis_matches_js_threshold_intent() {
        let black = RgbFrame {
            width: 64,
            height: 64,
            pixels: vec![0; 64 * 64 * 3],
        };
        assert!(analyze_frame(&black).suspicious);
        let bright = RgbFrame {
            width: 64,
            height: 64,
            pixels: vec![255; 64 * 64 * 3],
        };
        assert!(!analyze_frame(&bright).suspicious);
    }

    #[test]
    fn process_tree_collection_includes_descendants_only_when_requested() {
        let processes = HashMap::from([
            (
                1,
                ProcessInfo {
                    parent_pid: 0,
                    executable: "root".to_owned(),
                },
            ),
            (
                2,
                ProcessInfo {
                    parent_pid: 1,
                    executable: "child".to_owned(),
                },
            ),
            (
                3,
                ProcessInfo {
                    parent_pid: 2,
                    executable: "grandchild".to_owned(),
                },
            ),
            (
                4,
                ProcessInfo {
                    parent_pid: 9,
                    executable: "other".to_owned(),
                },
            ),
        ]);
        assert_eq!(
            collect_process_tree(&processes, 1, false),
            HashSet::from([1])
        );
        assert_eq!(
            collect_process_tree(&processes, 1, true),
            HashSet::from([1, 2, 3])
        );
    }

    #[test]
    fn capture_dimensions_are_bounded_before_allocation() {
        let (width, height, pixels) = validate_capture_dimensions(15_360, 2_160)
            .expect("four horizontal 4K displays stay supported");
        assert_eq!(width, 15_360);
        assert_eq!(height, 2_160);
        assert_eq!(pixels, 33_177_600);
        assert!(validate_capture_dimensions(32_768, 32_768).is_err());
        assert!(validate_capture_dimensions(40_000, 100).is_err());
        assert!(validate_capture_dimensions(-1, 100).is_err());
    }

    #[test]
    fn clipping_preserves_visible_intersection() {
        let window = FrameRect {
            left: -50,
            top: 10,
            width: 100,
            height: 80,
        };
        let screen = FrameRect {
            left: 0,
            top: 0,
            width: 1920,
            height: 1080,
        };
        let clipped = intersect_rect(window, screen).expect("intersection");
        assert_eq!(clipped.left, 0);
        assert_eq!(clipped.top, 10);
        assert_eq!(clipped.width, 50);
        assert_eq!(clipped.height, 80);
    }
}
