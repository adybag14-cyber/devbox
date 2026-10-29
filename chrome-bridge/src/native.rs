use crate::{
    ipc::{Handle, user_sid, wide},
    protocol::{error, nonce},
};
use base64::Engine;
use serde_json::{Value, json};
use std::{
    collections::HashMap,
    mem, ptr,
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::*,
    Graphics::Gdi::*,
    System::{StationsAndDesktops::*, Threading::*},
    UI::{HiDpi::*, Input::KeyboardAndMouse::*, WindowsAndMessaging::*},
};

#[derive(Clone, Debug, PartialEq)]
struct Window {
    id: usize,
    pid: u32,
    birth: u64,
    title: String,
    rect: [i32; 4],
}
impl Window {
    fn read(id: usize) -> Result<Self, String> {
        unsafe {
            let hwnd = id as HWND;
            if IsWindow(hwnd) == 0 || IsWindowVisible(hwnd) == 0 || IsIconic(hwnd) != 0 {
                return Err("WINDOW_NOT_VISIBLE".into());
            }
            let mut pid = 0;
            GetWindowThreadProcessId(hwnd, &mut pid);
            let process = Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid));
            if process.0.is_null() {
                return Err("PROCESS_IDENTITY_UNAVAILABLE".into());
            }
            let mut name = vec![0_u16; 32768];
            let mut size = name.len() as u32;
            if QueryFullProcessImageNameW(process.0, 0, name.as_mut_ptr(), &mut size) == 0 {
                return Err("PROCESS_IMAGE_UNAVAILABLE".into());
            }
            let path = String::from_utf16_lossy(&name[..size as usize]).to_lowercase();
            if !path.ends_with("\\google\\chrome\\application\\chrome.exe") {
                return Err("ONLY_GOOGLE_CHROME_ALLOWED".into());
            }
            let (mut created, mut exited, mut kernel, mut user) = (
                FILETIME::default(),
                FILETIME::default(),
                FILETIME::default(),
                FILETIME::default(),
            );
            if GetProcessTimes(process.0, &mut created, &mut exited, &mut kernel, &mut user) == 0 {
                return Err("PROCESS_BIRTH_UNAVAILABLE".into());
            }
            let mut text = vec![0_u16; 4096];
            let len = GetWindowTextW(hwnd, text.as_mut_ptr(), text.len() as i32).max(0) as usize;
            let mut rect = RECT::default();
            if GetWindowRect(hwnd, &mut rect) == 0 {
                return Err("WINDOW_GEOMETRY_UNAVAILABLE".into());
            }
            let geometry = [
                rect.left,
                rect.top,
                rect.right - rect.left,
                rect.bottom - rect.top,
            ];
            if geometry[2] < 100 || geometry[3] < 100 || geometry[2] > 16384 || geometry[3] > 16384
            {
                return Err("WINDOW_GEOMETRY_REJECTED".into());
            }
            Ok(Self {
                id,
                pid,
                birth: ((created.dwHighDateTime as u64) << 32) | created.dwLowDateTime as u64,
                title: String::from_utf16_lossy(&text[..len]),
                rect: geometry,
            })
        }
    }
    fn json(&self) -> Value {
        json!({"window_id":self.id,"pid":self.pid,"process_birth":self.birth.to_string(),"title":self.title,"rect":self.rect})
    }
}
unsafe extern "system" fn enum_window(hwnd: HWND, data: LPARAM) -> i32 {
    let windows = unsafe { &mut *(data as *mut Vec<Window>) };
    if windows.len() >= 32 {
        return 0;
    }
    if let Ok(window) = Window::read(hwnd as usize) {
        windows.push(window);
    }
    1
}
fn windows() -> Vec<Window> {
    let mut result = Vec::new();
    unsafe {
        EnumWindows(Some(enum_window), &mut result as *mut Vec<Window> as LPARAM);
    }
    result
}
struct Observation {
    window: Window,
    at: Instant,
    width: i32,
    height: i32,
    source_height: i32,
    header_only: bool,
}
struct InputLock(Handle);
impl InputLock {
    fn acquire() -> Result<Self, String> {
        unsafe {
            let name = wide(&format!(
                "Local\\DevboxChromeNativeInput-{}",
                user_sid().map_err(|e| e.to_string())?
            ));
            let handle = Handle(CreateMutexW(ptr::null(), 0, name.as_ptr()));
            if handle.0.is_null() {
                return Err("NATIVE_INPUT_LOCK_UNAVAILABLE".into());
            }
            match WaitForSingleObject(handle.0, 0) {
                WAIT_OBJECT_0 | WAIT_ABANDONED => Ok(Self(handle)),
                _ => Err("NATIVE_INPUT_BUSY".into()),
            }
        }
    }
}
impl Drop for InputLock {
    fn drop(&mut self) {
        unsafe {
            ReleaseMutex(self.0.0);
        }
    }
}
fn desktop_ready() -> Result<(), String> {
    unsafe {
        let desktop = OpenInputDesktop(0, 0, DESKTOP_READOBJECTS);
        if desktop.is_null() {
            return Err("DESKTOP_LOCKED_OR_SECURE".into());
        }
        let mut name = [0_u16; 256];
        let mut needed = 0;
        let ok = GetUserObjectInformationW(
            desktop,
            UOI_NAME,
            name.as_mut_ptr().cast(),
            (name.len() * 2) as u32,
            &mut needed,
        );
        CloseDesktop(desktop);
        if ok == 0 {
            return Err("DESKTOP_IDENTITY_UNAVAILABLE".into());
        }
        let end = name.iter().position(|c| *c == 0).unwrap_or(name.len());
        if String::from_utf16_lossy(&name[..end]) != "Default" {
            return Err("DESKTOP_LOCKED_OR_SECURE".into());
        }
        Ok(())
    }
}
fn input_guard(window: &Window) -> Result<(), String> {
    desktop_ready()?;
    if unsafe { GetAsyncKeyState(VK_ESCAPE as i32) } < 0 {
        return Err("USER_STOPPED: Escape is held".into());
    }
    if unsafe { GetForegroundWindow() } != window.id as HWND {
        return Err("FOREGROUND_CHANGED".into());
    }
    if Window::read(window.id)? != *window {
        return Err("WINDOW_IDENTITY_OR_GEOMETRY_CHANGED".into());
    }
    Ok(())
}
fn activate_window(window: &Window) -> Result<(), String> {
    use windows::Win32::{
        System::Com::{
            CLSCTX_INPROC_SERVER, COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx,
            CoUninitialize,
        },
        UI::Accessibility::{CUIAutomation8, IUIAutomation2},
    };
    desktop_ready()?;
    unsafe {
        if IsWindowEnabled(window.id as HWND) == 0 {
            return Err("WINDOW_BLOCKED_BY_MODAL: select its Chrome dialog from windows".into());
        }
        for code in [
            VK_ESCAPE, VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN, VK_LBUTTON, VK_RBUTTON,
        ] {
            if GetAsyncKeyState(code as i32) < 0 {
                return Err("PHYSICAL_INPUT_ACTIVE".into());
            }
        }
        SetForegroundWindow(window.id as HWND);
        if GetForegroundWindow() != window.id as HWND {
            let initialized = CoInitializeEx(None, COINIT_MULTITHREADED).is_ok();
            struct Apartment(bool);
            impl Drop for Apartment {
                fn drop(&mut self) {
                    if self.0 {
                        unsafe {
                            CoUninitialize();
                        }
                    }
                }
            }
            let _apartment = Apartment(initialized);
            let automation: IUIAutomation2 =
                CoCreateInstance(&CUIAutomation8, None, CLSCTX_INPROC_SERVER)
                    .map_err(|e| format!("FOCUS_UIA_UNAVAILABLE: {e}"))?;
            automation
                .SetConnectionTimeout(500)
                .map_err(|e| e.to_string())?;
            automation
                .SetTransactionTimeout(500)
                .map_err(|e| e.to_string())?;
            let element = automation
                .ElementFromHandle(windows::Win32::Foundation::HWND(window.id as HWND))
                .map_err(|e| e.to_string())?;
            element
                .SetFocus()
                .map_err(|e| format!("FOCUS_REQUIRED: {e}"))?;
        }
        let end = Instant::now() + Duration::from_millis(500);
        while GetForegroundWindow() != window.id as HWND && Instant::now() < end {
            std::thread::sleep(Duration::from_millis(10));
        }
        if GetForegroundWindow() != window.id as HWND {
            return Err("FOCUS_REQUIRED: activate the selected Chrome window".into());
        }
    }
    if Window::read(window.id)? != *window {
        return Err("WINDOW_CHANGED_DURING_ACTIVATION".into());
    }
    Ok(())
}
#[derive(Default)]
pub struct Native {
    observations: HashMap<String, Observation>,
    halted: bool,
}
fn mouse(flags: u32, data: u32) -> INPUT {
    INPUT {
        r#type: INPUT_MOUSE,
        Anonymous: INPUT_0 {
            mi: MOUSEINPUT {
                dx: 0,
                dy: 0,
                mouseData: data,
                dwFlags: flags,
                time: 0,
                dwExtraInfo: 0,
            },
        },
    }
}
fn key(code: u16, scan: u16, flags: u32) -> INPUT {
    INPUT {
        r#type: INPUT_KEYBOARD,
        Anonymous: INPUT_0 {
            ki: KEYBDINPUT {
                wVk: code,
                wScan: scan,
                dwFlags: flags,
                time: 0,
                dwExtraInfo: 0,
            },
        },
    }
}
fn send(inputs: &[INPUT]) -> Result<(), String> {
    unsafe {
        let count = SendInput(
            inputs.len() as u32,
            inputs.as_ptr(),
            mem::size_of::<INPUT>() as i32,
        );
        if count != inputs.len() as u32 {
            // Release only key/button downs present in the accepted prefix.
            // Never repeat a down event after a partial SendInput result.
            let cleanup = release_prefix(inputs, count as usize);
            let released = if cleanup.is_empty() {
                0
            } else {
                SendInput(
                    cleanup.len() as u32,
                    cleanup.as_ptr(),
                    mem::size_of::<INPUT>() as i32,
                )
            };
            return Err(format!(
                "INPUT_OUTCOME_UNKNOWN: accepted {count}/{} events; cleanup {released}/{}",
                inputs.len(),
                cleanup.len()
            ));
        }
    }
    Ok(())
}
fn release_prefix(inputs: &[INPUT], accepted: usize) -> Vec<INPUT> {
    let mut keys: Vec<(u16, u16, u32)> = Vec::new();
    let mut buttons = 0_u32;
    for input in inputs.iter().take(accepted) {
        unsafe {
            if input.r#type == INPUT_KEYBOARD {
                let value = input.Anonymous.ki;
                let identity = (value.wVk, value.wScan, value.dwFlags & !KEYEVENTF_KEYUP);
                if value.dwFlags & KEYEVENTF_KEYUP != 0 {
                    keys.retain(|k| *k != identity);
                } else if !keys.contains(&identity) {
                    keys.push(identity);
                }
            } else if input.r#type == INPUT_MOUSE {
                let flags = input.Anonymous.mi.dwFlags;
                for (down, up, bit) in [
                    (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 1),
                    (MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 2),
                    (MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 4),
                ] {
                    if flags & down != 0 {
                        buttons |= bit;
                    }
                    if flags & up != 0 {
                        buttons &= !bit;
                    }
                }
            }
        }
    }
    let mut releases: Vec<_> = keys
        .into_iter()
        .rev()
        .map(|(code, scan, flags)| key(code, scan, flags | KEYEVENTF_KEYUP))
        .collect();
    for (bit, flag) in [
        (1, MOUSEEVENTF_LEFTUP),
        (2, MOUSEEVENTF_RIGHTUP),
        (4, MOUSEEVENTF_MIDDLEUP),
    ] {
        if buttons & bit != 0 {
            releases.push(mouse(flag, 0));
        }
    }
    releases
}
struct MouseRelease(bool);
impl Drop for MouseRelease {
    fn drop(&mut self) {
        if self.0 {
            let release = mouse(MOUSEEVENTF_LEFTUP, 0);
            unsafe {
                SendInput(1, &release, mem::size_of::<INPUT>() as i32);
            }
        }
    }
}
fn screenshot(window: &Window, header_only: bool) -> Result<(String, i32, i32, i32), String> {
    unsafe {
        let [x, y, w, full_height] = window.rect;
        let h = if header_only {
            full_height.min(160)
        } else {
            full_height
        };
        let factor = (1600_f64 / w as f64).min(1200_f64 / h as f64).min(1.0);
        let width = (w as f64 * factor).floor() as i32;
        let height = (h as f64 * factor).floor() as i32;
        let screen = GetDC(ptr::null_mut());
        if screen.is_null() {
            return Err("SCREEN_UNAVAILABLE".into());
        }
        let dc = CreateCompatibleDC(screen);
        if dc.is_null() {
            ReleaseDC(ptr::null_mut(), screen);
            return Err("CAPTURE_DC_UNAVAILABLE".into());
        }
        let mut info: BITMAPINFO = mem::zeroed();
        info.bmiHeader.biSize = mem::size_of::<BITMAPINFOHEADER>() as u32;
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        let mut bits = ptr::null_mut();
        let bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &mut bits, ptr::null_mut(), 0);
        if bitmap.is_null() {
            DeleteDC(dc);
            ReleaseDC(ptr::null_mut(), screen);
            return Err("CAPTURE_BITMAP_UNAVAILABLE".into());
        }
        let previous = SelectObject(dc, bitmap);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, ptr::null_mut());
        let captured = StretchBlt(
            dc,
            0,
            0,
            width,
            height,
            screen,
            x,
            y,
            w,
            h,
            SRCCOPY | CAPTUREBLT,
        );
        let mut pixels = if captured != 0 {
            std::slice::from_raw_parts(bits.cast::<u8>(), width as usize * height as usize * 4)
                .to_vec()
        } else {
            Vec::new()
        };
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(ptr::null_mut(), screen);
        if captured == 0 {
            return Err("SCREEN_CAPTURE_FAILED".into());
        }
        for p in pixels.chunks_exact_mut(4) {
            p.swap(0, 2);
            p[3] = 255;
        }
        let mut bytes = Vec::new();
        {
            let mut encoder = png::Encoder::new(&mut bytes, width as u32, height as u32);
            encoder.set_color(png::ColorType::Rgba);
            encoder.set_depth(png::BitDepth::Eight);
            encoder.set_compression(png::Compression::Fast);
            encoder
                .write_header()
                .map_err(|e| e.to_string())?
                .write_image_data(&pixels)
                .map_err(|e| e.to_string())?;
        }
        if bytes.len() > 4 * 1024 * 1024 {
            return Err("NATIVE_IMAGE_LIMIT".into());
        }
        Ok((
            base64::engine::general_purpose::STANDARD.encode(bytes),
            width,
            height,
            h,
        ))
    }
}
impl Native {
    fn observe(&mut self, id: usize, header_only: bool, activate: bool) -> Result<Value, String> {
        unsafe {
            let before = Window::read(id)?;
            if activate && GetForegroundWindow() != id as HWND {
                activate_window(&before)?;
            }
            if GetForegroundWindow() != id as HWND {
                return Err("FOCUS_REQUIRED: bring the selected Chrome window forward".into());
            }
            let (image, width, height, source_height) = screenshot(&before, header_only)?;
            if Window::read(id)? != before || GetForegroundWindow() != id as HWND {
                return Err("WINDOW_CHANGED_DURING_CAPTURE".into());
            }
            self.observations
                .retain(|_, v| v.at.elapsed() < Duration::from_secs(30) && v.window.id != id);
            if self.observations.len() >= 8 {
                self.observations.clear();
            }
            let token = nonce()?;
            let metadata = before.json();
            self.observations.insert(
                token.clone(),
                Observation {
                    window: before,
                    at: Instant::now(),
                    width,
                    height,
                    source_height,
                    header_only,
                },
            );
            Ok(
                json!({"ok":true,"data":{"backend":"native_win32","window":metadata,"observation_id":token,"width":width,"height":height,"source_height":source_height,"header_only":header_only,"coordinate_space":"returned_image_pixels","ttl_ms":30000,"content_untrusted":true,"capture":"visible_desktop_crop; occluding windows may appear","devbox_required":false,"extension_required":false},"image":{"base64":image,"mime":"image/png"}}),
            )
        }
    }
    pub fn dispatch(&mut self, r: &Value) -> Value {
        if self.halted {
            return error(
                "USER_STOPPED",
                "Native input was stopped with Escape; resume through a fresh explicitly authorized session",
                false,
            );
        }
        let old =
            unsafe { SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) };
        let result = self.perform(r);
        if result
            .as_ref()
            .is_err_and(|(message, _)| message.starts_with("USER_STOPPED"))
        {
            self.halted = true;
            self.observations.clear();
        }
        if !old.is_null() {
            unsafe {
                SetThreadDpiAwarenessContext(old);
            }
        }
        result.unwrap_or_else(|(e, sent)| {
            error(
                if sent {
                    "NATIVE_OUTCOME_UNKNOWN"
                } else {
                    "NATIVE_REJECTED"
                },
                e,
                sent,
            )
        })
    }
    fn perform(&mut self, r: &Value) -> Result<Value, (String, bool)> {
        let object = r
            .as_object()
            .ok_or(("ARGUMENTS_MUST_BE_OBJECT".into(), false))?;
        const FIELDS: &[&str] = &[
            "action",
            "window_id",
            "observation_id",
            "header_only",
            "x",
            "y",
            "to_x",
            "to_y",
            "delta_x",
            "delta_y",
            "button",
            "text",
            "key",
        ];
        if object.keys().any(|key| !FIELDS.contains(&key.as_str())) {
            return Err(("UNKNOWN_NATIVE_ARGUMENT".into(), false));
        }
        for field in ["x", "y", "to_x", "to_y"] {
            if object.get(field).is_some_and(|v| {
                v.as_f64()
                    .is_none_or(|n| !n.is_finite() || n.abs() > 100000.0)
            }) {
                return Err(("INVALID_NATIVE_COORDINATE".into(), false));
            }
        }
        for field in ["delta_x", "delta_y"] {
            if object
                .get(field)
                .is_some_and(|v| v.as_i64().is_none_or(|n| n.unsigned_abs() > 20))
            {
                return Err((
                    "SCROLL_NOTCH_LIMIT: integral -20..20 required".into(),
                    false,
                ));
            }
        }
        if object.get("header_only").is_some_and(|v| !v.is_boolean()) {
            return Err(("INVALID_HEADER_ONLY".into(), false));
        }
        if object
            .get("button")
            .is_some_and(|v| !matches!(v.as_str(), Some("left" | "right" | "middle")))
        {
            return Err(("BUTTON_NOT_SUPPORTED".into(), false));
        }
        desktop_ready().map_err(|e| (e, false))?;
        let action = r["action"].as_str().unwrap_or("");
        if action == "windows" || action == "status" {
            return Ok(
                json!({"ok":true,"data":{"backend":"native_win32","windows":windows().iter().map(Window::json).collect::<Vec<_>>(),"devbox_required":false,"extension_required":false,"requires_fresh_observation":true}}),
            );
        }
        let _input_lock = InputLock::acquire().map_err(|e| (e, false))?;
        desktop_ready().map_err(|e| (e, false))?;
        if unsafe { GetAsyncKeyState(VK_ESCAPE as i32) } < 0 {
            return Err(("USER_STOPPED: Escape is held".into(), false));
        }
        let id = r["window_id"]
            .as_u64()
            .filter(|v| *v > 0 && *v <= usize::MAX as u64)
            .ok_or(("WINDOW_ID_REQUIRED".into(), false))? as usize;
        if action == "observe" {
            return self
                .observe(id, r["header_only"].as_bool().unwrap_or(false), true)
                .map_err(|e| (e, false));
        }
        if ![
            "click",
            "double_click",
            "move",
            "drag",
            "scroll",
            "type",
            "key",
        ]
        .contains(&action)
        {
            return Err(("UNSUPPORTED_NATIVE_ACTION".into(), false));
        }
        let token = r["observation_id"]
            .as_str()
            .ok_or(("OBSERVATION_REQUIRED".into(), false))?;
        let obs = self
            .observations
            .remove(token)
            .ok_or(("OBSERVATION_STALE".into(), false))?;
        self.observations.retain(|_, value| value.window.id != id);
        if obs.at.elapsed() > Duration::from_secs(30) {
            return Err(("OBSERVATION_EXPIRED".into(), false));
        }
        if obs.window.id != id {
            return Err(("OBSERVATION_WINDOW_MISMATCH".into(), false));
        }
        input_guard(&obs.window).map_err(|e| (e, false))?;
        let point = |x: &Value, y: &Value| -> Result<(i32, i32), (String, bool)> {
            let x = x.as_f64().ok_or(("X_REQUIRED".into(), false))?;
            let y = y.as_f64().ok_or(("Y_REQUIRED".into(), false))?;
            if !x.is_finite()
                || !y.is_finite()
                || x < 0.0
                || y < 0.0
                || x >= obs.width as f64
                || y >= obs.height as f64
            {
                return Err(("POINT_OUTSIDE_OBSERVATION".into(), false));
            }
            Ok((
                obs.window.rect[0]
                    + (x * obs.window.rect[2] as f64 / obs.width as f64).floor() as i32,
                obs.window.rect[1]
                    + (y * obs.source_height as f64 / obs.height as f64).floor() as i32,
            ))
        };
        // User-held modifiers and mouse buttons would make input ambiguous.
        for code in [
            VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN, VK_LBUTTON, VK_RBUTTON,
        ] {
            if unsafe { GetAsyncKeyState(code as i32) } < 0 {
                return Err((
                    "PHYSICAL_INPUT_ACTIVE: release held keys/buttons".into(),
                    false,
                ));
            }
        }
        let result = (|| -> Result<(), (String, bool)> {
            if ["click", "double_click", "move", "drag", "scroll"].contains(&action) {
                let (x, y) = point(&r["x"], &r["y"])?;
                if unsafe { GetAncestor(WindowFromPoint(POINT { x, y }), GA_ROOT) } != id as HWND {
                    return Err(("TARGET_OCCLUDED".into(), false));
                }
                if unsafe { SetCursorPos(x, y) } == 0 {
                    return Err(("POINTER_MOVE_FAILED".into(), false));
                }
                if action == "scroll" {
                    let dx = r["delta_x"].as_i64().unwrap_or(0);
                    let dy = r["delta_y"].as_i64().unwrap_or(if dx == 0 { 3 } else { 0 });
                    if dx == 0 && dy == 0 {
                        return Err(("SCROLL_NOTCH_LIMIT".into(), false));
                    }
                    let mut inputs = Vec::new();
                    if dx != 0 {
                        inputs.push(mouse(MOUSEEVENTF_HWHEEL, (dx * 120) as i32 as u32));
                    }
                    if dy != 0 {
                        inputs.push(mouse(MOUSEEVENTF_WHEEL, (-dy * 120) as i32 as u32));
                    }
                    send(&inputs).map_err(|e| (e, true))?;
                } else if action == "click" || action == "double_click" {
                    let (button_down, button_up) = match r["button"].as_str().unwrap_or("left") {
                        "left" => (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP),
                        "right" => (MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP),
                        "middle" => (MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP),
                        _ => return Err(("BUTTON_NOT_SUPPORTED".into(), false)),
                    };
                    let count = if action == "double_click" { 2 } else { 1 };
                    for _ in 0..count {
                        send(&[mouse(button_down, 0), mouse(button_up, 0)])
                            .map_err(|e| (e, true))?;
                    }
                } else if action == "drag" {
                    let (tx, ty) = point(&r["to_x"], &r["to_y"])?;
                    if unsafe { GetAncestor(WindowFromPoint(POINT { x: tx, y: ty }), GA_ROOT) }
                        != id as HWND
                    {
                        return Err(("DRAG_TARGET_OCCLUDED".into(), false));
                    }
                    send(&[mouse(MOUSEEVENTF_LEFTDOWN, 0)]).map_err(|e| (e, true))?;
                    let mut release = MouseRelease(true);
                    for n in 1..=8 {
                        input_guard(&obs.window).map_err(|e| (e, true))?;
                        unsafe {
                            if SetCursorPos(x + (tx - x) * n / 8, y + (ty - y) * n / 8) == 0 {
                                return Err(("DRAG_POINTER_MOVE_FAILED".into(), true));
                            }
                        }
                        std::thread::sleep(Duration::from_millis(15));
                    }
                    send(&[mouse(MOUSEEVENTF_LEFTUP, 0)]).map_err(|e| (e, true))?;
                    release.0 = false;
                }
            } else if action == "type" {
                let text = r["text"].as_str().ok_or(("TEXT_REQUIRED".into(), false))?;
                if text.len() > 16000 {
                    return Err(("TEXT_LIMIT".into(), false));
                }
                let units: Vec<_> = text.encode_utf16().collect();
                let mut sent = false;
                for chunk in units.chunks(128) {
                    input_guard(&obs.window).map_err(|e| (e, sent))?;
                    let mut inputs = Vec::with_capacity(chunk.len() * 2);
                    for unit in chunk {
                        inputs.push(key(0, *unit, KEYEVENTF_UNICODE));
                        inputs.push(key(0, *unit, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP));
                    }
                    send(&inputs).map_err(|e| (e, true))?;
                    sent = true;
                }
            } else if action == "key" {
                let text = r["key"].as_str().ok_or(("KEY_REQUIRED".into(), false))?;
                let parts: Vec<_> = text.split('+').collect();
                if parts.len() > 4 {
                    return Err(("KEY_COMBINATION_LIMIT".into(), false));
                }
                let mut codes = Vec::new();
                for part in parts {
                    let code = match part {
                        "Ctrl" | "Control" => VK_CONTROL,
                        "Shift" => VK_SHIFT,
                        "Alt" => VK_MENU,
                        "Enter" | "Return" => VK_RETURN,
                        "Tab" => VK_TAB,
                        "Escape" => VK_ESCAPE,
                        "Backspace" => VK_BACK,
                        "Delete" => VK_DELETE,
                        "PageDown" => VK_NEXT,
                        "PageUp" => VK_PRIOR,
                        "Home" => VK_HOME,
                        "End" => VK_END,
                        "ArrowDown" => VK_DOWN,
                        "ArrowUp" => VK_UP,
                        "ArrowLeft" => VK_LEFT,
                        "ArrowRight" => VK_RIGHT,
                        "Space" => VK_SPACE,
                        s if s.len() == 1 && s.as_bytes()[0].is_ascii_alphanumeric() => {
                            s.to_ascii_uppercase().as_bytes()[0] as u16
                        }
                        _ => return Err(("KEY_NOT_SUPPORTED".into(), false)),
                    };
                    codes.push(code);
                }
                if (codes.contains(&VK_CONTROL) && codes.contains(&VK_ESCAPE))
                    || (codes.contains(&VK_MENU)
                        && (codes.contains(&VK_TAB) || codes.contains(&VK_ESCAPE)))
                    || (codes.contains(&VK_CONTROL)
                        && codes.contains(&VK_MENU)
                        && codes.contains(&VK_DELETE))
                {
                    return Err(("SYSTEM_SHORTCUT_OUTSIDE_CHROME_SCOPE".into(), false));
                }
                let mut inputs = Vec::new();
                for code in &codes {
                    inputs.push(key(*code, 0, 0));
                }
                for code in codes.iter().rev() {
                    inputs.push(key(*code, 0, KEYEVENTF_KEYUP));
                }
                send(&inputs).map_err(|e| (e, true))?;
            }
            Ok(())
        })();
        result?;
        std::thread::sleep(Duration::from_millis(120));
        let mut observed = self
            .observe(id, obs.header_only, false)
            .map_err(|e| (e, true))?;
        observed["data"]["input_sent"] =
            json!(action != "type" || !r["text"].as_str().unwrap_or("").is_empty());
        observed["data"]["movement_requires_visual_verification"] = json!(true);
        observed["data"]["effect_verified"] = json!(false);
        Ok(observed)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn partial_input_cleanup_only_releases_accepted_downs() {
        let chord = [
            key(VK_CONTROL, 0, 0),
            key(65, 0, 0),
            key(65, 0, KEYEVENTF_KEYUP),
            key(VK_CONTROL, 0, KEYEVENTF_KEYUP),
        ];
        assert!(release_prefix(&chord, 0).is_empty());
        let cleanup = release_prefix(&chord, 2);
        assert_eq!(cleanup.len(), 2);
        unsafe {
            assert_eq!(cleanup[0].Anonymous.ki.wVk, 65);
            assert_eq!(cleanup[1].Anonymous.ki.wVk, VK_CONTROL);
            assert_eq!(cleanup[0].Anonymous.ki.dwFlags, KEYEVENTF_KEYUP);
        }
        assert!(release_prefix(&chord, 4).is_empty());
        let click = [mouse(MOUSEEVENTF_LEFTDOWN, 0), mouse(MOUSEEVENTF_LEFTUP, 0)];
        assert_eq!(release_prefix(&click, 1).len(), 1);
        assert!(release_prefix(&click, 2).is_empty());
    }
    #[test]
    fn unicode_cleanup_preserves_scan_code_and_never_retypes() {
        let input = [
            key(0, 0xd83c, KEYEVENTF_UNICODE),
            key(0, 0xd83c, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
        ];
        let cleanup = release_prefix(&input, 1);
        assert_eq!(cleanup.len(), 1);
        unsafe {
            assert_eq!(cleanup[0].Anonymous.ki.wScan, 0xd83c);
            assert_eq!(
                cleanup[0].Anonymous.ki.dwFlags,
                KEYEVENTF_UNICODE | KEYEVENTF_KEYUP
            );
        }
    }
}
