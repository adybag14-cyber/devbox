use std::{path::Path, sync::LazyLock};

use regex::{Captures, Regex};

use base64::{Engine as _, engine::general_purpose::STANDARD};

pub const MAX_POWERSHELL_ENCODED_COMMAND_CHARS_BEFORE_FILE: usize = 24_000;

const POWERSHELL_QUIET_PRELUDE: &str =
    "$ProgressPreference = 'SilentlyContinue'\n$InformationPreference = 'SilentlyContinue'";

#[must_use]
pub fn with_quiet_prelude(command: &str) -> String {
    format!("{POWERSHELL_QUIET_PRELUDE}\n{command}")
}

#[must_use]
pub fn encoded_command_args(command: &str) -> Vec<String> {
    let mut utf16le = Vec::new();
    for unit in with_quiet_prelude(command).encode_utf16() {
        utf16le.extend_from_slice(&unit.to_le_bytes());
    }
    vec![
        "-NoLogo".to_owned(),
        "-NoProfile".to_owned(),
        "-NonInteractive".to_owned(),
        "-ExecutionPolicy".to_owned(),
        "Bypass".to_owned(),
        "-EncodedCommand".to_owned(),
        STANDARD.encode(utf16le),
    ]
}

#[must_use]
pub fn file_args(script_path: &Path) -> Vec<String> {
    vec![
        "-NoLogo".to_owned(),
        "-NoProfile".to_owned(),
        "-NonInteractive".to_owned(),
        "-ExecutionPolicy".to_owned(),
        "Bypass".to_owned(),
        "-File".to_owned(),
        script_path.to_string_lossy().into_owned(),
    ]
}

#[must_use]
pub fn should_use_script_file(command: &str) -> bool {
    encoded_command_args(command).join(" ").len()
        >= MAX_POWERSHELL_ENCODED_COMMAND_CHARS_BEFORE_FILE
}

#[must_use]
pub fn admin_check_command() -> &'static str {
    r"$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
[Console]::Out.Write((@{
  isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
} | ConvertTo-Json -Compress))"
}

#[must_use]
pub fn elevated_wrapper(
    script_path: &Path,
    working_dir: &Path,
    stdout_path: &Path,
    stderr_path: &Path,
    exit_code_path: &Path,
) -> String {
    format!(
        r"$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$InformationPreference = 'SilentlyContinue'
$stdoutPath = '{}'
$stderrPath = '{}'
$exitCodePath = '{}'
Set-Location -LiteralPath '{}'
$global:LASTEXITCODE = 0
try {{
  & '{}' 1> $stdoutPath 2> $stderrPath 3>> $stdoutPath 4>> $stdoutPath 5>> $stdoutPath 6>> $stdoutPath
  $exitCode = if ($global:LASTEXITCODE -is [int]) {{ [int]$global:LASTEXITCODE }} else {{ 0 }}
}} catch {{
  $_ | Out-File -LiteralPath $stderrPath -Encoding utf8 -Append
  if ($_.ScriptStackTrace) {{
    $_.ScriptStackTrace | Out-File -LiteralPath $stderrPath -Encoding utf8 -Append
  }}
  $exitCode = 1
}}
Set-Content -LiteralPath $exitCodePath -Value ([string]$exitCode) -Encoding ascii",
        ps_single_quote(stdout_path),
        ps_single_quote(stderr_path),
        ps_single_quote(exit_code_path),
        ps_single_quote(working_dir),
        ps_single_quote(script_path),
    )
}

#[must_use]
pub fn elevated_launcher(
    power_shell_exe: &str,
    script_path: &Path,
    working_dir: &Path,
    stdout_path: &Path,
    stderr_path: &Path,
    exit_code_path: &Path,
    timeout_ms: u64,
) -> String {
    let child_args = encoded_command_args(&elevated_wrapper(
        script_path,
        working_dir,
        stdout_path,
        stderr_path,
        exit_code_path,
    ));
    let escaped_child_args = child_args
        .iter()
        .map(|argument| format!("'{}'", argument.replace('\'', "''")))
        .collect::<Vec<_>>()
        .join(", ");
    format!(
        r"$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$InformationPreference = 'SilentlyContinue'
$arguments = @({escaped_child_args})
$process = Start-Process -FilePath '{}' -Verb RunAs -PassThru -WindowStyle Hidden -WorkingDirectory '{}' -ArgumentList $arguments
if ($null -eq $process) {{
  throw 'Failed to start elevated PowerShell process.'
}}
if (-not $process.WaitForExit({})) {{
  try {{
    Stop-Process -Id $process.Id -Force -ErrorAction Stop
  }} catch {{
  }}
  throw 'Command timed out after {} ms.'
}}
exit $process.ExitCode",
        ps_single_quote_str(power_shell_exe),
        ps_single_quote(working_dir),
        timeout_ms.max(1),
        timeout_ms.max(1),
    )
}

