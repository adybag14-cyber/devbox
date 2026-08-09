use std::{
    collections::HashSet,
    net::{IpAddr, SocketAddr},
    sync::Arc,
};

use axum::{
    Json,
    body::Body,
    extract::{ConnectInfo, State},
    http::{HeaderMap, HeaderName, HeaderValue, Method, Request, StatusCode, header},
    middleware::Next,
    response::{IntoResponse, Response},
};
use serde_json::{Value, json};
use url::Url;

use crate::{AuthMode, Config};

const DEFAULT_ALLOW_HEADERS: &str =
    "authorization, content-type, last-event-id, mcp-protocol-version, mcp-session-id";
const ALLOW_METHODS: &str = "DELETE, GET, HEAD, OPTIONS, POST";
const EXPOSE_HEADERS: &str = "mcp-session-id";
const VARY_VALUE: &str = "Origin, Access-Control-Request-Method, Access-Control-Request-Headers, Access-Control-Request-Private-Network";

#[derive(Debug, Clone, Copy)]
pub struct GatewayRequestContext {
    pub is_local: bool,
}

#[derive(Debug)]
pub struct GatewayState {
    config: Arc<Config>,
    allowed_hosts: HashSet<String>,
    allowed_origins: Vec<String>,
}

impl GatewayState {
    #[must_use]
    pub fn new(config: Arc<Config>) -> Self {
        let mut allowed_hosts = ["127.0.0.1", "localhost", "[::1]"]
            .into_iter()
            .map(str::to_owned)
            .collect::<HashSet<_>>();
        if let Some(public_base_url) = config.public_base_url.as_deref()
            && let Ok(url) = Url::parse(public_base_url)
            && let Some(host) = url.host_str()
        {
            allowed_hosts.insert(host.to_ascii_lowercase());
        }
        let mut allowed_origins = Vec::new();
        for origin in config
            .gateway_bridge
            .origins
            .iter()
            .filter_map(|value| normalize_origin(value))
        {
            if !allowed_origins.contains(&origin) {
                allowed_origins.push(origin);
            }
        }
        Self {
            config,
            allowed_hosts,
            allowed_origins,
        }
    }

    #[must_use]
    pub fn bridge_info(&self, is_local: bool) -> Value {
        let expose = self.config.gateway_bridge.enabled
            && self.config.auth_mode == AuthMode::None
            && is_local;
        json!({
            "enabled": expose,
            "origins": if expose {
                self.config.gateway_bridge.origins.clone()
            } else {
                Vec::<String>::new()
            },
            "private_network_access": expose,
        })
    }
}

/// Enforce the JS server's Host-header validation and local `ChatGPT` gateway bridge policy.
pub async fn guard_and_bridge(
    State(state): State<Arc<GatewayState>>,
    mut request: Request<Body>,
    next: Next,
) -> Response {
    if let Some(response) = validate_host(request.headers(), &state.allowed_hosts) {
        return response;
    }

    let peer = request
        .extensions()
        .get::<ConnectInfo<SocketAddr>>()
        .map(|value| value.0);
    let is_local = request_is_local(request.headers(), peer);
    request
        .extensions_mut()
        .insert(GatewayRequestContext { is_local });

    let origin = request
        .headers()
        .get(header::ORIGIN)
        .and_then(|value| value.to_str().ok())
        .and_then(normalize_origin);
    let expose =
        state.config.gateway_bridge.enabled && state.config.auth_mode == AuthMode::None && is_local;
    let origin_allowed = origin
        .as_ref()
        .is_some_and(|value| state.allowed_origins.contains(value));

    if request.method() == Method::OPTIONS && origin.is_some() {
        if !expose {
            return (StatusCode::METHOD_NOT_ALLOWED, "").into_response();
        }
        if !origin_allowed {
            return (
                StatusCode::FORBIDDEN,
                Json(json!({ "error": "Origin is not allowed for the local ChatGPT gateway bridge." })),
            )
                .into_response();
        }
        let mut response = StatusCode::NO_CONTENT.into_response();
        apply_bridge_headers(
            request.headers(),
            &mut response,
            origin.as_deref().unwrap_or_default(),
        );
        return response;
    }

    let bridge_headers = (expose && origin_allowed).then(|| {
        (
            request.headers().clone(),
            origin.clone().unwrap_or_default(),
        )
    });
    let mut response = next.run(request).await;
    if let Some((headers, origin)) = bridge_headers {
        apply_bridge_headers(&headers, &mut response, &origin);
    }
    response
}

fn validate_host(headers: &HeaderMap, allowed_hosts: &HashSet<String>) -> Option<Response> {
    let Some(host_header) = headers
        .get(header::HOST)
        .and_then(|value| value.to_str().ok())
    else {
        return Some(host_failure("Missing Host header"));
    };
    let Some(hostname) = hostname_from_host_header(host_header) else {
        return Some(host_failure(&format!("Invalid Host header: {host_header}")));
    };
    if !allowed_hosts.contains(&hostname.to_ascii_lowercase()) {
        return Some(host_failure(&format!("Invalid Host: {hostname}")));
    }
    None
}

fn host_failure(message: &str) -> Response {
    (
        StatusCode::FORBIDDEN,
        Json(json!({
            "jsonrpc": "2.0",
            "error": { "code": -32000, "message": message },
            "id": Value::Null,
        })),
    )
        .into_response()
}

fn hostname_from_host_header(value: &str) -> Option<String> {
    Url::parse(&format!("http://{value}"))
        .ok()?
        .host_str()
        .map(str::to_owned)
}

fn normalize_origin(value: &str) -> Option<String> {
    let value = value.trim();
    if value.is_empty() {
        return None;
    }
    Url::parse(value)
        .ok()
        .map(|url| url.origin().ascii_serialization())
}

