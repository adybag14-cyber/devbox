#[cfg(not(target_os = "macos"))]
use std::collections::{HashMap, HashSet, VecDeque};
use std::{
    path::{Path, PathBuf},
    time::Duration,
};

use anyhow::{Context, Result, bail};
use serde_json::{Value, json};
use tokio::fs;
use tokio_util::sync::CancellationToken;

#[cfg(not(target_os = "macos"))]
use crate::process::ProcessError;
use crate::process::{ProcessOptions, ProcessOutput, spawn_process};

const COMMAND_TIMEOUT: Duration = Duration::from_secs(20);
const CAPTURE_CHARS: usize = 128_000;
const PNG_SIGNATURE: &[u8] = &[0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];

#[derive(Debug, Clone)]
pub struct NativeCapture {
    pub image: Vec<u8>,
    pub mime_type: &'static str,
    pub metadata: Value,
}

#[cfg(not(target_os = "macos"))]
#[derive(Debug, Clone)]
struct WindowCandidate {
    window_id: String,
    owner_pid: u32,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
}

/// Capture the full macOS host display using the native screenshot backend.
///
/// # Errors
/// Returns when capture fails or PNG output is invalid.
#[cfg(target_os = "macos")]
pub async fn capture_display(
    quality: u8,
    cancellation: CancellationToken,
) -> Result<NativeCapture> {
    capture_macos_display(quality, &cancellation).await
}

/// Capture the full Linux host display using the configured native screenshot backend.
///
/// # Errors
/// Returns when no graphical session/capture utility exists, the command fails, or PNG output is invalid.
#[cfg(not(target_os = "macos"))]
pub async fn capture_display(
    quality: u8,
    cancellation: CancellationToken,
) -> Result<NativeCapture> {
    capture_linux_display(quality, &cancellation).await
}

/// Capture the best visible macOS window for a PID/process tree.
///
/// # Errors
/// Returns for missing processes/windows/utilities, capture failures, or invalid PNG output.
#[cfg(target_os = "macos")]
pub async fn capture_program(
    pid: u32,
    quality: u8,
    include_process_tree: bool,
    cancellation: CancellationToken,
) -> Result<NativeCapture> {
    if pid == 0 {
        bail!("pid must be a positive process ID.");
    }
    capture_macos_program(pid, quality, include_process_tree, &cancellation).await
}

/// Capture the best visible Linux window for a PID/process tree.
///
/// # Errors
/// Returns for missing processes/windows/utilities, capture failures, or invalid PNG output.
#[cfg(not(target_os = "macos"))]
pub async fn capture_program(
    pid: u32,
    quality: u8,
    include_process_tree: bool,
    cancellation: CancellationToken,
) -> Result<NativeCapture> {
    if pid == 0 {
        bail!("pid must be a positive process ID.");
    }
    capture_linux_program(pid, quality, include_process_tree, &cancellation).await
}

#[cfg(not(target_os = "macos"))]
async fn capture_linux_display(
    quality: u8,
    cancellation: &CancellationToken,
) -> Result<NativeCapture> {
    assert_linux_session()?;
    let (_temp_dir, path) = temporary_png_target("display")?;
    let (tool, args) = select_linux_display_backend(&path, cancellation).await?;
    run_command(&tool, args, cancellation.child_token()).await?;
    let image = read_valid_png(&path).await?;
    Ok(NativeCapture {
        image,
        mime_type: "image/png",
        metadata: json!({
            "capture_mode": "full_display",
            "capture_method": tool,
            "quality": quality,
        }),
    })
}

