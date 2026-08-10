use std::{
    path::{Path, PathBuf},
    sync::Arc,
    time::Duration,
};

use anyhow::{Context, Result};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use time::OffsetDateTime;
use tokio::{fs, io::AsyncReadExt};
use tokio_util::sync::CancellationToken;

use crate::{
    Config,
    runtime::{RuntimeExecError, RuntimeExecutor, ShellRequest},
};

const MAX_HASH_BYTES: u64 = 8 * 1024 * 1024;
const PREVIEW_CHARS: usize = 400;
const POWERSHELL_EXTENSIONS: &[&str] = &[".ps1", ".psm1", ".psd1"];
const MOJIBAKE_MARKERS: &[&str] = &["â€”", "â€“", "â€œ", "â€�", "â€˜", "â€™", "â€¦", "â€¢", "ðŸ"];

#[derive(Debug, Clone)]
pub struct InspectFileRequest {
    pub path: String,
    pub working_dir: PathBuf,
    pub resolved_path: Option<PathBuf>,
    pub max_bytes: usize,
}

/// Inspect exact host-file bytes and text/syntax integrity without mutating the file.
///
/// # Errors
/// Returns filesystem or metadata errors other than a missing path.
pub async fn inspect_host_file(
    config: Arc<Config>,
    runtime: Arc<RuntimeExecutor>,
    request: InspectFileRequest,
    cancellation: CancellationToken,
) -> Result<Value> {
    let resolved = request
        .resolved_path
        .clone()
        .unwrap_or_else(|| resolve_host_path(&request.path, &request.working_dir));
    let extension = extension_lower(&resolved);
    let mut info = base_info(&request.path, &resolved, &extension);
    let metadata = match fs::metadata(&resolved).await {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            push_string(
                &mut info,
                "observations",
                "The path does not exist on disk.",
            );
            push_string(
                &mut info,
                "repair_hints",
                "Verify the path or recreate the file before retrying the command.",
            );
            return Ok(info);
        }
        Err(error) => return Err(error).with_context(|| format!("inspect {}", resolved.display())),
    };
    apply_metadata(&mut info, &metadata);
    if !metadata.is_file() {
        push_string(
            &mut info,
            "observations",
            "The path exists but is not a regular file.",
        );
        return Ok(info);
    }
    inspect_regular_file(
        InspectionContext {
            config,
            runtime,
            request,
            resolved,
            extension,
        },
        metadata,
        info,
        cancellation,
    )
    .await
}

struct InspectionContext {
    config: Arc<Config>,
    runtime: Arc<RuntimeExecutor>,
    request: InspectFileRequest,
    resolved: PathBuf,
    extension: String,
}

fn base_info(requested: &str, resolved: &Path, extension: &str) -> Value {
    json!({
        "requested_path": requested,
        "resolved_path": resolved.to_string_lossy(),
        "extension": extension,
        "exists": false,
        "is_file": false,
        "likely_corrupted_on_disk": false,
        "syntax_invalid": false,
        "observations": [],
        "repair_hints": [],
    })
}

fn apply_metadata(info: &mut Value, metadata: &std::fs::Metadata) {
    info["exists"] = json!(true);
    info["is_file"] = json!(metadata.is_file());
    info["size_bytes"] = json!(metadata.len());
    info["last_modified_utc"] = metadata
        .modified()
        .ok()
        .map(OffsetDateTime::from)
        .map(format_javascript_iso_utc)
        .map_or(Value::Null, Value::String);
}

fn format_javascript_iso_utc(value: OffsetDateTime) -> String {
    let nanos = value.unix_timestamp_nanos();
    let rounded_millis = if nanos >= 0 {
        (nanos + 500_000) / 1_000_000
    } else {
        (nanos - 500_000) / 1_000_000
    };
    let rounded =
        OffsetDateTime::from_unix_timestamp_nanos(rounded_millis * 1_000_000).unwrap_or(value);
    format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        rounded.year(),
        rounded.month() as u8,
        rounded.day(),
        rounded.hour(),
        rounded.minute(),
        rounded.second(),
        rounded.nanosecond() / 1_000_000,
    )
}

