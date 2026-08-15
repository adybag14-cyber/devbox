use std::{
    borrow::Cow,
    sync::{Arc, LazyLock},
};

use rmcp::model::{Tool, ToolAnnotations};
use serde_json::{Map, Value, json};

use crate::{Config, RuntimeMode};

static TOOL_METADATA: LazyLock<Map<String, Value>> = LazyLock::new(|| {
    serde_json::from_str(include_str!("tool_metadata_contract.json"))
        .expect("embedded JS tool metadata contract must be valid JSON")
});

const MAX_SAFE_INTEGER: u64 = 9_007_199_254_740_991;

pub fn configure_tool_output_schema(tool: &mut Tool) {
    tool.output_schema = Some(Arc::new(Map::from_iter([
        ("type".to_owned(), json!("object")),
        ("additionalProperties".to_owned(), json!(false)),
        (
            "properties".to_owned(),
            json!({
                "ok": { "type": "boolean" },
                "summary": { "type": "string" },
                "data": {},
                "stdout": { "type": "string" },
                "stderr": { "type": "string" },
                "exitCode": { "anyOf": [{ "type": "number" }, { "type": "null" }] },
                "truncated": { "type": "boolean" }
            }),
        ),
        ("required".to_owned(), json!(["ok", "summary"])),
    ])));
}

pub fn configure_tool_metadata(tool: &mut Tool, config: &Config) {
    let profile = if config.runtime_mode == RuntimeMode::Docker {
        "docker"
    } else {
        "host"
    };
    let Some(metadata) = TOOL_METADATA
        .get(profile)
        .and_then(Value::as_object)
        .and_then(|tools| tools.get(tool.name.as_ref()))
        .and_then(Value::as_object)
    else {
        return;
    };
    tool.title = metadata
        .get("title")
        .and_then(Value::as_str)
        .map(|value| render_metadata_field(tool.name.as_ref(), "title", value, config));
    tool.description = metadata
        .get("description")
        .and_then(Value::as_str)
        .map(|value| {
            Cow::Owned(render_metadata_field(
                tool.name.as_ref(),
                "description",
                value,
                config,
            ))
        });
    tool.annotations = metadata
        .get("annotations")
        .cloned()
        .and_then(|value| serde_json::from_value::<ToolAnnotations>(value).ok());
}

#[derive(Debug, Clone)]
struct MetadataLabels {
    runtime_mode: RuntimeMode,
    runtime_title: String,
    runtime_label: String,
    host_title: String,
    host_command_title: String,
    is_windows: bool,
}

impl MetadataLabels {
    fn from_config(config: &Config) -> Self {
        Self::new(
            config.runtime_mode,
            &config.platform.display_name,
            config.platform.is_windows,
        )
    }

    fn new(runtime_mode: RuntimeMode, platform_name: &str, is_windows: bool) -> Self {
        let runtime_title = if runtime_mode == RuntimeMode::Docker {
            "Docker Devbox".to_owned()
        } else {
            format!("{platform_name} Host Devbox")
        };
        let runtime_label = if runtime_mode == RuntimeMode::Docker {
            "Docker devbox".to_owned()
        } else {
            format!("{platform_name} host devbox")
        };
        let host_title = if is_windows {
            "Windows Host".to_owned()
        } else {
            format!("{platform_name} Host")
        };
        let host_command_title = if is_windows {
            "Windows PowerShell".to_owned()
        } else {
            format!("{platform_name} Host Shell")
        };
        Self {
            runtime_mode,
            runtime_title,
            runtime_label,
            host_title,
            host_command_title,
            is_windows,
        }
    }
}

fn render_metadata_field(tool_name: &str, field: &str, value: &str, config: &Config) -> String {
    render_metadata_field_with_labels(
        tool_name,
        field,
        value,
        &MetadataLabels::from_config(config),
    )
}