#[cfg(not(target_os = "macos"))]
async fn capture_linux_program(
    pid: u32,
    quality: u8,
    include_process_tree: bool,
    cancellation: &CancellationToken,
) -> Result<NativeCapture> {
    assert_linux_session()?;
    let processes = linux_process_table(cancellation).await?;
    let process_name = processes
        .get(&pid)
        .map(|value| value.1.clone())
        .with_context(|| format!("Linux process {pid} does not exist."))?;
    let candidate_pids = collect_process_tree(&processes, pid, include_process_tree);
    let window = find_linux_window(&candidate_pids, cancellation).await?.with_context(|| {
        format!(
            "Process {pid} ({process_name}) and its visible child processes have no discoverable top-level X11 window."
        )
    })?;
    let (_temp_dir, path) = temporary_png_target("window")?;
    let method = if command_available("import", cancellation).await {
        run_command(
            "import",
            vec![
                "-window".to_owned(),
                window.window_id.clone(),
                path_text(&path),
            ],
            cancellation.child_token(),
        )
        .await?;
        "import"
    } else if command_available("gnome-screenshot", cancellation).await {
        run_command(
            "gnome-screenshot",
            vec!["-w".to_owned(), "-f".to_owned(), path_text(&path)],
            cancellation.child_token(),
        )
        .await?;
        "gnome-screenshot"
    } else {
        bail!(
            "No supported Linux window screenshot tool is installed. Install ImageMagick import or gnome-screenshot."
        );
    };
    let image = read_valid_png(&path).await?;
    Ok(NativeCapture {
        image,
        mime_type: "image/png",
        metadata: json!({
            "capture_mode": "program_pid",
            "capture_method": method,
            "pid": pid,
            "process_name": process_name,
            "window_owner_pid": window.owner_pid,
            "process_tree_fallback": window.owner_pid != pid,
            "candidate_pid_count": candidate_pids.len(),
            "window_id": window.window_id,
            "left": window.x,
            "top": window.y,
            "width": window.width,
            "height": window.height,
            "quality": quality,
        }),
    })
}

#[cfg(target_os = "macos")]
async fn capture_macos_display(
    quality: u8,
    cancellation: &CancellationToken,
) -> Result<NativeCapture> {
    let (_temp_dir, path) = temporary_png_target("display")?;
    run_command(
        "screencapture",
        vec!["-x".to_owned(), path_text(&path)],
        cancellation.child_token(),
    )
    .await?;
    let image = read_valid_png(&path).await?;
    Ok(NativeCapture {
        image,
        mime_type: "image/png",
        metadata: json!({
            "capture_mode": "full_display",
            "capture_method": "screencapture",
            "quality": quality,
        }),
    })
}

#[cfg(target_os = "macos")]
async fn capture_macos_program(
    pid: u32,
    quality: u8,
    include_process_tree: bool,
    cancellation: &CancellationToken,
) -> Result<NativeCapture> {
    let process_name = macos_process_name(pid, cancellation).await?;
    let window = macos_window_metadata(pid, include_process_tree, cancellation).await?;
    let window_id = window["window_id"]
        .as_u64()
        .context("macOS Quartz window discovery omitted window_id")?;
    let owner_pid =
        u32::try_from(window["owner_pid"].as_u64().unwrap_or(u64::from(pid))).unwrap_or(pid);
    let (_temp_dir, path) = temporary_png_target("window")?;
    run_command(
        "screencapture",
        vec![
            "-x".to_owned(),
            "-l".to_owned(),
            window_id.to_string(),
            path_text(&path),
        ],
        cancellation.child_token(),
    )
    .await?;
    let image = read_valid_png(&path).await?;
    Ok(NativeCapture {
        image,
        mime_type: "image/png",
        metadata: json!({
            "capture_mode": "program_pid",
            "capture_method": "screencapture(window-id)",
            "pid": pid,
            "process_name": process_name,
            "window_owner_pid": owner_pid,
            "process_tree_fallback": owner_pid != pid,
            "candidate_pid_count": window["candidate_pid_count"].clone(),
            "window_id": window_id,
            "left": window["left"].clone(),
            "top": window["top"].clone(),
            "width": window["width"].clone(),
            "height": window["height"].clone(),
            "quality": quality,
        }),
    })
}

#[cfg(not(target_os = "macos"))]
fn assert_linux_session() -> Result<()> {
    if std::env::var_os("DISPLAY").is_none() && std::env::var_os("WAYLAND_DISPLAY").is_none() {
        bail!("No DISPLAY or WAYLAND_DISPLAY is available for Linux screen capture.");
    }
    Ok(())
}

#[cfg(not(target_os = "macos"))]
async fn select_linux_display_backend(
    path: &Path,
    cancellation: &CancellationToken,
) -> Result<(String, Vec<String>)> {
    let wayland = std::env::var_os("WAYLAND_DISPLAY").is_some();
    if wayland && command_available("grim", cancellation).await {
        return Ok(("grim".to_owned(), vec![path_text(path)]));
    }
    if command_available("gnome-screenshot", cancellation).await {
        return Ok((
            "gnome-screenshot".to_owned(),
            vec!["-f".to_owned(), path_text(path)],
        ));
    }
    if command_available("scrot", cancellation).await {
        return Ok(("scrot".to_owned(), vec![path_text(path)]));
    }
    if command_available("import", cancellation).await {
        return Ok((
            "import".to_owned(),
            vec!["-window".to_owned(), "root".to_owned(), path_text(path)],
        ));
    }
    bail!(
        "No supported Linux screenshot tool is installed. Install grim, gnome-screenshot, scrot, or ImageMagick import."
    )
}