async fn inspect_regular_file(
    context: InspectionContext,
    metadata: std::fs::Metadata,
    mut info: Value,
    cancellation: CancellationToken,
) -> Result<Value> {
    let sample = read_prefix(&context.resolved, context.request.max_bytes.max(1)).await?;
    let bom = detect_bom(&sample);
    let binary_format = detect_binary_format(&sample);
    info["sampled_bytes"] = json!(sample.len());
    info["sha256"] = if metadata.len() <= MAX_HASH_BYTES {
        Value::String(hash_file_sha256(&context.resolved).await?)
    } else {
        Value::Null
    };
    info["bom"] = bom.map_or(Value::Null, |value| json!(value));
    info["binary_format"] = binary_format.map_or(Value::Null, |value| json!(value));
    info["text_inspection_skipped"] = json!(binary_format.is_some());
    if let Some(format) = binary_format {
        info["utf8_valid"] = Value::Null;
        push_string(
            &mut info,
            "observations",
            &format!(
                "Skipped text-corruption tests because the file has {} binary magic.",
                format.to_ascii_uppercase()
            ),
        );
        return Ok(info);
    }

    inspect_text_sample(&mut info, &sample, bom);
    inspect_optional_powershell_syntax(&context, &mut info, cancellation).await;
    finish_integrity_assessment(&mut info);
    Ok(info)
}

async fn inspect_optional_powershell_syntax(
    context: &InspectionContext,
    info: &mut Value,
    cancellation: CancellationToken,
) {
    if !context.config.platform.is_windows
        || !POWERSHELL_EXTENSIONS.contains(&context.extension.as_str())
    {
        return;
    }
    let syntax = inspect_powershell_syntax(
        context.runtime.clone(),
        &context.resolved,
        &context.request.working_dir,
        cancellation,
    )
    .await;
    let syntax_invalid = syntax["parse_ok"].as_bool() == Some(false);
    info["powershell_syntax"] = syntax;
    info["syntax_invalid"] = json!(syntax_invalid);
    if syntax_invalid {
        let count = info["powershell_syntax"]["error_count"]
            .as_u64()
            .unwrap_or(1);
        push_string(
            info,
            "observations",
            &format!("PowerShell reported {count} parse error(s) for this file."),
        );
    }
}

fn finish_integrity_assessment(info: &mut Value) {
    let corrupted = info["utf8_valid"].as_bool() == Some(false)
        || info["null_byte_count"].as_u64().unwrap_or_default() > 0
        || info["suspicious_mojibake_count"]
            .as_u64()
            .unwrap_or_default()
            > 0
        || info["replacement_character_count"]
            .as_u64()
            .unwrap_or_default()
            > 0;
    info["likely_corrupted_on_disk"] = json!(corrupted);
    if corrupted {
        push_string(
            info,
            "repair_hints",
            "Read the exact bytes with windows_host_read_large_file before editing so you do not lose evidence of the corruption.",
        );
        push_string(
            info,
            "repair_hints",
            "Rewrite the file from a clean UTF-8 or exact-byte payload with windows_host_write_large_file, then rerun the original host command.",
        );
    } else if info["syntax_invalid"].as_bool() == Some(true) {
        push_string(
            info,
            "repair_hints",
            "The file appears to be syntactically invalid but not obviously byte-corrupted; inspect the script text and fix the source logic.",
        );
    }
}