fn render_metadata_field_with_labels(
    tool_name: &str,
    field: &str,
    value: &str,
    labels: &MetadataLabels,
) -> String {
    if tool_name == "host_exec" && field == "description" && !labels.is_windows {
        return format!(
            "Use this when you explicitly need native {} tooling rather than the {}, such as shell automation, git, node, python, or other host commands.",
            labels.host_title.to_lowercase(),
            labels.runtime_label
        );
    }

    let mut rendered = value.to_owned();
    if labels.runtime_mode != RuntimeMode::Docker {
        rendered = rendered
            .replace("Windows Host Devbox", &labels.runtime_title)
            .replace("Windows host devbox", &labels.runtime_label);
    }
    if is_dynamic_host_metadata(tool_name) && !labels.is_windows {
        rendered = rendered
            .replace("Windows PowerShell", &labels.host_command_title)
            .replace("Windows Host", &labels.host_title)
            .replace("windows host", &labels.host_title.to_lowercase());
    }
    rendered
}

fn is_dynamic_host_metadata(tool_name: &str) -> bool {
    matches!(
        tool_name,
        "host_status"
            | "windows_host_status"
            | "host_capture_display"
            | "host_capture_window"
            | "host_capture_program"
            | "windows_host_capture_display"
            | "windows_host_capture_program"
            | "host_exec"
            | "windows_host_exec"
            | "host_run_program"
            | "windows_host_run_program"
    )
}

