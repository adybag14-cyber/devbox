use std::{
    collections::HashSet,
    path::{Path, PathBuf},
    sync::Arc,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, bail};
use regex::{Regex, RegexBuilder};
use serde_json::Value;
use tokio::{fs, sync::mpsc::unbounded_channel};
use tokio_util::sync::CancellationToken;

use crate::{
    Config, RuntimeMode,
    config::HostSearchBackend,
    files::ProcessResult,
    process::{OutputStream, ProcessError, ProcessOptions, spawn_process},
};

#[derive(Debug, Clone)]
pub struct SearchRequest {
    pub pattern: String,
    pub path: String,
    pub glob: String,
    pub case_sensitive: bool,
    pub max_matches: usize,
    pub max_depth: usize,
    pub max_file_bytes: u64,
    pub timeout: Duration,
    pub exclude_directories: Vec<String>,
    pub include_ignored: bool,
}

#[derive(Debug, Clone)]
pub struct SearchService {
    config: Arc<Config>,
}

impl SearchService {
    #[must_use]
    pub fn new(config: Arc<Config>) -> Self {
        Self { config }
    }

    /// Search the selected Devbox runtime with ripgrep, using a bounded native Rust
    /// fallback for host mode when ripgrep cannot be launched.
    ///
    /// # Errors
    /// Returns cancellation, timeout, filesystem, regex, or ripgrep execution errors.
    pub async fn search(
        &self,
        mut request: SearchRequest,
        cancellation: CancellationToken,
    ) -> Result<ProcessResult> {
        request.max_matches = request.max_matches.max(1);
        request.max_depth = request.max_depth.max(1);
        request.max_file_bytes = request.max_file_bytes.max(1);
        request.timeout = request.timeout.max(Duration::from_millis(1));
        let (line_regex, fixed_strings) =
            compile_pattern(&request.pattern, request.case_sensitive)?;

        if self.config.runtime_mode == RuntimeMode::Host
            && self.config.host_search_backend == HostSearchBackend::Rust
        {
            return search_host_fallback(
                resolve_host_root(&self.config, &request.path),
                &request,
                &line_regex,
                cancellation,
            )
            .await;
        }

        match self
            .search_with_ripgrep(&request, fixed_strings, cancellation.clone())
            .await
        {
            Ok(result) => Ok(result),
            Err(RipgrepAttemptError::Unavailable(error))
                if self.config.runtime_mode == RuntimeMode::Host =>
            {
                let _ = error;
                search_host_fallback(
                    resolve_host_root(&self.config, &request.path),
                    &request,
                    &line_regex,
                    cancellation,
                )
                .await
            }
            Err(RipgrepAttemptError::Unavailable(error)) => {
                bail!("ripgrep is unavailable in the selected Devbox runtime: {error}")
            }
            Err(RipgrepAttemptError::Failed(error)) => Err(error),
        }
    }

    async fn search_with_ripgrep(
        &self,
        request: &SearchRequest,
        fixed_strings: bool,
        cancellation: CancellationToken,
    ) -> std::result::Result<ProcessResult, RipgrepAttemptError> {
        let args = build_rg_args(request, fixed_strings);
        let (program, args, cwd) = self.process_command(args);
        let process_cancel = cancellation.child_token();
        let task_cancel = process_cancel.clone();
        let (tx, mut rx) = unbounded_channel();
        let max_capture_chars = self.config.max_mcp_transfer_chars.clamp(1, 65_536);
        let timeout = request.timeout;
        let task = tokio::spawn(async move {
            spawn_process(
                &program,
                &args,
                ProcessOptions {
                    cwd,
                    timeout: Some(timeout),
                    max_capture_chars: Some(max_capture_chars),
                    output_tx: Some(tx),
                    ..ProcessOptions::default()
                },
                task_cancel,
            )
            .await
        });

        let mut parser = RipgrepJsonParser::new(request.max_matches);
        while let Some(chunk) = rx.recv().await {
            if chunk.stream == OutputStream::Stdout {
                parser.push_bytes(&chunk.bytes);
                if parser.match_limit_reached {
                    process_cancel.cancel();
                    break;
                }
            }
            if cancellation.is_cancelled() {
                process_cancel.cancel();
                break;
            }
        }
        drop(rx);
        let outcome = task.await.map_err(|error| {
            RipgrepAttemptError::Failed(anyhow::anyhow!("join ripgrep search task: {error}"))
        })?;

        if cancellation.is_cancelled() {
            return Err(RipgrepAttemptError::Failed(anyhow::anyhow!(
                "Search cancelled by the MCP client."
            )));
        }
        match outcome {
            Ok(output) => Ok(build_rg_result(
                request,
                fixed_strings,
                parser,
                &output.stderr,
            )),
            Err(error) if parser.match_limit_reached && error.aborted => Ok(build_rg_result(
                request,
                fixed_strings,
                parser,
                &error.stderr,
            )),
            Err(error) if error.exit_code == Some(1) && !error.timed_out && !error.aborted => Ok(
                build_rg_result(request, fixed_strings, parser, &error.stderr),
            ),
            Err(error) if error.exit_code.is_none() && looks_like_missing_program(&error) => {
                Err(RipgrepAttemptError::Unavailable(error.to_string()))
            }
            Err(error) => Err(RipgrepAttemptError::Failed(anyhow::Error::new(error))),
        }
    }

