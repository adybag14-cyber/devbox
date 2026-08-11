use std::{
    collections::HashMap,
    fmt::Write as _,
    sync::{Arc, Mutex, PoisonError},
};

use axum::{
    body::Body,
    extract::{ConnectInfo, State},
    http::{HeaderMap, Request, request::Parts},
    middleware::Next,
    response::Response,
};
use rmcp::{RoleServer, model::NumberOrString, service::RequestContext};
use serde_json::Value;
use sha2::{Digest, Sha256};
use tokio_util::sync::CancellationToken;
use uuid::Uuid;

#[derive(Debug, Clone)]
pub struct CancellationNotification {
    pub request_id: Value,
}

#[derive(Debug, Clone)]
pub struct McpRequestIdentity {
    pub request_id: Value,
}

#[derive(Debug, Clone, Hash, PartialEq, Eq)]
struct ActiveRequestKey {
    authorization_digest: String,
    session_id: String,
    peer: String,
    user_agent: String,
    request_id_type: &'static str,
    request_id: String,
}

#[derive(Debug, Default)]
pub struct ActiveRequestRegistry {
    entries: Mutex<HashMap<ActiveRequestKey, HashMap<Uuid, CancellationToken>>>,
}

#[derive(Debug, Clone)]
pub struct DisconnectCancellation {
    registry: Arc<ActiveRequestRegistry>,
    key: ActiveRequestKey,
}

impl DisconnectCancellation {
    #[must_use]
    pub fn cancel(&self) -> usize {
        self.registry.cancel_key(&self.key)
    }
}

impl ActiveRequestRegistry {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    #[must_use]
    pub fn active_count(&self) -> usize {
        self.lock_entries().values().map(HashMap::len).sum()
    }

    #[must_use]
    pub fn register_context(
        self: &Arc<Self>,
        context: &RequestContext<RoleServer>,
    ) -> ActiveRequestGuard {
        let (request_id_type, request_id) = request_id_parts(&context.id);
        let parts = context.extensions.get::<Parts>();
        let headers = parts.map_or_else(HeaderMap::new, |value| value.headers.clone());
        let peer = parts.and_then(peer_from_parts);
        let key = request_key(&headers, peer.as_deref(), request_id_type, &request_id);
        let registration_id = Uuid::new_v4();
        self.lock_entries()
            .entry(key.clone())
            .or_default()
            .insert(registration_id, context.ct.clone());
        ActiveRequestGuard {
            registry: self.clone(),
            key,
            registration_id,
        }
    }

    #[must_use]
    pub fn disconnect_cancellation(
        self: &Arc<Self>,
        headers: &HeaderMap,
        peer: Option<&str>,
        request_id: &Value,
    ) -> Option<DisconnectCancellation> {
        let (request_id_type, request_id) = json_request_id_parts(request_id)?;
        Some(DisconnectCancellation {
            registry: self.clone(),
            key: request_key(headers, peer, request_id_type, &request_id),
        })
    }

    fn cancel_notification(
        &self,
        headers: &HeaderMap,
        peer: Option<&str>,
        request_id: &Value,
    ) -> usize {
        let Some((request_id_type, request_id)) = json_request_id_parts(request_id) else {
            return 0;
        };
        let key = request_key(headers, peer, request_id_type, &request_id);
        self.cancel_key(&key)
    }

    fn cancel_key(&self, key: &ActiveRequestKey) -> usize {
        let tokens = {
            let entries = self.lock_entries();
            let Some(tokens) = entries.get(key) else {
                return 0;
            };
            tokens.values().cloned().collect::<Vec<_>>()
        };
        let mut cancelled = 0_usize;
        for token in &tokens {
            if !token.is_cancelled() {
                token.cancel();
                cancelled = cancelled.saturating_add(1);
            }
        }
        cancelled
    }

    fn unregister(&self, key: &ActiveRequestKey, registration_id: Uuid) {
        let mut entries = self.lock_entries();
        let remove_key = if let Some(registrations) = entries.get_mut(key) {
            registrations.remove(&registration_id);
            registrations.is_empty()
        } else {
            false
        };
        if remove_key {
            entries.remove(key);
        }
    }

    fn lock_entries(
        &self,
    ) -> std::sync::MutexGuard<'_, HashMap<ActiveRequestKey, HashMap<Uuid, CancellationToken>>>
    {
        self.entries.lock().unwrap_or_else(PoisonError::into_inner)
    }
}