#[must_use]
pub fn clean_output(value: &str) -> String {
    static ENVELOPE: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r"(?s)#< CLIXML\s*<Objs\b.*?</Objs>").expect("valid CLIXML envelope regex")
    });
    static CHANNEL: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(
            r#"(?is)<S\s+S="(?:Error|Warning|Verbose|Debug|Information|Output)"[^>]*>(.*?)</S>"#,
        )
        .expect("valid CLIXML channel regex")
    });
    if !value.contains("#< CLIXML") {
        return value.to_owned();
    }
    ENVELOPE
        .replace_all(value, |envelope: &Captures<'_>| {
            let messages = CHANNEL
                .captures_iter(&envelope[0])
                .filter_map(|capture| capture.get(1))
                .map(|capture| decode_clixml_text(capture.as_str()).trim().to_owned())
                .filter(|message| !message.is_empty())
                .collect::<Vec<_>>();
            if messages.is_empty() {
                String::new()
            } else {
                format!("{}\n", messages.join("\n"))
            }
        })
        .into_owned()
}

fn decode_clixml_text(value: &str) -> String {
    static ESCAPE: LazyLock<Regex> = LazyLock::new(|| {
        Regex::new(r"(?i)_x([0-9a-f]{4})_").expect("valid PowerShell escape regex")
    });
    let decoded = ESCAPE.replace_all(value, |capture: &Captures<'_>| {
        capture
            .get(1)
            .and_then(|hex| u32::from_str_radix(hex.as_str(), 16).ok())
            .and_then(char::from_u32)
            .map_or_else(|| capture[0].to_owned(), |character| character.to_string())
    });
    decoded
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&apos;", "'")
        .replace("&amp;", "&")
}

fn ps_single_quote(path: &Path) -> String {
    ps_single_quote_str(&path.to_string_lossy())
}

fn ps_single_quote_str(value: &str) -> String {
    value.replace('\'', "''")
}

#[cfg(test)]
mod tests {
    use super::*;
    use base64::engine::general_purpose::STANDARD;

    #[test]
    fn encoded_command_matches_utf16le_powershell_contract() {
        let args = encoded_command_args("Write-Output 'hello'");
        assert_eq!(
            args[..6],
            [
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-EncodedCommand",
            ]
        );
        let bytes = STANDARD.decode(&args[6]).expect("base64");
        let units = bytes
            .chunks_exact(2)
            .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
            .collect::<Vec<_>>();
        let decoded = String::from_utf16(&units).expect("utf16");
        assert!(decoded.starts_with("$ProgressPreference = 'SilentlyContinue'"));
        assert!(decoded.ends_with("Write-Output 'hello'"));
    }

    #[test]
    fn script_file_cutoff_matches_javascript_threshold() {
        assert!(!should_use_script_file("Write-Output ok"));
        assert!(should_use_script_file(&"x".repeat(20_000)));
    }

    #[test]
    fn clixml_cleanup_matches_javascript_channels() {
        let source = "before\n#< CLIXML <Objs><S S=\"Error\">bad_x000A_thing &amp; more</S><S S=\"Progress\">ignored</S></Objs>after";
        assert_eq!(clean_output(source), "before\nbad\nthing & more\nafter");
    }

    #[test]
    fn elevated_launcher_escapes_single_quotes_and_waits() {
        let launcher = elevated_launcher(
            r"C:\Program Files\PowerShell\7\pwsh.exe",
            Path::new(r"C:\Temp\it's\command.ps1"),
            Path::new(r"C:\work dir"),
            Path::new(r"C:\Temp\stdout.txt"),
            Path::new(r"C:\Temp\stderr.txt"),
            Path::new(r"C:\Temp\exitcode.txt"),
            30_000,
        );
        assert!(launcher.contains("Start-Process"));
        assert!(launcher.contains("-Verb RunAs"));
        assert!(launcher.contains("WaitForExit(30000)"));
        let wrapper = elevated_wrapper(
            Path::new(r"C:\Temp\it's\command.ps1"),
            Path::new(r"C:\work dir"),
            Path::new(r"C:\Temp\stdout.txt"),
            Path::new(r"C:\Temp\stderr.txt"),
            Path::new(r"C:\Temp\exitcode.txt"),
        );
        assert!(wrapper.contains("it''s"));
    }
}