    fn process_command(&self, rg_args: Vec<String>) -> (String, Vec<String>, Option<PathBuf>) {
        match self.config.runtime_mode {
            RuntimeMode::Host => (
                "rg".to_owned(),
                rg_args,
                Some(self.config.devbox_workspace_path.clone()),
            ),
            RuntimeMode::Docker => {
                let mut args = vec!["exec".to_owned()];
                if !self.config.devbox_default_user.trim().is_empty() {
                    args.extend(["-u".to_owned(), self.config.devbox_default_user.clone()]);
                }
                args.extend([
                    "-w".to_owned(),
                    self.config
                        .devbox_workspace_path
                        .to_string_lossy()
                        .into_owned(),
                    self.config.devbox_container_name.clone(),
                    "rg".to_owned(),
                ]);
                args.extend(rg_args);
                ("docker".to_owned(), args, None)
            }
        }
    }
}

#[derive(Debug)]
enum RipgrepAttemptError {
    Unavailable(String),
    Failed(anyhow::Error),
}

fn build_rg_args(request: &SearchRequest, fixed_strings: bool) -> Vec<String> {
    let mut args = vec![
        "--json".to_owned(),
        "--color".to_owned(),
        "never".to_owned(),
        "--no-messages".to_owned(),
        "--max-depth".to_owned(),
        request.max_depth.max(1).to_string(),
        "--max-filesize".to_owned(),
        request.max_file_bytes.max(1).to_string(),
    ];
    if !request.case_sensitive {
        args.push("-i".to_owned());
    }
    if fixed_strings {
        args.push("-F".to_owned());
    }
    if request.include_ignored {
        args.extend(["--hidden".to_owned(), "--no-ignore".to_owned()]);
    }
    if !request.glob.trim().is_empty() {
        args.extend(["--glob".to_owned(), request.glob.clone()]);
    }
    for name in &request.exclude_directories {
        let name = name.trim();
        if !name.is_empty() {
            args.extend(["--glob".to_owned(), format!("!**/{name}/**")]);
        }
    }
    args.extend([
        "--".to_owned(),
        request.pattern.clone(),
        request.path.clone(),
    ]);
    args
}

fn build_rg_result(
    request: &SearchRequest,
    fixed_strings: bool,
    mut parser: RipgrepJsonParser,
    process_stderr: &str,
) -> ProcessResult {
    parser.finish();
    let mut notices = vec!["search backend ripgrep".to_owned()];
    if fixed_strings {
        notices.push("invalid regex treated as literal text".to_owned());
    }
    if parser.match_limit_reached {
        notices.push(format!("match limit {} reached", request.max_matches));
    }
    if !request.exclude_directories.is_empty() {
        notices.push(format!(
            "excluded {} directory names",
            request.exclude_directories.len()
        ));
    }
    notices.push(format!("candidate files {}", parser.files_scanned));
    let process_stderr = process_stderr.trim();
    if !process_stderr.is_empty() {
        notices.push(process_stderr.to_owned());
    }
    let stdout = if parser.matches.is_empty() {
        String::new()
    } else {
        format!("{}\n", parser.matches.join("\n"))
    };
    let stderr = if notices.is_empty() {
        String::new()
    } else {
        format!("{}\n", notices.join("; "))
    };
    ProcessResult::success(stdout, stderr)
}

#[derive(Debug, Default)]
struct RipgrepJsonParser {
    pending: String,
    matches: Vec<String>,
    files_scanned: usize,
    max_matches: usize,
    match_limit_reached: bool,
}

impl RipgrepJsonParser {
    fn new(max_matches: usize) -> Self {
        Self {
            max_matches: max_matches.max(1),
            ..Self::default()
        }
    }

