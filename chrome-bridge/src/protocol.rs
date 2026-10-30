use serde_json::{Value, json};
use std::io::{self, Read, Write};
pub const REQUEST_LIMIT: usize = 256 * 1024;
pub const RESPONSE_LIMIT: usize = 8 * 1024 * 1024;
pub const NATIVE_OUTPUT_LIMIT: usize = 1024 * 1024;
pub const ACTIONS: &[&str] = &[
    "status",
    "tabs",
    "open",
    "observe",
    "screenshot",
    "read_html",
    "navigate",
    "back",
    "forward",
    "reload",
    "focus",
    "close",
    "click",
    "double_click",
    "move",
    "drag",
    "scroll",
    "type",
    "key",
    "select_text",
    "select_option",
    "wait",
];
pub fn nonce() -> Result<String, String> {
    let mut bytes = [0_u8; 16];
    getrandom::fill(&mut bytes).map_err(|e| e.to_string())?;
    Ok(bytes.iter().map(|b| format!("{b:02x}")).collect())
}
pub fn error(code: &str, message: impl ToString, input_sent: bool) -> Value {
    json!({"ok":false,"error":{"code":code,"message":message.to_string()},"input_sent":input_sent,
      "outcome":if input_sent {"unknown"} else {"not_sent"},"automatic_input_replay_allowed":false})
}
pub fn validate_request(value: &Value) -> Result<(), String> {
    let object = value.as_object().ok_or("Request must be an object")?;
    const FIELDS: &[&str] = &[
        "action",
        "tab_id",
        "observation_id",
        "request_id",
        "target_id",
        "url",
        "text",
        "key",
        "x",
        "y",
        "to_x",
        "to_y",
        "delta_x",
        "delta_y",
        "button",
        "replace",
        "screenshot",
        "active",
        "offset",
        "max_bytes",
        "wait_ms",
    ];
    if object.keys().any(|key| !FIELDS.contains(&key.as_str())) {
        return Err("Unknown or reserved request field".into());
    }
    let action = value
        .get("action")
        .and_then(Value::as_str)
        .ok_or("action is required")?;
    if !ACTIONS.contains(&action) {
        return Err("Unsupported browser action".into());
    }
    for (name, maximum) in [
        ("observation_id", 128),
        ("request_id", 64),
        ("target_id", 64),
        ("url", 8192),
        ("text", 16000),
        ("key", 64),
    ] {
        if let Some(item) = object.get(name) {
            if item.as_str().is_none_or(|s| s.chars().count() > maximum) {
                return Err(format!("Invalid {name}"));
            }
        }
    }
    for name in ["replace", "screenshot", "active"] {
        if object.get(name).is_some_and(|v| !v.is_boolean()) {
            return Err(format!("Invalid {name}"));
        }
    }
    for name in ["x", "y", "to_x", "to_y", "delta_x", "delta_y"] {
        if object.get(name).is_some_and(|v| {
            v.as_f64()
                .is_none_or(|n| !n.is_finite() || n.abs() > 100000.0)
        }) {
            return Err(format!("Invalid {name}"));
        }
    }
    for (name, minimum, maximum) in [
        ("tab_id", 0, 2147483647),
        ("offset", 0, 2097152),
        ("max_bytes", 1, 64000),
        ("wait_ms", 0, 1000),
    ] {
        if object
            .get(name)
            .is_some_and(|v| v.as_u64().is_none_or(|n| n < minimum || n > maximum))
        {
            return Err(format!("Invalid {name}"));
        }
    }
    if object
        .get("button")
        .is_some_and(|v| !matches!(v.as_str(), Some("left" | "right" | "middle")))
    {
        return Err("Invalid button".into());
    }
    if ["open", "navigate"].contains(&action) && object.get("url").and_then(Value::as_str).is_none()
    {
        return Err("url is required".into());
    }
    if ["type", "select_text", "select_option"].contains(&action)
        && object.get("text").and_then(Value::as_str).is_none()
    {
        return Err("text is required".into());
    }
    if action == "key" && object.get("key").and_then(Value::as_str).is_none() {
        return Err("key is required".into());
    }
    if !["status", "tabs", "observe", "screenshot", "read_html"].contains(&action)
        && !object.contains_key("request_id")
    {
        return Err("request_id is required for input".into());
    }
    if let Some(id) = value.get("request_id") {
        let id = id.as_str().ok_or("request_id must be a string")?;
        if id.is_empty()
            || id.len() > 64
            || !id
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"_-".contains(&b))
        {
            return Err("Invalid request_id".into());
        }
    }
    if serde_json::to_vec(value).map_err(|e| e.to_string())?.len() > REQUEST_LIMIT {
        return Err("Request too large".into());
    }
    Ok(())
}
pub fn read_frame(reader: &mut impl Read, maximum: usize) -> io::Result<Option<Value>> {
    let mut header = [0; 4];
    match reader.read(&mut header[..1])? {
        0 => return Ok(None),
        1 => {}
        _ => unreachable!(),
    }
    reader.read_exact(&mut header[1..])?;
    let size = u32::from_le_bytes(header) as usize;
    if size == 0 || size > maximum {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "frame size rejected",
        ));
    }
    let mut data = vec![0; size];
    reader.read_exact(&mut data)?;
    serde_json::from_slice(&data)
        .map(Some)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))
}
pub fn write_frame(writer: &mut impl Write, value: &Value, maximum: usize) -> io::Result<()> {
    let data = serde_json::to_vec(value)?;
    if data.len() > maximum {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "frame size rejected",
        ));
    }
    writer.write_all(&(data.len() as u32).to_le_bytes())?;
    writer.write_all(&data)?;
    writer.flush()
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn frame_bounds_and_truncation() {
        assert!(read_frame(&mut &u32::MAX.to_le_bytes()[..], REQUEST_LIMIT).is_err());
        assert!(read_frame(&mut &[4, 0, 0, 0, b'{'][..], REQUEST_LIMIT).is_err());
        assert!(read_frame(&mut &[][..], REQUEST_LIMIT).unwrap().is_none());
        let v = json!({"hello":"🌍"});
        let mut data = Vec::new();
        write_frame(&mut data, &v, 100).unwrap();
        assert_eq!(read_frame(&mut &data[..], 100).unwrap(), Some(v));
    }
    #[test]
    fn only_explicit_operations() {
        assert!(validate_request(&json!({"action":"eval"})).is_err());
        assert!(validate_request(&json!({"action":"click","request_id":"../other"})).is_err());
        assert!(validate_request(&json!({"action":"status"})).is_ok());
    }
    #[test]
    fn reject_reserved_fields_and_wrong_types_before_connecting() {
        for value in [
            json!({"action":"status","_bridge_deadline_unix_ms":99999999999999u64}),
            json!({"action":"status","id":"forged"}),
            json!({"action":"observe","tab_id":-1}),
            json!({"action":"type","text":"hi","replace":"false","request_id":"one"}),
            json!({"action":"read_html","max_bytes":64001}),
            json!({"action":"type","request_id":"one"}),
        ] {
            assert!(validate_request(&value).is_err(), "{value}");
        }
        assert!(validate_request(&json!({"action":"type","tab_id":1,"request_id":"one","text":"🌍","observation_id":"fresh"})).is_ok());
    }
}