#[cfg(not(target_os = "macos"))]
async fn linux_process_table(
    cancellation: &CancellationToken,
) -> Result<HashMap<u32, (u32, String)>> {
    let mut result = HashMap::new();
    let mut entries = fs::read_dir("/proc").await.context("list /proc")?;
    while let Some(entry) = entries.next_entry().await? {
        if cancellation.is_cancelled() {
            bail!("Screen capture cancelled while scanning Linux processes.");
        }
        let Some(name) = entry.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        let Ok(pid) = name.parse::<u32>() else {
            continue;
        };
        let Ok(stat) = fs::read_to_string(entry.path().join("stat")).await else {
            continue;
        };
        let Some((parent, executable)) = parse_proc_stat(&stat) else {
            continue;
        };
        result.insert(pid, (parent, executable));
    }
    Ok(result)
}

#[cfg(not(target_os = "macos"))]
fn parse_proc_stat(stat: &str) -> Option<(u32, String)> {
    let open = stat.find('(')?;
    let close = stat.rfind(')')?;
    if close <= open {
        return None;
    }
    let executable = stat[open + 1..close].to_owned();
    let after = stat.get(close + 2..)?;
    let fields = after.split_whitespace().collect::<Vec<_>>();
    let parent = fields.get(1)?.parse::<u32>().ok()?;
    Some((parent, executable))
}

#[cfg(not(target_os = "macos"))]
fn collect_process_tree(
    processes: &HashMap<u32, (u32, String)>,
    root: u32,
    include_tree: bool,
) -> HashSet<u32> {
    let mut result = HashSet::from([root]);
    if !include_tree {
        return result;
    }
    let mut queue = VecDeque::from([root]);
    while let Some(parent) = queue.pop_front() {
        for (&pid, (ppid, _)) in processes {
            if *ppid == parent && result.insert(pid) {
                queue.push_back(pid);
            }
        }
    }
    result
}

#[cfg(not(target_os = "macos"))]
async fn find_linux_window(
    candidate_pids: &HashSet<u32>,
    cancellation: &CancellationToken,
) -> Result<Option<WindowCandidate>> {
    let mut window_ids = Vec::new();
    if command_available("xdotool", cancellation).await {
        for pid in candidate_pids {
            match run_command(
                "xdotool",
                vec![
                    "search".to_owned(),
                    "--onlyvisible".to_owned(),
                    "--pid".to_owned(),
                    pid.to_string(),
                ],
                cancellation.child_token(),
            )
            .await
            {
                Ok(output) => {
                    for id in output
                        .stdout
                        .lines()
                        .map(str::trim)
                        .filter(|line| !line.is_empty())
                    {
                        window_ids.push((id.to_owned(), *pid));
                    }
                }
                Err(error) if is_no_match_error(&error) => {}
                Err(error) => return Err(error),
            }
        }
    } else if command_available("wmctrl", cancellation).await {
        let output =
            run_command("wmctrl", vec!["-lp".to_owned()], cancellation.child_token()).await?;
        window_ids.extend(parse_wmctrl_windows(&output.stdout, candidate_pids));
    } else {
        bail!("Window discovery requires xdotool or wmctrl on Linux.");
    }

    let mut best = None;
    let mut best_area = 0_u64;
    for (window_id, owner_pid) in window_ids {
        let Ok(geometry) = xwininfo_geometry(&window_id, cancellation).await else {
            continue;
        };
        if geometry.width < 32 || geometry.height < 32 {
            continue;
        }
        let area = u64::from(geometry.width).saturating_mul(u64::from(geometry.height));
        if area > best_area {
            best_area = area;
            best = Some(WindowCandidate {
                window_id,
                owner_pid,
                x: geometry.x,
                y: geometry.y,
                width: geometry.width,
                height: geometry.height,
            });
        }
    }
    Ok(best)
}