    fn push_bytes(&mut self, bytes: &[u8]) {
        self.pending.push_str(&String::from_utf8_lossy(bytes));
        while let Some(index) = self.pending.find('\n') {
            let line = self.pending[..index].trim_end_matches('\r').to_owned();
            self.pending.drain(..=index);
            self.push_line(&line);
            if self.match_limit_reached {
                break;
            }
        }
    }

    fn finish(&mut self) {
        if !self.pending.is_empty() && !self.match_limit_reached {
            let line = std::mem::take(&mut self.pending);
            self.push_line(line.trim_end_matches(['\r', '\n']));
        }
    }

    fn push_line(&mut self, line: &str) {
        let Ok(event) = serde_json::from_str::<Value>(line) else {
            return;
        };
        match event.get("type").and_then(Value::as_str) {
            Some("begin") => {
                self.files_scanned = self.files_scanned.saturating_add(1);
            }
            Some("match") => self.push_match(&event),
            _ => {}
        }
    }

    fn push_match(&mut self, event: &Value) {
        if self.matches.len() >= self.max_matches {
            self.match_limit_reached = true;
            return;
        }
        let data = &event["data"];
        let path = data["path"]["text"].as_str().unwrap_or_default();
        let line_number = data["line_number"].as_u64().unwrap_or_default();
        let text = data["lines"]["text"]
            .as_str()
            .unwrap_or_default()
            .trim_end_matches(['\r', '\n']);
        self.matches.push(format!("{path}:{line_number}:{text}"));
        if self.matches.len() >= self.max_matches {
            self.match_limit_reached = true;
        }
    }
}

fn compile_pattern(pattern: &str, case_sensitive: bool) -> Result<(Regex, bool)> {
    let mut builder = RegexBuilder::new(pattern);
    builder.case_insensitive(!case_sensitive);
    if let Ok(regex) = builder.build() {
        Ok((regex, false))
    } else {
        let mut literal = RegexBuilder::new(&regex::escape(pattern));
        literal.case_insensitive(!case_sensitive);
        Ok((
            literal
                .build()
                .context("compile escaped literal search pattern")?,
            true,
        ))
    }
}

fn looks_like_missing_program(error: &ProcessError) -> bool {
    let text = format!("{} {}", error.message, error.stderr).to_ascii_lowercase();
    text.contains("not found")
        || text.contains("cannot find")
        || text.contains("no such file")
        || text.contains("system cannot find")
}

fn resolve_host_root(config: &Config, path: &str) -> PathBuf {
    let requested = PathBuf::from(path);
    if requested.is_absolute() {
        requested
    } else {
        config.devbox_workspace_path.join(requested)
    }
}

struct FallbackContext<'a> {
    root: &'a Path,
    root_is_file: bool,
    request: &'a SearchRequest,
    line_regex: &'a Regex,
    glob_regex: &'a Regex,
    excluded: &'a HashSet<String>,
}

struct FallbackState {
    stack: Vec<(PathBuf, usize)>,
    matches: Vec<String>,
    files_scanned: usize,
    skipped: usize,
    skipped_large: usize,
    skipped_binary: usize,
    pruned: usize,
    timed_out: bool,
}

impl FallbackState {
    fn new(root: PathBuf) -> Self {
        Self {
            stack: vec![(root, 0)],
            matches: Vec::new(),
            files_scanned: 0,
            skipped: 0,
            skipped_large: 0,
            skipped_binary: 0,
            pruned: 0,
            timed_out: false,
        }
    }
}

async fn search_host_fallback(
    root: PathBuf,
    request: &SearchRequest,
    line_regex: &Regex,
    cancellation: CancellationToken,
) -> Result<ProcessResult> {
    let started = Instant::now();
    let excluded = request
        .exclude_directories
        .iter()
        .map(|name| name.trim().to_ascii_lowercase())
        .filter(|name| !name.is_empty())
        .collect::<HashSet<_>>();
    let glob_regex = glob_regex(&request.glob)?;
    let root_is_file = fs::metadata(&root)
        .await
        .map(|value| value.is_file())
        .unwrap_or(false);
    let context = FallbackContext {
        root: &root,
        root_is_file,
        request,
        line_regex,
        glob_regex: &glob_regex,
        excluded: &excluded,
    };
    let mut state = FallbackState::new(root.clone());

    while let Some((path, depth)) = state.stack.pop() {
        if cancellation.is_cancelled() {
            bail!("Search cancelled by the MCP client.");
        }
        if started.elapsed() >= request.timeout {
            state.timed_out = true;
            break;
        }
        visit_fallback_path(&context, &mut state, path, depth).await?;
        if state.matches.len() >= request.max_matches {
            break;
        }
    }
    Ok(build_fallback_result(request, &state))
}