fn inspect_text_sample(info: &mut Value, sample: &[u8], bom: Option<&str>) {
    let strict = std::str::from_utf8(sample);
    let utf8_valid = strict.is_ok();
    let decoded = strict.map_or_else(
        |_| String::from_utf8_lossy(sample).into_owned(),
        str::to_owned,
    );
    let null_count: usize = sample.iter().map(|byte| usize::from(*byte == 0)).sum();
    let replacement_count = decoded.matches('\u{fffd}').count();
    let mut mojibake_markers = Vec::new();
    let mut mojibake_count = 0_usize;
    for marker in MOJIBAKE_MARKERS {
        let count = decoded.matches(marker).count();
        if count > 0 {
            mojibake_markers.push((*marker).to_owned());
            mojibake_count = mojibake_count.saturating_add(count);
        }
    }
    info["utf8_valid"] = json!(utf8_valid);
    info["null_byte_count"] = json!(null_count);
    info["replacement_character_count"] = json!(replacement_count);
    info["suspicious_mojibake_count"] = json!(mojibake_count);
    info["suspicious_mojibake_markers"] = json!(mojibake_markers);
    info["line_endings"] = json!(detect_line_endings(&decoded));
    info["preview"] = Value::String(decoded.chars().take(PREVIEW_CHARS).collect());

    if let Some(bom) = bom {
        push_string(
            info,
            "observations",
            &format!("The file starts with a {} BOM.", bom.to_ascii_uppercase()),
        );
    }
    if !utf8_valid {
        push_string(
            info,
            "observations",
            "The sampled bytes are not valid UTF-8.",
        );
    }
    if null_count > 0 {
        push_string(
            info,
            "observations",
            &format!(
                "The sampled bytes contain {null_count} NUL byte(s), which is unusual for a text source file."
            ),
        );
    }
    if mojibake_count > 0 {
        push_string(
            info,
            "observations",
            &format!(
                "The sampled text contains {mojibake_count} suspicious mojibake marker(s): {}.",
                mojibake_markers.join(", ")
            ),
        );
    }
}

async fn inspect_powershell_syntax(
    runtime: Arc<RuntimeExecutor>,
    path: &Path,
    working_dir: &Path,
    cancellation: CancellationToken,
) -> Value {
    let escaped = path.to_string_lossy().replace('\'', "''");
    let command = format!(
        "$tokens=$null;$errors=$null;[System.Management.Automation.Language.Parser]::ParseFile('{escaped}',[ref]$tokens,[ref]$errors)|Out-Null;$r=@{{parse_ok=(@($errors).Count -eq 0);error_count=@($errors).Count;errors=@(@($errors)|Select-Object -First 8|ForEach-Object {{ @{{message=$_.Message;line=$_.Extent.StartLineNumber;column=$_.Extent.StartColumnNumber;text=$_.Extent.Text}} }})}};[Console]::Out.Write(($r|ConvertTo-Json -Compress -Depth 6))"
    );
    match runtime
        .run_host_shell_only(
            ShellRequest {
                command,
                working_dir: working_dir.to_path_buf(),
                timeout: Duration::from_secs(15),
                user: String::new(),
                max_capture_chars: Some(65_536),
                output_tx: None,
                pid_tx: None,
            },
            cancellation,
        )
        .await
    {
        Ok(output) => serde_json::from_str(output.stdout.trim()).unwrap_or_else(|error| {
            syntax_failure(&format!("PowerShell parser returned invalid JSON: {error}"))
        }),
        Err(RuntimeExecError::Process(error)) => syntax_failure(&error.message),
        Err(error) => syntax_failure(&error.to_string()),
    }
}

fn syntax_failure(message: &str) -> Value {
    json!({
        "parse_ok": false,
        "error_count": 1,
        "errors": [{"message": message, "line": null, "column": null, "text": null}],
    })
}

async fn read_prefix(path: &Path, max_bytes: usize) -> Result<Vec<u8>> {
    let mut file = fs::File::open(path)
        .await
        .with_context(|| format!("open {}", path.display()))?;
    let mut result = Vec::with_capacity(max_bytes.min(1024 * 1024));
    let mut buffer = vec![0_u8; 16 * 1024];
    while result.len() < max_bytes {
        let remaining = max_bytes - result.len();
        let chunk_len = remaining.min(buffer.len());
        let count = file.read(&mut buffer[..chunk_len]).await?;
        if count == 0 {
            break;
        }
        result.extend_from_slice(&buffer[..count]);
    }
    Ok(result)
}

async fn hash_file_sha256(path: &Path) -> Result<String> {
    let mut file = fs::File::open(path).await?;
    let mut hash = Sha256::new();
    let mut buffer = vec![0_u8; 64 * 1024];
    loop {
        let count = file.read(&mut buffer).await?;
        if count == 0 {
            break;
        }
        hash.update(&buffer[..count]);
    }
    Ok(format!("{:x}", hash.finalize()))
}