pub fn configure_tool_input_schema(
    tool_name: &str,
    schema: &mut Map<String, Value>,
    config: &Config,
) {
    for value in schema.values_mut() {
        normalize_schema_artifacts(value);
    }
    let required = schema
        .get("required")
        .and_then(Value::as_array)
        .map(|values| {
            values
                .iter()
                .filter_map(Value::as_str)
                .map(str::to_owned)
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    let Some(properties) = schema.get_mut("properties").and_then(Value::as_object_mut) else {
        return;
    };

    if tool_name == "devbox_run_program" {
        properties.remove("input");
    }
    if tool_name == "devbox_exec_start" {
        properties.insert(
            "read_only".to_owned(),
            json!({ "type": "boolean", "default": false }),
        );
    }

    apply_dynamic_defaults(tool_name, properties, config);
    apply_common_constraints(tool_name, properties, config);
    apply_required_string_constraints(properties, &required);
}

fn apply_dynamic_defaults(tool_name: &str, properties: &mut Map<String, Value>, config: &Config) {
    if property(properties, "working_dir").is_some() {
        let value = if tool_name.starts_with("host_") || tool_name.starts_with("windows_host_") {
            config.host_default_workdir.to_string_lossy().into_owned()
        } else {
            config.devbox_workspace_path.to_string_lossy().into_owned()
        };
        set_default(properties, "working_dir", json!(value));
    }
    if property(properties, "user").is_some() {
        set_default(properties, "user", json!(config.devbox_default_user));
    }
    if matches!(tool_name, "devbox_list_files" | "devbox_search_files") {
        set_default(
            properties,
            "path",
            json!(config.devbox_workspace_path.to_string_lossy().into_owned()),
        );
    }
}

fn apply_common_constraints(tool_name: &str, properties: &mut Map<String, Value>, config: &Config) {
    set_enum(properties, "output_mode", &["head", "tail", "summary"]);
    set_enum(
        properties,
        "resource_class",
        &["auto", "watch", "light", "heavy", "io-heavy"],
    );
    set_integer_bounds(
        properties,
        "max_output_chars",
        100,
        config.command_output_limit_chars as u64,
    );
    set_default(
        properties,
        "max_output_chars",
        json!(config.command_output_limit_chars),
    );
    set_integer_bounds(properties, "max_output_lines", 0, 10_000);
    set_integer_bounds(properties, "quality", 1, 100);
    set_integer_bounds(properties, "pid", 1, MAX_SAFE_INTEGER);
    set_integer_bounds(properties, "max_chars", 100, 100_000);
    set_integer_bounds(properties, "max_entries", 1, 50_000);
    set_integer_bounds(properties, "max_matches", 1, 5_000);
    set_integer_bounds(properties, "max_file_bytes", 1, 64 * 1024 * 1024);
    let transfer_max = u64::try_from(config.max_mcp_transfer_chars)
        .unwrap_or(u64::MAX)
        .min(MAX_SAFE_INTEGER);
    set_integer_bounds(properties, "max_bytes", 1, transfer_max);
    set_integer_bounds(properties, "offset_bytes", 0, MAX_SAFE_INTEGER);
    set_integer_bounds(properties, "content_max_bytes", 1_024, transfer_max);
    set_integer_bounds(properties, "min_bytes", 0, MAX_SAFE_INTEGER);
    set_integer_bounds(properties, "stable_ms", 0, 30_000);
    set_integer_bounds(properties, "poll_ms", 50, 5_000);
    set_string_max_length(properties, "reason", 200);
    set_array_limits(properties, "exclude_directories", Some(32), Some(1));

    if tool_name == "devbox_list_files" {
        set_integer_bounds(properties, "max_depth", 1, 20);
    } else if tool_name == "devbox_search_files" {
        set_integer_bounds(properties, "max_depth", 1, 50);
    }

    let interactive_max = config.max_wait_seconds.min(85.0);
    set_number_bounds(properties, "seconds", 0.05, interactive_max);
    set_number_bounds(properties, "wait_seconds", 0.0, interactive_max);

    match tool_name {
        "devbox_wait_for_file" => {
            set_number_bounds(properties, "timeout_seconds", 0.1, interactive_max);
            set_default(
                properties,
                "timeout_seconds",
                json!(interactive_max.min(60.0)),
            );
        }
        "devbox_exec_start" | "devbox_run_program_start" => {
            set_integer_bounds(properties, "timeout_seconds", 1, 86_400);
        }
        "devbox_list_files" | "devbox_search_files" => {
            set_integer_bounds(properties, "timeout_seconds", 1, 300);
        }
        "devbox_exec"
        | "devbox_exec_readonly"
        | "devbox_run_program"
        | "host_exec"
        | "windows_host_exec"
        | "host_run_program"
        | "windows_host_run_program" => {
            set_integer_bounds(properties, "timeout_seconds", 1, 90);
        }
        _ => {}
    }
}

fn apply_required_string_constraints(properties: &mut Map<String, Value>, required: &[String]) {
    for key in ["command", "program", "path", "pattern"] {
        if required.iter().any(|value| value == key) {
            set_string_min_length(properties, key, 1);
        }
    }
    if required.iter().any(|value| value == "job_id") {
        set_string_min_length(properties, "job_id", 8);
    }
    set_string_pattern(properties, "expected_sha256", "^[A-Fa-f0-9]{64}$");
}

fn normalize_schema_artifacts(value: &mut Value) {
    match value {
        Value::Array(values) => {
            for value in values {
                normalize_schema_artifacts(value);
            }
        }
        Value::Object(object) => {
            object.remove("format");
            if let Some(Value::Array(types)) = object.get("type") {
                let non_null = types
                    .iter()
                    .filter(|value| value.as_str() != Some("null"))
                    .cloned()
                    .collect::<Vec<_>>();
                if non_null.len() == 1 && non_null.len() != types.len() {
                    object.insert("type".to_owned(), non_null[0].clone());
                    if object.get("default") == Some(&Value::Null) {
                        object.remove("default");
                    }
                }
            }
            for value in object.values_mut() {
                normalize_schema_artifacts(value);
            }
        }
        _ => {}
    }
}

fn property<'a>(
    properties: &'a mut Map<String, Value>,
    key: &str,
) -> Option<&'a mut Map<String, Value>> {
    properties.get_mut(key)?.as_object_mut()
}

fn set_default(properties: &mut Map<String, Value>, key: &str, value: Value) {
    if let Some(property) = property(properties, key) {
        property.insert("default".to_owned(), value);
    }
}

fn set_enum(properties: &mut Map<String, Value>, key: &str, values: &[&str]) {
    if let Some(property) = property(properties, key) {
        property.insert("enum".to_owned(), json!(values));
    }
}

fn set_integer_bounds(properties: &mut Map<String, Value>, key: &str, min: u64, max: u64) {
    if let Some(property) = property(properties, key) {
        property.insert("minimum".to_owned(), json!(min));
        property.insert("maximum".to_owned(), json!(max));
    }
}