async fn visit_fallback_path(
    context: &FallbackContext<'_>,
    state: &mut FallbackState,
    path: PathBuf,
    depth: usize,
) -> Result<()> {
    let metadata = match fs::symlink_metadata(&path).await {
        Ok(value) => value,
        Err(error) if is_skippable_search_error(&error) => {
            state.skipped = state.skipped.saturating_add(1);
            return Ok(());
        }
        Err(error) => return Err(error).with_context(|| format!("inspect {}", path.display())),
    };
    if metadata.is_dir() {
        enqueue_fallback_children(context, state, &path, depth).await?;
    } else if metadata.is_file() {
        search_fallback_file(context, state, &path, metadata.len()).await?;
    }
    Ok(())
}

async fn enqueue_fallback_children(
    context: &FallbackContext<'_>,
    state: &mut FallbackState,
    path: &Path,
    depth: usize,
) -> Result<()> {
    if depth >= context.request.max_depth {
        return Ok(());
    }
    let mut reader = match fs::read_dir(path).await {
        Ok(value) => value,
        Err(error) if is_skippable_search_error(&error) => {
            state.skipped = state.skipped.saturating_add(1);
            return Ok(());
        }
        Err(error) => return Err(error).with_context(|| format!("list {}", path.display())),
    };
    let mut children = Vec::new();
    while let Some(entry) = reader.next_entry().await? {
        children.push(entry.path());
    }
    children.sort_by(|left, right| left.to_string_lossy().cmp(&right.to_string_lossy()));
    for child in children.into_iter().rev() {
        let name = child
            .file_name()
            .map(|value| value.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        if context.excluded.contains(&name) {
            state.pruned = state.pruned.saturating_add(1);
        } else {
            state.stack.push((child, depth.saturating_add(1)));
        }
    }
    Ok(())
}

async fn search_fallback_file(
    context: &FallbackContext<'_>,
    state: &mut FallbackState,
    path: &Path,
    file_size: u64,
) -> Result<()> {
    let relative = if context.root_is_file {
        path.file_name()
            .map_or_else(|| path.to_path_buf(), PathBuf::from)
    } else {
        path.strip_prefix(context.root)
            .unwrap_or(path)
            .to_path_buf()
    };
    let relative_text = relative.to_string_lossy().replace('\\', "/");
    if !context.glob_regex.is_match(&relative_text) {
        return Ok(());
    }
    if file_size > context.request.max_file_bytes {
        state.skipped_large = state.skipped_large.saturating_add(1);
        return Ok(());
    }
    let bytes = match fs::read(path).await {
        Ok(bytes) => bytes,
        Err(error) if is_skippable_search_error(&error) => {
            state.skipped = state.skipped.saturating_add(1);
            return Ok(());
        }
        Err(error) => return Err(error).with_context(|| format!("read {}", path.display())),
    };
    state.files_scanned = state.files_scanned.saturating_add(1);
    if bytes.iter().take(8_192).any(|byte| *byte == 0) {
        state.skipped_binary = state.skipped_binary.saturating_add(1);
        return Ok(());
    }
    let text = String::from_utf8_lossy(&bytes);
    for (index, line) in text.lines().enumerate() {
        if context.line_regex.is_match(line) {
            state.matches.push(format!(
                "{}:{}:{line}",
                path.display(),
                index.saturating_add(1)
            ));
            if state.matches.len() >= context.request.max_matches {
                break;
            }
        }
    }
    Ok(())
}

fn build_fallback_result(request: &SearchRequest, state: &FallbackState) -> ProcessResult {
    let mut notices = Vec::new();
    if state.timed_out {
        notices.push(format!(
            "search stopped after {} ms",
            request.timeout.as_millis()
        ));
    }
    if state.matches.len() >= request.max_matches {
        notices.push(format!("match limit {} reached", request.max_matches));
    }
    if state.pruned > 0 {
        notices.push(format!("pruned {} excluded directories", state.pruned));
    }
    if state.skipped > 0 {
        notices.push(format!(
            "skipped {} inaccessible or vanished paths",
            state.skipped
        ));
    }
    if state.skipped_large > 0 {
        notices.push(format!("skipped {} oversized files", state.skipped_large));
    }
    let stdout = if state.matches.is_empty() {
        String::new()
    } else {
        format!("{}\n", state.matches.join("\n"))
    };
    let stderr = if notices.is_empty() {
        String::new()
    } else {
        format!("{}\n", notices.join("; "))
    };
    ProcessResult::success(stdout, stderr)
}

fn is_skippable_search_error(error: &std::io::Error) -> bool {
    matches!(
        error.kind(),
        std::io::ErrorKind::NotFound | std::io::ErrorKind::PermissionDenied
    )
}

fn glob_regex(glob: &str) -> Result<Regex> {
    let glob = if glob.trim().is_empty() { "*" } else { glob };
    let mut pattern = String::from("^");
    for character in glob.chars() {
        match character {
            '*' => pattern.push_str(".*"),
            '?' => pattern.push('.'),
            '.' | '+' | '(' | ')' | '^' | '$' | '|' | '{' | '}' | '[' | ']' | '\\' => {
                pattern.push('\\');
                pattern.push(character);
            }
            character => pattern.push(character),
        }
    }
    pattern.push('$');
    Regex::new(&pattern).context("compile glob filter")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(path: String) -> SearchRequest {
        SearchRequest {
            pattern: "needle".to_owned(),
            path,
            glob: "*.txt".to_owned(),
            case_sensitive: false,
            max_matches: 2,
            max_depth: 4,
            max_file_bytes: 1024 * 1024,
            timeout: Duration::from_secs(1),
            exclude_directories: vec!["node_modules".to_owned()],
            include_ignored: false,
        }
    }

    #[test]
    fn ripgrep_args_preserve_filters_and_literal_fallback() {
        let args = build_rg_args(&request("/workspace".to_owned()), true);
        assert!(args.windows(2).any(|pair| pair == ["--glob", "*.txt"]));
        assert!(args.contains(&"-F".to_owned()));
        assert!(args.contains(&"-i".to_owned()));
        assert!(args.contains(&"!**/node_modules/**".to_owned()));
        assert_eq!(&args[args.len() - 2..], ["needle", "/workspace"]);
    }

    #[test]
    fn parser_handles_fragmented_json_and_global_limit() {
        let first = r#"{"type":"begin","data":{"path":{"text":"a.txt"}}}
{"type":"match","data":{"path":{"text":"a.txt"},"lines":{"text":"one needle\n"},"line_number":3}}
{"type":"match","data":{"path":{"text":"a.txt"},"lines":{"text":"two needle\n"},"line_number":8}}
"#;
        let mut parser = RipgrepJsonParser::new(2);
        let split = first.len() / 2;
        parser.push_bytes(&first.as_bytes()[..split]);
        parser.push_bytes(&first.as_bytes()[split..]);
        parser.finish();
        assert_eq!(parser.files_scanned, 1);
        assert_eq!(parser.matches, ["a.txt:3:one needle", "a.txt:8:two needle"]);
        assert!(parser.match_limit_reached);
    }

    #[test]
    fn invalid_regex_is_compiled_as_literal_text() {
        let (regex, fixed) = compile_pattern("[unterminated", false).expect("literal fallback");
        assert!(fixed);
        assert!(regex.is_match("prefix [UNTERMINATED suffix"));
    }

    #[tokio::test]
    async fn host_fallback_is_bounded_and_prunes_excluded_directories() {
        let temp = tempfile::tempdir().expect("temp dir");
        fs::create_dir_all(temp.path().join("node_modules"))
            .await
            .unwrap();
        fs::write(temp.path().join("a.txt"), b"Needle one\nother\n")
            .await
            .unwrap();
        fs::write(temp.path().join("b.txt"), b"needle two\nneedle three\n")
            .await
            .unwrap();
        fs::write(
            temp.path().join("node_modules").join("hidden.txt"),
            b"needle hidden\n",
        )
        .await
        .unwrap();
        let req = request(temp.path().to_string_lossy().into_owned());
        let (regex, _) = compile_pattern(&req.pattern, req.case_sensitive).unwrap();
        let result = search_host_fallback(
            temp.path().to_path_buf(),
            &req,
            &regex,
            CancellationToken::new(),
        )
        .await
        .expect("fallback search");
        let lines = result.stdout.lines().collect::<Vec<_>>();
        assert_eq!(lines.len(), 2);
        assert!(result.stderr.contains("match limit 2 reached"));
        assert!(result.stderr.contains("pruned 1 excluded directories"));
        assert!(!result.stdout.contains("hidden.txt"));
    }
}