#[cfg(not(target_os = "macos"))]
fn parse_wmctrl_windows(stdout: &str, candidate_pids: &HashSet<u32>) -> Vec<(String, u32)> {
    stdout
        .lines()
        .filter_map(|line| {
            let fields = line.split_whitespace().collect::<Vec<_>>();
            let id = fields.first()?;
            let pid = fields.get(2)?.parse::<u32>().ok()?;
            candidate_pids
                .contains(&pid)
                .then(|| ((*id).to_owned(), pid))
        })
        .collect()
}

#[cfg(not(target_os = "macos"))]
async fn xwininfo_geometry(
    window_id: &str,
    cancellation: &CancellationToken,
) -> Result<WindowCandidate> {
    let output = run_command(
        "xwininfo",
        vec!["-id".to_owned(), window_id.to_owned()],
        cancellation.child_token(),
    )
    .await?;
    let value = parse_xwininfo(&output.stdout)?;
    Ok(WindowCandidate {
        window_id: window_id.to_owned(),
        owner_pid: 0,
        x: value.0,
        y: value.1,
        width: value.2,
        height: value.3,
    })
}

#[cfg(not(target_os = "macos"))]
fn parse_xwininfo(stdout: &str) -> Result<(i32, i32, u32, u32)> {
    let read = |label: &str| -> Result<String> {
        stdout
            .lines()
            .find_map(|line| line.trim().strip_prefix(label).map(str::trim))
            .map(str::to_owned)
            .with_context(|| format!("xwininfo output omitted {label}"))
    };
    Ok((
        read("Absolute upper-left X:")?.parse()?,
        read("Absolute upper-left Y:")?.parse()?,
        read("Width:")?.parse()?,
        read("Height:")?.parse()?,
    ))
}

#[cfg(target_os = "macos")]
async fn macos_process_name(pid: u32, cancellation: &CancellationToken) -> Result<String> {
    let output = run_command(
        "ps",
        vec![
            "-p".to_owned(),
            pid.to_string(),
            "-o".to_owned(),
            "comm=".to_owned(),
        ],
        cancellation.child_token(),
    )
    .await?;
    let name = Path::new(output.stdout.trim())
        .file_name()
        .map(|value| value.to_string_lossy().into_owned())
        .unwrap_or_default();
    if name.is_empty() {
        bail!("macOS process {pid} does not exist.");
    }
    Ok(name)
}

#[cfg(target_os = "macos")]
async fn macos_window_metadata(
    pid: u32,
    include_process_tree: bool,
    cancellation: &CancellationToken,
) -> Result<Value> {
    let script = r"import json, subprocess, sys
try:
    from Quartz import CGWindowListCopyWindowInfo, kCGWindowListOptionOnScreenOnly, kCGNullWindowID
except Exception as exc:
    print(json.dumps({'error': 'pyobjc Quartz is required for macOS program-window capture: ' + str(exc)}))
    sys.exit(2)
root = int(sys.argv[1]); include_tree = sys.argv[2].lower() == 'true'
pids = {root}
if include_tree:
    changed = True
    while changed:
        changed = False
        output = subprocess.check_output(['ps', '-axo', 'pid=,ppid='], text=True)
        for line in output.splitlines():
            fields = line.split()
            if len(fields) != 2: continue
            child, parent = map(int, fields)
            if parent in pids and child not in pids:
                pids.add(child); changed = True
best = None
for item in CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID):
    owner = int(item.get('kCGWindowOwnerPID', -1))
    if owner not in pids: continue
    bounds = item.get('kCGWindowBounds') or {}
    width = int(bounds.get('Width', 0)); height = int(bounds.get('Height', 0))
    if width < 32 or height < 32: continue
    area = width * height
    if best is None or area > best['area']:
        best = {'window_id': int(item.get('kCGWindowNumber')), 'owner_pid': owner,
                'left': int(bounds.get('X', 0)), 'top': int(bounds.get('Y', 0)),
                'width': width, 'height': height, 'area': area,
                'candidate_pid_count': len(pids)}
if best is None:
    print(json.dumps({'error': 'No visible Quartz window found for the requested process tree.'})); sys.exit(3)
best.pop('area', None); print(json.dumps(best))";
    let output = run_command(
        "python3",
        vec![
            "-c".to_owned(),
            script.to_owned(),
            pid.to_string(),
            include_process_tree.to_string(),
        ],
        cancellation.child_token(),
    )
    .await?;
    serde_json::from_str(output.stdout.trim()).context("parse macOS Quartz window metadata")
}