fn request_is_local(headers: &HeaderMap, peer: Option<SocketAddr>) -> bool {
    if headers
        .get(header::HOST)
        .and_then(|value| value.to_str().ok())
        .and_then(hostname_from_host_header)
        .is_some_and(|value| is_loopback_address(&value))
    {
        return true;
    }
    if let Some(forwarded) = headers
        .get("x-forwarded-for")
        .and_then(|value| value.to_str().ok())
    {
        return forwarded
            .split(',')
            .next()
            .map(str::trim)
            .is_some_and(is_loopback_address);
    }
    peer.is_some_and(|value| value.ip().is_loopback())
}

fn is_loopback_address(value: &str) -> bool {
    let normalized = value.trim().trim_matches(['[', ']']);
    if normalized.eq_ignore_ascii_case("localhost") {
        return true;
    }
    normalized.parse::<IpAddr>().is_ok_and(|address| {
        address.is_loopback()
            || matches!(address, IpAddr::V6(ipv6) if ipv6.to_ipv4_mapped().is_some_and(|ipv4| ipv4.is_loopback()))
    })
}

fn append_vary(headers: &mut HeaderMap, fields: &str) {
    let mut values = headers
        .get(header::VARY)
        .and_then(|value| value.to_str().ok())
        .unwrap_or_default()
        .split(',')
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .map(str::to_owned)
        .collect::<Vec<_>>();
    for field in fields
        .split(',')
        .map(str::trim)
        .filter(|value| !value.is_empty())
    {
        if !values.iter().any(|value| value.eq_ignore_ascii_case(field)) {
            values.push(field.to_owned());
        }
    }
    if let Ok(value) = HeaderValue::from_str(&values.join(", ")) {
        headers.insert(header::VARY, value);
    }
}

fn apply_bridge_headers(request_headers: &HeaderMap, response: &mut Response, origin: &str) {
    let headers = response.headers_mut();
    if let Ok(origin) = HeaderValue::from_str(origin) {
        headers.insert(header::ACCESS_CONTROL_ALLOW_ORIGIN, origin);
    }
    headers.insert(
        header::ACCESS_CONTROL_ALLOW_METHODS,
        HeaderValue::from_static(ALLOW_METHODS),
    );
    let requested_headers = request_headers
        .get(header::ACCESS_CONTROL_REQUEST_HEADERS)
        .and_then(|value| value.to_str().ok())
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .unwrap_or(DEFAULT_ALLOW_HEADERS);
    if let Ok(value) = HeaderValue::from_str(requested_headers) {
        headers.insert(header::ACCESS_CONTROL_ALLOW_HEADERS, value);
    }
    headers.insert(
        header::ACCESS_CONTROL_EXPOSE_HEADERS,
        HeaderValue::from_static(EXPOSE_HEADERS),
    );
    headers.insert(
        header::ACCESS_CONTROL_MAX_AGE,
        HeaderValue::from_static("600"),
    );
    append_vary(headers, VARY_VALUE);
    if request_headers
        .get("access-control-request-private-network")
        .and_then(|value| value.to_str().ok())
        .is_some_and(|value| value.eq_ignore_ascii_case("true"))
    {
        headers.insert(
            HeaderName::from_static("access-control-allow-private-network"),
            HeaderValue::from_static("true"),
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_parser_matches_ipv4_ipv6_and_hostname_forms() {
        assert_eq!(
            hostname_from_host_header("localhost:8100").as_deref(),
            Some("localhost")
        );
        assert_eq!(
            hostname_from_host_header("127.0.0.1:8100").as_deref(),
            Some("127.0.0.1")
        );
        assert_eq!(
            hostname_from_host_header("[::1]:8100").as_deref(),
            Some("[::1]")
        );
        assert!(hostname_from_host_header("bad host value").is_none());
    }

    #[test]
    fn origin_normalization_discards_paths_like_javascript_url_origin() {
        assert_eq!(
            normalize_origin("https://chatgpt.com/some/path").as_deref(),
            Some("https://chatgpt.com")
        );
        assert_eq!(
            normalize_origin(" https://chat.openai.com/ ").as_deref(),
            Some("https://chat.openai.com")
        );
        assert!(normalize_origin("").is_none());
    }

    #[test]
    fn vary_append_preserves_existing_fields() {
        let mut headers = HeaderMap::new();
        headers.insert(
            header::VARY,
            HeaderValue::from_static("Accept-Encoding, Origin"),
        );
        append_vary(&mut headers, "Origin, Access-Control-Request-Headers");
        assert_eq!(
            headers
                .get(header::VARY)
                .and_then(|value| value.to_str().ok()),
            Some("Accept-Encoding, Origin, Access-Control-Request-Headers")
        );
    }

    #[test]
    fn forwarded_loopback_detection_matches_javascript_bridge_rule() {
        let mut headers = HeaderMap::new();
        headers.insert(header::HOST, HeaderValue::from_static("example.com"));
        headers.insert(
            "x-forwarded-for",
            HeaderValue::from_static("203.0.113.4, 127.0.0.1"),
        );
        assert!(!request_is_local(
            &headers,
            Some("203.0.113.9:1234".parse().expect("peer"))
        ));
        headers.insert(
            "x-forwarded-for",
            HeaderValue::from_static("127.0.0.1, 203.0.113.4"),
        );
        assert!(request_is_local(
            &headers,
            Some("203.0.113.9:1234".parse().expect("peer"))
        ));
        headers.insert("x-forwarded-for", HeaderValue::from_static("203.0.113.4"));
        assert!(!request_is_local(
            &headers,
            Some("127.0.0.1:1234".parse().expect("peer"))
        ));
    }
}
