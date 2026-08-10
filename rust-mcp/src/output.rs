use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum OutputMode {
    Head,
    Tail,
    Summary,
}

impl OutputMode {
    #[must_use]
    pub fn parse(value: &str) -> Self {
        match value.trim().to_ascii_lowercase().as_str() {
            "head" => Self::Head,
            "summary" => Self::Summary,
            _ => Self::Tail,
        }
    }

    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Head => "head",
            Self::Tail => "tail",
            Self::Summary => "summary",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct ShapedOutput {
    pub text: String,
    pub truncated: bool,
    #[serde(rename = "originalChars")]
    pub original_chars: usize,
    #[serde(rename = "originalLines")]
    pub original_lines: Option<usize>,
    pub mode: OutputMode,
}

#[must_use]
pub fn shape_process_output(
    text: &str,
    mode: OutputMode,
    max_chars: usize,
    max_lines: usize,
) -> ShapedOutput {
    let original_chars = js_len(text);
    let line_result = by_lines(text, max_lines, mode);
    let char_result = by_chars(&line_result.text, max_chars, mode);
    ShapedOutput {
        text: char_result.text,
        truncated: line_result.truncated || char_result.truncated,
        original_chars,
        original_lines: line_result.original_lines,
        mode,
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct CharResult {
    text: String,
    truncated: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct LineResult {
    text: String,
    truncated: bool,
    original_lines: Option<usize>,
}

fn by_chars(text: &str, max_chars: usize, mode: OutputMode) -> CharResult {
    let limit = max_chars.max(100);
    let length = js_len(text);
    if length <= limit {
        return CharResult {
            text: text.to_owned(),
            truncated: false,
        };
    }
    match mode {
        OutputMode::Head => {
            let note = marker(length.saturating_sub(limit), "tail");
            let keep = limit.saturating_sub(js_len(&note));
            CharResult {
                text: format!("{}{}", slice_js_units(text, 0, keep), note),
                truncated: true,
            }
        }
        OutputMode::Summary => {
            let note = marker(length.saturating_sub(limit), "middle");
            let available = limit.saturating_sub(js_len(&note));
            let head = available / 2;
            let tail = available.saturating_sub(head);
            CharResult {
                text: format!(
                    "{}{}{}",
                    slice_js_units(text, 0, head),
                    note,
                    slice_js_units(text, length.saturating_sub(tail), length)
                ),
                truncated: true,
            }
        }
        OutputMode::Tail => {
            let note = marker(length.saturating_sub(limit), "head");
            let keep = limit.saturating_sub(js_len(&note));
            CharResult {
                text: format!(
                    "{}{}",
                    note,
                    slice_js_units(text, length.saturating_sub(keep), length)
                ),
                truncated: true,
            }
        }
    }
}

fn by_lines(text: &str, max_lines: usize, mode: OutputMode) -> LineResult {
    if max_lines == 0 {
        return LineResult {
            text: text.to_owned(),
            truncated: false,
            original_lines: None,
        };
    }
    let (content, trailing_newline) = strip_one_trailing_newline(text);
    let lines = content
        .split('\n')
        .map(strip_terminal_cr)
        .collect::<Vec<_>>();
    let original_lines = lines.len();
    if original_lines <= max_lines {
        return LineResult {
            text: text.to_owned(),
            truncated: false,
            original_lines: Some(original_lines),
        };
    }
    let suffix = if trailing_newline { "\n" } else { "" };
    let shaped = match mode {
        OutputMode::Head => format!(
            "{}\n... tail lines omitted ...{suffix}",
            lines[..max_lines].join("\n")
        ),
        OutputMode::Summary => {
            let head = max_lines / 2;
            let tail = max_lines.saturating_sub(head);
            format!(
                "{}\n... middle lines omitted ...\n{}{}",
                lines[..head].join("\n"),
                lines[lines.len() - tail..].join("\n"),
                suffix
            )
        }
        OutputMode::Tail => format!(
            "... head lines omitted ...\n{}{}",
            lines[lines.len() - max_lines..].join("\n"),
            suffix
        ),
    };
    LineResult {
        text: shaped,
        truncated: true,
        original_lines: Some(original_lines),
    }
}

fn marker(omitted_chars: usize, mode: &str) -> String {
    format!("\n... {mode} output omitted {omitted_chars} characters ...\n")
}

fn js_len(value: &str) -> usize {
    value.encode_utf16().count()
}

fn slice_js_units(value: &str, start: usize, end: usize) -> String {
    let units = value.encode_utf16().collect::<Vec<_>>();
    let start = start.min(units.len());
    let end = end.min(units.len()).max(start);
    String::from_utf16_lossy(&units[start..end])
}

fn strip_one_trailing_newline(value: &str) -> (&str, bool) {
    if let Some(content) = value.strip_suffix("\r\n") {
        (content, true)
    } else if let Some(content) = value.strip_suffix('\n') {
        (content, true)
    } else {
        (value, false)
    }
}

fn strip_terminal_cr(value: &str) -> &str {
    value.strip_suffix('\r').unwrap_or(value)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn line_limit_preserves_one_terminal_newline() {
        let result = shape_process_output("a\nb\nc\n", OutputMode::Tail, 1000, 2);
        assert_eq!(result.text, "... head lines omitted ...\nb\nc\n");
        assert_eq!(result.original_lines, Some(3));
        assert!(result.truncated);
    }

    #[test]
    fn summary_keeps_both_ends_inside_character_budget() {
        let input = "a".repeat(500);
        let result = shape_process_output(&input, OutputMode::Summary, 120, 0);
        assert!(result.truncated);
        assert!(js_len(&result.text) <= 120);
        assert!(result.text.contains("middle output omitted 380 characters"));
    }

    #[test]
    fn character_accounting_matches_javascript_utf16_length() {
        assert_eq!(js_len("a😀b"), 4);
    }
}