#[cfg(not(target_os = "macos"))]
async fn command_available(program: &str, cancellation: &CancellationToken) -> bool {
    match run_command(
        program,
        version_probe_args(program),
        cancellation.child_token(),
    )
    .await
    {
        Ok(_) => true,
        Err(error) => !is_missing_program_error(&error),
    }
}

#[cfg(not(target_os = "macos"))]
fn version_probe_args(program: &str) -> Vec<String> {
    match program {
        "gnome-screenshot" | "grim" | "scrot" | "xdotool" | "wmctrl" | "xwininfo" => {
            vec!["--version".to_owned()]
        }
        "import" => vec!["-version".to_owned()],
        _ => vec!["--version".to_owned()],
    }
}

async fn run_command(
    program: &str,
    args: Vec<String>,
    cancellation: CancellationToken,
) -> Result<ProcessOutput> {
    spawn_process(
        program,
        &args,
        ProcessOptions {
            timeout: Some(COMMAND_TIMEOUT),
            max_capture_chars: Some(CAPTURE_CHARS),
            ..ProcessOptions::default()
        },
        cancellation,
    )
    .await
    .map_err(anyhow::Error::new)
}

#[cfg(not(target_os = "macos"))]
fn is_missing_program_error(error: &anyhow::Error) -> bool {
    let text = process_error_text(error).to_ascii_lowercase();
    text.contains("no such file") || text.contains("not found") || text.contains("cannot find")
}

#[cfg(not(target_os = "macos"))]
fn is_no_match_error(error: &anyhow::Error) -> bool {
    error
        .downcast_ref::<ProcessError>()
        .and_then(|process| process.exit_code)
        == Some(1)
}

#[cfg(not(target_os = "macos"))]
fn process_error_text(error: &anyhow::Error) -> String {
    if let Some(process) = error.downcast_ref::<ProcessError>() {
        format!("{} {} {}", process.message, process.stdout, process.stderr)
    } else {
        error.to_string()
    }
}

async fn read_valid_png(path: &Path) -> Result<Vec<u8>> {
    let image = fs::read(path)
        .await
        .with_context(|| format!("read captured PNG {}", path.display()))?;
    if !image.starts_with(PNG_SIGNATURE) {
        bail!("Screen capture did not return a valid PNG image.");
    }
    Ok(image)
}

fn temporary_png_target(kind: &str) -> Result<(tempfile::TempDir, PathBuf)> {
    let directory = tempfile::Builder::new()
        .prefix(&format!("devbox-rust-{kind}-"))
        .tempdir()
        .context("create private capture temporary directory")?;
    let path = directory.path().join("capture.png");
    Ok((directory, path))
}

fn path_text(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

#[cfg(all(test, not(target_os = "macos")))]
mod tests {
    use super::*;

    #[test]
    fn capture_temp_target_is_inside_a_private_random_directory() {
        let (directory, path) = temporary_png_target("test").expect("private temp target");
        assert_eq!(path.parent(), Some(directory.path()));
        assert_eq!(
            path.file_name().and_then(|value| value.to_str()),
            Some("capture.png")
        );
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt as _;
            let mode = std::fs::metadata(directory.path())
                .expect("temp dir metadata")
                .permissions()
                .mode();
            assert_eq!(mode & 0o077, 0);
        }
    }

    #[test]
    fn proc_stat_parser_handles_spaces_in_process_name() {
        let parsed = parse_proc_stat("123 (my process) S 42 0 0 0").expect("parse stat");
        assert_eq!(parsed.0, 42);
        assert_eq!(parsed.1, "my process");
    }

    #[test]
    fn wmctrl_parser_filters_candidate_pids() {
        let pids = HashSet::from([100_u32]);
        let result =
            parse_wmctrl_windows("0x01  0 100 host title\n0x02  0 200 host other\n", &pids);
        assert_eq!(result, [("0x01".to_owned(), 100)]);
    }

    #[test]
    fn xwininfo_parser_extracts_geometry() {
        let parsed = parse_xwininfo(
            "Absolute upper-left X:  12\nAbsolute upper-left Y:  34\nWidth: 800\nHeight: 600\n",
        )
        .expect("parse geometry");
        assert_eq!(parsed, (12, 34, 800, 600));
    }
}