fn resolve_host_path(requested: &str, working_dir: &Path) -> PathBuf {
    let trimmed = requested.trim();
    if let Some(rest) = trimmed.strip_prefix('~') {
        let home = std::env::var_os("USERPROFILE")
            .or_else(|| std::env::var_os("HOME"))
            .map_or_else(|| working_dir.to_path_buf(), PathBuf::from);
        return home.join(rest.trim_start_matches(['\\', '/']));
    }
    let path = PathBuf::from(trimmed);
    if path.is_absolute() {
        path
    } else {
        working_dir.join(path)
    }
}

fn extension_lower(path: &Path) -> String {
    path.extension()
        .map(|value| format!(".{}", value.to_string_lossy().to_ascii_lowercase()))
        .unwrap_or_default()
}

fn detect_bom(bytes: &[u8]) -> Option<&'static str> {
    if bytes.starts_with(&[0xef, 0xbb, 0xbf]) {
        Some("utf8")
    } else if bytes.starts_with(&[0xff, 0xfe]) {
        Some("utf16le")
    } else if bytes.starts_with(&[0xfe, 0xff]) {
        Some("utf16be")
    } else {
        None
    }
}

fn detect_binary_format(bytes: &[u8]) -> Option<&'static str> {
    if bytes.starts_with(b"MZ") {
        Some("pe")
    } else if bytes.starts_with(&[0x7f, b'E', b'L', b'F']) {
        Some("elf")
    } else if bytes.len() >= 3 && bytes[0..2] == *b"PK" && matches!(bytes[2], 0x03 | 0x05 | 0x07) {
        Some("zip")
    } else if bytes.starts_with(&[0x1f, 0x8b]) {
        Some("gzip")
    } else if bytes.starts_with(&[0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c]) {
        Some("7z")
    } else if bytes.starts_with(&[0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00]) {
        Some("rar")
    } else {
        None
    }
}

fn detect_line_endings(value: &str) -> &'static str {
    let crlf = value.matches("\r\n").count();
    let without_crlf = value.replace("\r\n", "");
    let lf = without_crlf.matches('\n').count();
    let cr = without_crlf.matches('\r').count();
    match (crlf > 0, lf > 0, cr > 0) {
        (true, false, false) => "crlf",
        (false, true, false) => "lf",
        (false, false, true) => "cr",
        (false, false, false) => "none",
        _ => "mixed",
    }
}

fn push_string(value: &mut Value, field: &str, message: &str) {
    if let Some(items) = value[field].as_array_mut() {
        items.push(Value::String(message.to_owned()));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_bom_binary_magic_and_line_endings() {
        assert_eq!(detect_bom(&[0xef, 0xbb, 0xbf, b'a']), Some("utf8"));
        assert_eq!(detect_binary_format(b"MZ...."), Some("pe"));
        assert_eq!(detect_binary_format(&[0x7f, b'E', b'L', b'F']), Some("elf"));
        assert_eq!(detect_line_endings("a\r\nb\r\n"), "crlf");
        assert_eq!(detect_line_endings("a\nb\n"), "lf");
        assert_eq!(detect_line_endings("a\r\nb\n"), "mixed");
    }

    #[test]
    fn javascript_iso_rounds_native_submillisecond_timestamp() {
        let value = OffsetDateTime::from_unix_timestamp(0)
            .unwrap()
            .replace_nanosecond(735_970_000)
            .unwrap();
        assert_eq!(format_javascript_iso_utc(value), "1970-01-01T00:00:00.736Z");
    }

    #[test]
    fn standalone_latin_letters_are_not_mojibake_markers() {
        let mut info = json!({"observations": []});
        inspect_text_sample(&mut info, "Ã Â".as_bytes(), None);
        assert_eq!(info["utf8_valid"], true);
        assert_eq!(info["suspicious_mojibake_count"], 0);

        let mut corrupted = json!({"observations": []});
        inspect_text_sample(&mut corrupted, "â€”".as_bytes(), None);
        assert!(corrupted["suspicious_mojibake_count"].as_u64().unwrap() > 0);
    }

    #[test]
    fn text_sample_marks_invalid_utf8_and_nul_as_corrupt_signals() {
        let mut info = json!({"observations": []});
        inspect_text_sample(&mut info, &[0xff, 0, b'a'], None);
        assert_eq!(info["utf8_valid"], false);
        assert_eq!(info["null_byte_count"], 1);
        assert!(info["replacement_character_count"].as_u64().unwrap() > 0);
    }
}