fn set_number_bounds(properties: &mut Map<String, Value>, key: &str, min: f64, max: f64) {
    if let Some(property) = property(properties, key) {
        property.insert("minimum".to_owned(), json!(min));
        property.insert("maximum".to_owned(), json!(max));
    }
}

fn set_string_min_length(properties: &mut Map<String, Value>, key: &str, min: usize) {
    if let Some(property) = property(properties, key) {
        property.insert("minLength".to_owned(), json!(min));
    }
}

fn set_string_pattern(properties: &mut Map<String, Value>, key: &str, pattern: &str) {
    if let Some(property) = property(properties, key) {
        property.insert("pattern".to_owned(), json!(pattern));
    }
}

fn set_string_max_length(properties: &mut Map<String, Value>, key: &str, max: usize) {
    if let Some(property) = property(properties, key) {
        property.insert("maxLength".to_owned(), json!(max));
    }
}

fn set_array_limits(
    properties: &mut Map<String, Value>,
    key: &str,
    max_items: Option<usize>,
    item_min_length: Option<usize>,
) {
    let Some(property) = property(properties, key) else {
        return;
    };
    if let Some(max_items) = max_items {
        property.insert("maxItems".to_owned(), json!(max_items));
    }
    if let Some(item_min_length) = item_min_length
        && let Some(items) = property.get_mut("items").and_then(Value::as_object_mut)
    {
        items.insert("minLength".to_owned(), json!(item_min_length));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nullable_string_artifacts_are_removed_without_making_field_required() {
        let mut value = json!({
            "properties": {
                "input": {"type": ["string", "null"], "default": null, "format": "uint64"}
            },
            "required": []
        });
        for item in value.as_object_mut().expect("object").values_mut() {
            normalize_schema_artifacts(item);
        }
        assert_eq!(value["properties"]["input"]["type"], "string");
        assert!(value["properties"]["input"].get("default").is_none());
        assert!(value["properties"]["input"].get("format").is_none());
        assert_eq!(value["required"], json!([]));
    }

    #[test]
    fn linux_host_metadata_renders_runtime_and_host_labels() {
        let labels = MetadataLabels::new(RuntimeMode::Host, "Linux", false);
        let status = render_metadata_field_with_labels(
            "devbox_status",
            "title",
            "Windows Host Devbox Status",
            &labels,
        );
        assert_eq!(status, "Linux Host Devbox Status");

        let host_status = render_metadata_field_with_labels(
            "host_status",
            "description",
            "Use this when you need to inspect whether windows host execution is enabled and which native programs are allowed.",
            &labels,
        );
        assert_eq!(
            host_status,
            "Use this when you need to inspect whether linux host execution is enabled and which native programs are allowed."
        );

        let host_exec_title = render_metadata_field_with_labels(
            "host_exec",
            "title",
            "Run Windows PowerShell Command",
            &labels,
        );
        assert_eq!(host_exec_title, "Run Linux Host Shell Command");
    }

    #[test]
    fn linux_docker_metadata_keeps_docker_runtime_and_linux_host_tooling() {
        let labels = MetadataLabels::new(RuntimeMode::Docker, "Linux", false);
        let status = render_metadata_field_with_labels(
            "devbox_status",
            "title",
            "Docker Devbox Status",
            &labels,
        );
        assert_eq!(status, "Docker Devbox Status");

        let host_exec = render_metadata_field_with_labels(
            "host_exec",
            "description",
            "unused Windows host description",
            &labels,
        );
        assert_eq!(
            host_exec,
            "Use this when you explicitly need native linux host tooling rather than the Docker devbox, such as shell automation, git, node, python, or other host commands."
        );
    }

    #[test]
    fn fixed_windows_compatibility_tool_metadata_remains_windows_specific() {
        let labels = MetadataLabels::new(RuntimeMode::Host, "macOS", false);
        let fixed = render_metadata_field_with_labels(
            "windows_host_inspect_file",
            "title",
            "Inspect Windows Host File Integrity",
            &labels,
        );
        assert_eq!(fixed, "Inspect Windows Host File Integrity");
    }
}
