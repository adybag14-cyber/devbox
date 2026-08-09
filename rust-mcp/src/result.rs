use std::borrow::Cow;

use rmcp::model::{CallToolResult, ContentBlock};
use schemars::{JsonSchema, Schema, SchemaGenerator};
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct ToolEnvelope {
    pub ok: bool,
    pub summary: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[schemars(with = "Option<UnconstrainedJson>")]
    pub data: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub stdout: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub stderr: Option<String>,
    #[serde(rename = "exitCode", skip_serializing_if = "Option::is_none")]
    pub exit_code: Option<i32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub truncated: Option<bool>,
}

struct UnconstrainedJson;

impl JsonSchema for UnconstrainedJson {
    fn inline_schema() -> bool {
        true
    }

    fn schema_name() -> Cow<'static, str> {
        Cow::Borrowed("UnconstrainedJson")
    }

    fn json_schema(_: &mut SchemaGenerator) -> Schema {
        schemars::json_schema!({})
    }
}

impl ToolEnvelope {
    pub fn success(summary: impl Into<String>, data: Option<Value>) -> CallToolResult {
        let summary = summary.into();
        Self::render(
            &Self {
                ok: true,
                summary,
                data,
                stdout: None,
                stderr: None,
                exit_code: None,
                truncated: Some(false),
            },
            false,
            None,
        )
    }

    pub fn error(summary: impl Into<String>, data: Option<Value>) -> CallToolResult {
        let summary = summary.into();
        Self::render(
            &Self {
                ok: false,
                summary,
                data,
                stdout: None,
                stderr: None,
                exit_code: None,
                truncated: Some(false),
            },
            true,
            None,
        )
    }

    pub fn success_with_text(
        summary: impl Into<String>,
        data: Option<Value>,
        text: impl Into<String>,
    ) -> CallToolResult {
        let summary = summary.into();
        Self::render(
            &Self {
                ok: true,
                summary,
                data,
                stdout: None,
                stderr: None,
                exit_code: None,
                truncated: Some(false),
            },
            false,
            Some(text.into()),
        )
    }

    pub fn image_success(
        summary: impl Into<String>,
        data: Option<Value>,
        image_base64: impl Into<String>,
        mime_type: impl Into<String>,
    ) -> CallToolResult {
        let mut result = Self::success(summary, data);
        result
            .content
            .push(ContentBlock::image(image_base64.into(), mime_type.into()));
        result
    }

    pub fn process_success(
        summary: impl Into<String>,
        data: Option<Value>,
        stdout: impl Into<String>,
        stderr: impl Into<String>,
        exit_code: i32,
        truncated: bool,
    ) -> CallToolResult {
        let summary = summary.into();
        Self::render(
            &Self {
                ok: true,
                summary,
                data,
                stdout: Some(stdout.into()),
                stderr: Some(stderr.into()),
                exit_code: Some(exit_code),
                truncated: Some(truncated),
            },
            false,
            None,
        )
    }

    pub fn process_error(
        summary: impl Into<String>,
        data: Option<Value>,
        stdout: impl Into<String>,
        stderr: impl Into<String>,
        exit_code: Option<i32>,
        truncated: bool,
    ) -> CallToolResult {
        let summary = summary.into();
        Self::render(
            &Self {
                ok: false,
                summary,
                data,
                stdout: Some(stdout.into()),
                stderr: Some(stderr.into()),
                exit_code,
                truncated: Some(truncated),
            },
            true,
            None,
        )
    }

    fn render(envelope: &Self, is_error: bool, explicit_text: Option<String>) -> CallToolResult {
        let text = explicit_text.unwrap_or_else(|| envelope.text_content());
        let structured = serde_json::to_value(envelope).expect("ToolEnvelope is serializable");
        let mut result = if is_error {
            CallToolResult::error(vec![ContentBlock::text(text)])
        } else {
            CallToolResult::success(vec![ContentBlock::text(text)])
        };
        result.structured_content = Some(structured);
        result
    }

    fn text_content(&self) -> String {
        let mut parts = vec![self.summary.clone()];
        if let Some(value) = self.data.as_ref() {
            parts.push(serde_json::to_string_pretty(value).unwrap_or_else(|_| value.to_string()));
        }
        if let Some(stdout) = self.stdout.as_deref().filter(|value| !value.is_empty()) {
            parts.push(format!("stdout:\n{stdout}"));
        }
        if let Some(stderr) = self.stderr.as_deref().filter(|value| !value.is_empty()) {
            parts.push(format!("stderr:\n{stderr}"));
        }
        parts.join("\n\n")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arbitrary_data_is_optional_and_uses_object_form_unconstrained_schema() {
        let schema = schemars::schema_for!(ToolEnvelope);
        let value = serde_json::to_value(schema).expect("serialize schema");
        let required = value["required"].as_array().expect("required array");
        assert!(!required.iter().any(|item| item == "data"));
        let data = &value["properties"]["data"];
        assert!(data.is_object());
        assert!(!data.to_string().contains("true"));
    }

    #[test]
    fn image_success_adds_image_content_without_structured_base64() {
        let result = ToolEnvelope::image_success(
            "captured",
            Some(serde_json::json!({"mime_type":"image/jpeg","bytes":3})),
            "/9j/",
            "image/jpeg",
        );
        assert_eq!(result.content.len(), 2);
        assert!(result.content[1].as_image().is_some());
        let structured = result.structured_content.expect("structured content");
        assert!(!structured.to_string().contains("/9j/"));
    }

    #[test]
    fn process_result_preserves_stream_fields_and_text_sections() {
        let result = ToolEnvelope::process_success(
            "done",
            Some(serde_json::json!({"x": 1})),
            "hello",
            "warning",
            0,
            false,
        );
        let structured = result.structured_content.expect("structured content");
        assert_eq!(structured["stdout"], "hello");
        assert_eq!(structured["stderr"], "warning");
        assert_eq!(structured["exitCode"], 0);
        let text = result.content[0].as_text().expect("text content");
        assert!(text.text.contains("stdout:\nhello"));
        assert!(text.text.contains("stderr:\nwarning"));
    }
}