#[derive(Debug)]
pub struct ActiveRequestGuard {
    registry: Arc<ActiveRequestRegistry>,
    key: ActiveRequestKey,
    registration_id: Uuid,
}

impl Drop for ActiveRequestGuard {
    fn drop(&mut self) {
        self.registry.unregister(&self.key, self.registration_id);
    }
}

/// Apply a previously parsed stateless MCP cancellation notification to matching active calls.
pub async fn apply_cancellation_notification(
    State(registry): State<Arc<ActiveRequestRegistry>>,
    request: Request<Body>,
    next: Next,
) -> Response {
    if let Some(notification) = request.extensions().get::<CancellationNotification>() {
        let peer = request
            .extensions()
            .get::<ConnectInfo<std::net::SocketAddr>>()
            .map(|value| value.0.ip().to_string());
        let cancelled = registry.cancel_notification(
            request.headers(),
            peer.as_deref(),
            &notification.request_id,
        );
        tracing::debug!(cancelled, "Applied stateless MCP cancellation notification");
    }
    next.run(request).await
}

fn request_key(
    headers: &HeaderMap,
    peer: Option<&str>,
    request_id_type: &'static str,
    request_id: &str,
) -> ActiveRequestKey {
    ActiveRequestKey {
        authorization_digest: authorization_digest(headers),
        session_id: header_text(headers, "mcp-session-id"),
        peer: peer.unwrap_or_default().to_owned(),
        user_agent: header_text(headers, "user-agent"),
        request_id_type,
        request_id: request_id.to_owned(),
    }
}

fn authorization_digest(headers: &HeaderMap) -> String {
    let value = header_text(headers, "authorization");
    if value.is_empty() {
        return String::new();
    }
    let digest = Sha256::digest(value.as_bytes());
    let mut encoded = String::with_capacity(digest.len() * 2);
    for byte in digest {
        let _ = write!(&mut encoded, "{byte:02x}");
    }
    encoded
}

fn header_text(headers: &HeaderMap, name: &str) -> String {
    headers
        .get(name)
        .and_then(|value| value.to_str().ok())
        .unwrap_or_default()
        .to_owned()
}

fn peer_from_parts(parts: &Parts) -> Option<String> {
    parts
        .extensions
        .get::<ConnectInfo<std::net::SocketAddr>>()
        .map(|value| value.0.ip().to_string())
}

fn request_id_parts(id: &NumberOrString) -> (&'static str, String) {
    match id {
        NumberOrString::Number(value) => ("number", value.to_string()),
        NumberOrString::String(value) => ("string", value.to_string()),
    }
}

fn json_request_id_parts(value: &Value) -> Option<(&'static str, String)> {
    match value {
        Value::Number(value) => Some(("number", value.to_string())),
        Value::String(value) => Some(("string", value.clone())),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn forwarded_for_header_cannot_spoof_request_identity() {
        let mut headers = HeaderMap::new();
        headers.insert("x-forwarded-for", "203.0.113.99".parse().expect("header"));
        headers.insert(
            "authorization",
            "Bearer secret-value".parse().expect("header"),
        );
        let key = request_key(&headers, Some("127.0.0.1"), "number", "7");
        assert_eq!(key.peer, "127.0.0.1");
        assert_eq!(key.authorization_digest.len(), 64);
        assert!(!key.authorization_digest.contains("secret-value"));
    }

    #[test]
    fn matching_notification_cancels_only_matching_peer_and_id() {
        let registry = ActiveRequestRegistry::new();
        let mut headers = HeaderMap::new();
        headers.insert("user-agent", "smoke-client".parse().expect("header"));
        let token = CancellationToken::new();
        let key = request_key(&headers, Some("127.0.0.1"), "number", "7");
        registry
            .lock_entries()
            .entry(key)
            .or_default()
            .insert(Uuid::new_v4(), token.clone());
        assert_eq!(
            registry.cancel_notification(&headers, Some("127.0.0.1"), &Value::from(8)),
            0
        );
        assert!(!token.is_cancelled());
        assert_eq!(
            registry.cancel_notification(&headers, Some("127.0.0.1"), &Value::from(7)),
            1
        );
        assert!(token.is_cancelled());
    }
}
