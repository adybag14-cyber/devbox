use rmcp::model::{CallToolResult, ContentBlock};
use schemars::{JsonSchema, Schema, SchemaGenerator};
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct ToolEnvelope {
    pub ok: bool,
    pub summary: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    #[schemars(schema_with = "any_json_schema")]
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

fn any_json_schema(_: &mut SchemaGenerator) -> Schema {
    // schemars represents serde_json::Value as the boolean schema `true`.
    // The current JavaScript MCP SDK validator rejects boolean schemas inside
    // Tool.outputSchema. An empty object is the equivalent unconstrained JSON
    // Schema and matches the existing JS server's z.any() contract.
    schemars::json_schema!({})
}

impl ToolEnvelope {
    pub fn success(summary: impl Into<String>, data: Option<Value>) -> CallToolResult {
        Self::into_result(false, summary.into(), data, None)
    }

    pub fn error(summary: impl Into<String>, data: Option<Value>) -> CallToolResult {
        Self::into_result(true, summary.into(), data, None)
    }

    pub fn success_with_text(
        summary: impl Into<String>,
        data: Option<Value>,
        text: impl Into<String>,
    ) -> CallToolResult {
        Self::into_result(false, summary.into(), data, Some(text.into()))
    }

    fn into_result(
        is_error: bool,
        summary: String,
        data: Option<Value>,
        explicit_text: Option<String>,
    ) -> CallToolResult {
        let envelope = Self {
            ok: !is_error,
            summary: summary.clone(),
            data: data.clone(),
            stdout: None,
            stderr: None,
            exit_code: None,
            truncated: Some(false),
        };
        let structured = serde_json::to_value(&envelope).expect("ToolEnvelope is serializable");
        let text = explicit_text.unwrap_or_else(|| match data {
            Some(value) => format!(
                "{summary}\n\n{}",
                serde_json::to_string_pretty(&value).unwrap_or_else(|_| value.to_string())
            ),
            None => summary,
        });
        let mut result = if is_error {
            CallToolResult::error(vec![ContentBlock::text(text)])
        } else {
            CallToolResult::success(vec![ContentBlock::text(text)])
        };
        result.structured_content = Some(structured);
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arbitrary_data_uses_object_form_unconstrained_schema() {
        let schema = schemars::schema_for!(ToolEnvelope);
        let value = serde_json::to_value(schema).expect("serialize schema");
        assert_eq!(value["properties"]["data"], serde_json::json!({}));
    }
}
