use std::{
    collections::{BTreeMap, HashMap},
    path::PathBuf,
    sync::Arc,
    time::{SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result};
use axum::{
    Form, Json, Router,
    body::Body,
    extract::{Query, State},
    http::{HeaderMap, Method, Request, StatusCode, header},
    middleware::Next,
    response::{IntoResponse, Response},
    routing::{get, post},
};
use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
use jsonwebtoken::{Algorithm, DecodingKey, Validation, decode, decode_header, jwk::JwkSet};
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value, json};
use sha2::{Digest, Sha256};
use tokio::{fs, sync::Mutex};
use url::Url;
use uuid::Uuid;

use crate::{AuthMode, Config, lifecycle::replace_file_preserving_previous};

const ACCESS_TOKEN_TTL_MS: u64 = 60 * 60 * 1_000;
const REFRESH_TOKEN_TTL_MS: u64 = 7 * 24 * 60 * 60 * 1_000;
const AUTHORIZATION_CODE_TTL_MS: u64 = 10 * 60 * 1_000;
const CLIENT_SECRET_TTL_SECONDS: u64 = 30 * 24 * 60 * 60;
const JWKS_CACHE_TTL_MS: u64 = 5 * 60 * 1_000;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct AuthorizationCodeRecord {
    client_id: String,
    expires_at: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    identity: Option<Value>,
    params: AuthorizationParams,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct AuthorizationParams {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    state: Option<String>,
    #[serde(default)]
    scopes: Vec<String>,
    #[serde(rename = "redirectUri")]
    redirect_uri: String,
    #[serde(rename = "codeChallenge")]
    code_challenge: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    resource: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct TokenRecord {
    client_id: String,
    #[serde(default)]
    scopes: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    resource: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    identity: Option<Value>,
    expires_at: u64,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DiskState {
    #[serde(default)]
    clients: Vec<(String, Value)>,
    #[serde(default)]
    authorization_codes: Vec<(String, AuthorizationCodeRecord)>,
    #[serde(default)]
    access_tokens: Vec<(String, TokenRecord)>,
    #[serde(default)]
    refresh_tokens: Vec<(String, TokenRecord)>,
}

#[derive(Debug, Default)]
struct OAuthState {
    loaded: bool,
    clients: BTreeMap<String, Value>,
    authorization_codes: BTreeMap<String, AuthorizationCodeRecord>,
    access_tokens: BTreeMap<String, TokenRecord>,
    refresh_tokens: BTreeMap<String, TokenRecord>,
}

impl OAuthState {
    fn from_disk(disk: DiskState) -> Self {
        Self {
            loaded: true,
            clients: disk.clients.into_iter().collect(),
            authorization_codes: disk.authorization_codes.into_iter().collect(),
            access_tokens: disk.access_tokens.into_iter().collect(),
            refresh_tokens: disk.refresh_tokens.into_iter().collect(),
        }
    }

    fn snapshot(&self) -> DiskState {
        DiskState {
            clients: self
                .clients
                .iter()
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect(),
            authorization_codes: self
                .authorization_codes
                .iter()
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect(),
            access_tokens: self
                .access_tokens
                .iter()
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect(),
            refresh_tokens: self
                .refresh_tokens
                .iter()
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect(),
        }
    }

    fn prune_expired(&mut self, now_ms: u64) -> bool {
        let before =
            self.authorization_codes.len() + self.access_tokens.len() + self.refresh_tokens.len();
        self.authorization_codes
            .retain(|_, value| value.expires_at > now_ms);
        self.access_tokens
            .retain(|_, value| value.expires_at > now_ms);
        self.refresh_tokens
            .retain(|_, value| value.expires_at > now_ms);
        before
            != self.authorization_codes.len() + self.access_tokens.len() + self.refresh_tokens.len()
    }
}

#[derive(Debug)]
struct CachedJwks {
    fetched_at_ms: u64,
    set: JwkSet,
}

#[derive(Debug, Clone)]
pub struct OAuthService {
    mode: AuthMode,
    state_file: PathBuf,
    public_base_url: String,
    resource_name: String,
    max_clients: usize,
    cloudflare_issuer: Option<String>,
    cloudflare_audience: String,
    cloudflare_jwks_url: Option<String>,
    http: reqwest::Client,
    jwks_cache: Arc<Mutex<Option<CachedJwks>>>,
    state: Arc<Mutex<OAuthState>>,
}

#[derive(Debug, Clone)]
pub struct AuthorizationRequest {
    pub client_id: String,
    pub redirect_uri: Option<String>,
    pub response_type: String,
    pub code_challenge: String,
    pub code_challenge_method: String,
    pub scope: Option<String>,
    pub state: Option<String>,
    pub resource: Option<String>,
}

#[derive(Debug, Clone)]
pub struct AuthorizationRedirect {
    pub location: String,
}

#[derive(Debug, Clone)]
pub struct TokenRequest {
    pub grant_type: String,
    pub client_id: String,
    pub client_secret: Option<String>,
    pub code: Option<String>,
    pub code_verifier: Option<String>,
    pub redirect_uri: Option<String>,
    pub refresh_token: Option<String>,
    pub scope: Option<String>,
    pub resource: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct TokenResponse {
    pub access_token: String,
    pub token_type: &'static str,
    pub expires_in: u64,
    pub refresh_token: String,
    pub scope: String,
}

#[derive(Debug, Clone)]
pub struct VerifiedAccessToken {
    pub client_id: String,
    pub scopes: Vec<String>,
    pub expires_at: u64,
    pub resource: Option<String>,
    pub identity: Option<Value>,
}

#[derive(Debug, Clone)]
pub struct OAuthRequestInfo {
    pub client_id: String,
    pub scopes: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct OAuthFailure {
    pub code: &'static str,
    pub message: String,
    pub status: u16,
    safe_redirect_uri: Option<String>,
}

impl OAuthFailure {
    #[must_use]
    pub fn invalid_request(message: impl Into<String>) -> Self {
        Self {
            code: "invalid_request",
            message: message.into(),
            status: 400,
            safe_redirect_uri: None,
        }
    }

    #[must_use]
    pub fn invalid_client_metadata(message: impl Into<String>) -> Self {
        Self {
            code: "invalid_client_metadata",
            message: message.into(),
            status: 400,
            safe_redirect_uri: None,
        }
    }

    #[must_use]
    pub fn invalid_client(message: impl Into<String>) -> Self {
        Self {
            code: "invalid_client",
            message: message.into(),
            status: 400,
            safe_redirect_uri: None,
        }
    }

    #[must_use]
    pub fn invalid_grant(message: impl Into<String>) -> Self {
        Self {
            code: "invalid_grant",
            message: message.into(),
            status: 400,
            safe_redirect_uri: None,
        }
    }

    #[must_use]
    pub fn unsupported_grant(message: impl Into<String>) -> Self {
        Self {
            code: "unsupported_grant_type",
            message: message.into(),
            status: 400,
            safe_redirect_uri: None,
        }
    }

    #[must_use]
    pub fn invalid_token(message: impl Into<String>) -> Self {
        Self {
            code: "invalid_token",
            message: message.into(),
            status: 401,
            safe_redirect_uri: None,
        }
    }

    #[must_use]
    pub fn server(message: impl Into<String>) -> Self {
        Self {
            code: "server_error",
            message: message.into(),
            status: 500,
            safe_redirect_uri: None,
        }
    }

    #[must_use]
    fn with_safe_redirect_uri(mut self, redirect_uri: &str) -> Self {
        self.safe_redirect_uri = Some(redirect_uri.to_owned());
        self
    }

    #[must_use]
    pub fn response(&self) -> Value {
        json!({ "error": self.code, "error_description": self.message })
    }
}

fn prune_clients_for_capacity(state: &mut OAuthState, max_clients: usize) -> usize {
    if state.clients.len() < max_clients {
        return 0;
    }
    let mut referenced = std::collections::BTreeSet::new();
    referenced.extend(
        state
            .authorization_codes
            .values()
            .map(|record| record.client_id.clone()),
    );
    referenced.extend(
        state
            .access_tokens
            .values()
            .map(|record| record.client_id.clone()),
    );
    referenced.extend(
        state
            .refresh_tokens
            .values()
            .map(|record| record.client_id.clone()),
    );
    let mut candidates = state
        .clients
        .iter()
        .filter(|(client_id, _)| !referenced.contains(*client_id))
        .map(|(client_id, metadata)| {
            let issued = metadata
                .get("client_id_issued_at")
                .and_then(Value::as_u64)
                .unwrap_or(0);
            (issued, client_id.clone())
        })
        .collect::<Vec<_>>();
    candidates.sort();
    let mut removed = 0;
    for (_, client_id) in candidates {
        if state.clients.len() < max_clients {
            break;
        }
        if state.clients.remove(&client_id).is_some() {
            removed += 1;
        }
    }
    removed
}

impl OAuthService {
    #[must_use]
    pub fn new(config: &Config) -> Option<Self> {
        let public_base_url = config.public_base_url.clone()?;
        (config.auth_mode != AuthMode::None).then(|| Self {
            mode: config.auth_mode,
            state_file: config.oauth_state_file_path.clone(),
            public_base_url,
            resource_name: config.server_name(),
            max_clients: config.oauth_max_clients.max(1),
            cloudflare_issuer: config.cloudflare_access_team_domain.clone(),
            cloudflare_audience: config.cloudflare_access_aud.clone(),
            cloudflare_jwks_url: config.cloudflare_access_jwks_url.clone().or_else(|| {
                config
                    .cloudflare_access_team_domain
                    .as_ref()
                    .map(|issuer| format!("{issuer}/cdn-cgi/access/certs"))
            }),
            http: reqwest::Client::new(),
            jwks_cache: Arc::new(Mutex::new(None)),
            state: Arc::new(Mutex::new(OAuthState::default())),
        })
    }

    #[must_use]
    pub fn authorization_server_metadata(&self) -> Value {
        json!({
            "issuer": self.issuer_url(),
            "authorization_endpoint": self.endpoint("authorize"),
            "response_types_supported": ["code"],
            "code_challenge_methods_supported": ["S256"],
            "token_endpoint": self.endpoint("token"),
            "token_endpoint_auth_methods_supported": ["client_secret_post", "none"],
            "grant_types_supported": ["authorization_code", "refresh_token"],
            "scopes_supported": [
                "mcp:tools",
                "mcp:devbox:read",
                "mcp:devbox:exec",
                "mcp:host:read",
                "mcp:host:exec",
                "mcp:admin"
            ],
            "revocation_endpoint": self.endpoint("revoke"),
            "revocation_endpoint_auth_methods_supported": ["client_secret_post"],
            "registration_endpoint": self.endpoint("register"),
        })
    }

    #[must_use]
    pub fn protected_resource_metadata(&self, legacy: bool) -> Value {
        let resource = if legacy {
            self.endpoint("mcp")
        } else {
            self.endpoint("")
        };
        json!({
            "resource": resource,
            "authorization_servers": [self.issuer_url()],
            "scopes_supported": [
                "mcp:tools",
                "mcp:devbox:read",
                "mcp:devbox:exec",
                "mcp:host:read",
                "mcp:host:exec",
                "mcp:admin"
            ],
            "resource_name": if legacy { "Docker ChatGPT Devbox MCP" } else { &self.resource_name },
        })
    }

    #[must_use]
    pub fn oauth_info(&self) -> Value {
        json!({
            "issuer": self.issuer_url(),
            "resourceMetadataUrl": self.endpoint(".well-known/oauth-protected-resource"),
            "legacyResourceMetadataUrl": self.endpoint(".well-known/oauth-protected-resource/mcp"),
        })
    }

    #[must_use]
    pub fn resource_metadata_url(&self, legacy: bool) -> String {
        if legacy {
            self.endpoint(".well-known/oauth-protected-resource/mcp")
        } else {
            self.endpoint(".well-known/oauth-protected-resource")
        }
    }

    /// Register one OAuth client using the same public/secret client semantics as the JS SDK router.
    ///
    /// # Errors
    /// Returns an OAuth failure for malformed metadata and an I/O failure for persistence errors.
    pub async fn register_client(
        &self,
        metadata: Value,
    ) -> std::result::Result<Value, OAuthFailure> {
        let mut object = metadata.as_object().cloned().ok_or_else(|| {
            OAuthFailure::invalid_client_metadata("Client metadata must be a JSON object")
        })?;
        sanitize_client_metadata(&mut object);
        validate_client_metadata(&mut object)?;
        let public = object
            .get("token_endpoint_auth_method")
            .and_then(Value::as_str)
            == Some("none");
        let issued_at = unix_seconds();
        let client_id = Uuid::new_v4().to_string();
        object.insert("client_id".to_owned(), Value::String(client_id.clone()));
        object.insert("client_id_issued_at".to_owned(), json!(issued_at));
        if !public {
            object.insert(
                "client_secret".to_owned(),
                Value::String(random_client_secret()),
            );
            object.insert(
                "client_secret_expires_at".to_owned(),
                json!(issued_at.saturating_add(CLIENT_SECRET_TTL_SECONDS)),
            );
        }
        let value = Value::Object(object);
        let mut state = self.state.lock().await;
        self.ensure_loaded_locked(&mut state)
            .await
            .map_err(internal_failure)?;
        prune_clients_for_capacity(&mut state, self.max_clients);
        if state.clients.len() >= self.max_clients {
            return Err(OAuthFailure::invalid_client_metadata(
                "OAuth client registry capacity is occupied by active clients; retry after older sessions expire or are revoked.",
            ));
        }
        state.clients.insert(client_id, value.clone());
        self.persist_locked(&state)
            .await
            .map_err(internal_failure)?;
        Ok(value)
    }

    /// Validate and fulfill an OAuth authorization-code request.
    ///
    /// # Errors
    /// Returns standards-shaped OAuth failures or persistence failures.
    pub async fn authorize(
        &self,
        request: AuthorizationRequest,
        cloudflare_assertion: Option<&str>,
        cloudflare_email: Option<&str>,
    ) -> std::result::Result<AuthorizationRedirect, OAuthFailure> {
        let mut state = self.state.lock().await;
        self.ensure_loaded_locked(&mut state)
            .await
            .map_err(internal_failure)?;
        let client = state
            .clients
            .get(&request.client_id)
            .cloned()
            .ok_or_else(|| OAuthFailure::invalid_client("Invalid client_id"))?;
        let registered = string_array(client.get("redirect_uris")).ok_or_else(|| {
            OAuthFailure::invalid_request("Registered client has no redirect_uris")
        })?;
        let redirect_uri = resolve_redirect_uri(request.redirect_uri.as_deref(), &registered)?;
        if request.response_type != "code" {
            return Err(OAuthFailure::invalid_request("response_type must be code")
                .with_safe_redirect_uri(&redirect_uri));
        }
        if request.code_challenge.is_empty() || request.code_challenge_method != "S256" {
            return Err(
                OAuthFailure::invalid_request("code_challenge and S256 are required")
                    .with_safe_redirect_uri(&redirect_uri),
            );
        }
        if let Some(resource) = request.resource.as_deref() {
            Url::parse(resource).map_err(|_| {
                OAuthFailure::invalid_request("resource must be a valid URL")
                    .with_safe_redirect_uri(&redirect_uri)
            })?;
        }
        let identity = if self.mode == AuthMode::CloudflareAccess {
            Some(
                self.verify_cloudflare_access_identity(cloudflare_assertion, cloudflare_email)
                    .await
                    .map_err(|failure| failure.with_safe_redirect_uri(&redirect_uri))?,
            )
        } else {
            None
        };
        let code = Uuid::new_v4().to_string();
        state.authorization_codes.insert(
            code.clone(),
            AuthorizationCodeRecord {
                client_id: request.client_id,
                expires_at: unix_millis().saturating_add(AUTHORIZATION_CODE_TTL_MS),
                identity,
                params: AuthorizationParams {
                    state: request.state.clone(),
                    scopes: split_scope(request.scope.as_deref()),
                    redirect_uri: redirect_uri.clone(),
                    code_challenge: request.code_challenge,
                    resource: request.resource,
                },
            },
        );
        self.persist_locked(&state)
            .await
            .map_err(|error| internal_failure(error).with_safe_redirect_uri(&redirect_uri))?;
        let mut target = Url::parse(&redirect_uri).map_err(|_| {
            OAuthFailure::invalid_request("redirect_uri must be a valid URL")
                .with_safe_redirect_uri(&redirect_uri)
        })?;
        target.query_pairs_mut().append_pair("code", &code);
        if let Some(value) = request.state {
            target.query_pairs_mut().append_pair("state", &value);
        }
        Ok(AuthorizationRedirect {
            location: target.to_string(),
        })
    }

    /// Exchange an authorization code or refresh token.
    ///
    /// # Errors
    /// Returns OAuth client/grant failures or persistence failures.
    pub async fn exchange_token(
        &self,
        request: TokenRequest,
    ) -> std::result::Result<TokenResponse, OAuthFailure> {
        let mut state = self.state.lock().await;
        self.ensure_loaded_locked(&mut state)
            .await
            .map_err(internal_failure)?;
        authenticate_client(&state, &request.client_id, request.client_secret.as_deref())?;
        match request.grant_type.as_str() {
            "authorization_code" => self.exchange_authorization_code(&mut state, request).await,
            "refresh_token" => self.exchange_refresh_token(&mut state, request).await,
            _ => Err(OAuthFailure::unsupported_grant(
                "The grant type is not supported by this authorization server.",
            )),
        }
    }

    async fn exchange_authorization_code(
        &self,
        state: &mut OAuthState,
        request: TokenRequest,
    ) -> std::result::Result<TokenResponse, OAuthFailure> {
        let code = request
            .code
            .as_deref()
            .ok_or_else(|| OAuthFailure::invalid_request("code is required"))?;
        let verifier = request
            .code_verifier
            .as_deref()
            .ok_or_else(|| OAuthFailure::invalid_request("code_verifier is required"))?;
        let record = state
            .authorization_codes
            .get(code)
            .cloned()
            .ok_or_else(|| OAuthFailure::invalid_grant("Invalid authorization code"))?;
        if record.expires_at <= unix_millis() {
            state.authorization_codes.remove(code);
            self.persist_locked(state).await.map_err(internal_failure)?;
            return Err(OAuthFailure::invalid_grant("Authorization code expired"));
        }
        if record.client_id != request.client_id {
            return Err(OAuthFailure::invalid_grant(
                "Authorization code was not issued to this client",
            ));
        }
        if pkce_challenge(verifier) != record.params.code_challenge {
            return Err(OAuthFailure::invalid_grant(
                "code_verifier does not match the challenge",
            ));
        }
        if let Some(redirect) = request.redirect_uri.as_deref()
            && redirect != record.params.redirect_uri
        {
            return Err(OAuthFailure::invalid_grant(
                "redirect_uri does not match the authorization request",
            ));
        }
        state.authorization_codes.remove(code);
        let result = issue_tokens(
            state,
            &request.client_id,
            record.params.scopes,
            request.resource.or(record.params.resource),
            record.identity,
        );
        self.persist_locked(state).await.map_err(internal_failure)?;
        Ok(result)
    }

    async fn exchange_refresh_token(
        &self,
        state: &mut OAuthState,
        request: TokenRequest,
    ) -> std::result::Result<TokenResponse, OAuthFailure> {
        let refresh = request
            .refresh_token
            .as_deref()
            .ok_or_else(|| OAuthFailure::invalid_request("refresh_token is required"))?;
        let record = state
            .refresh_tokens
            .get(refresh)
            .cloned()
            .ok_or_else(|| OAuthFailure::invalid_grant("Invalid refresh token"))?;
        if record.expires_at <= unix_millis() {
            state.refresh_tokens.remove(refresh);
            self.persist_locked(state).await.map_err(internal_failure)?;
            return Err(OAuthFailure::invalid_grant("Refresh token expired"));
        }
        if record.client_id != request.client_id {
            return Err(OAuthFailure::invalid_grant(
                "Refresh token was not issued to this client",
            ));
        }
        let scopes = request
            .scope
            .as_deref()
            .map(|value| split_scope(Some(value)))
            .filter(|value| !value.is_empty())
            .unwrap_or(record.scopes);
        state.refresh_tokens.remove(refresh);
        let result = issue_tokens(
            state,
            &request.client_id,
            scopes,
            request.resource.or(record.resource),
            record.identity,
        );
        self.persist_locked(state).await.map_err(internal_failure)?;
        Ok(result)
    }

    /// Revoke one access or refresh token after authenticating its client.
    ///
    /// # Errors
    /// Returns OAuth client failures or persistence failures.
    pub async fn revoke(
        &self,
        client_id: &str,
        client_secret: Option<&str>,
        token: &str,
    ) -> std::result::Result<(), OAuthFailure> {
        let mut state = self.state.lock().await;
        self.ensure_loaded_locked(&mut state)
            .await
            .map_err(internal_failure)?;
        authenticate_client(&state, client_id, client_secret)?;
        state.access_tokens.remove(token);
        state.refresh_tokens.remove(token);
        self.persist_locked(&state)
            .await
            .map_err(internal_failure)?;
        Ok(())
    }

    /// Verify one bearer access token for MCP endpoint protection.
    ///
    /// # Errors
    /// Returns invalid-token or persistence failures.
    pub async fn verify_access_token(
        &self,
        token: &str,
    ) -> std::result::Result<VerifiedAccessToken, OAuthFailure> {
        let mut state = self.state.lock().await;
        self.ensure_loaded_locked(&mut state)
            .await
            .map_err(internal_failure)?;
        let Some(record) = state.access_tokens.get(token).cloned() else {
            return Err(OAuthFailure::invalid_token("Unknown access token"));
        };
        if record.expires_at <= unix_millis() {
            state.access_tokens.remove(token);
            self.persist_locked(&state)
                .await
                .map_err(internal_failure)?;
            return Err(OAuthFailure::invalid_token("Access token expired"));
        }
        Ok(VerifiedAccessToken {
            client_id: record.client_id,
            scopes: record.scopes,
            expires_at: record.expires_at / 1_000,
            resource: record.resource,
            identity: record.identity,
        })
    }

    async fn verify_cloudflare_access_identity(
        &self,
        assertion: Option<&str>,
        fallback_email: Option<&str>,
    ) -> std::result::Result<Value, OAuthFailure> {
        let assertion = assertion.ok_or_else(|| {
            OAuthFailure::invalid_request(
                "Cloudflare Access authentication is required on /authorize. Protect that path with a Cloudflare Access application.",
            )
        })?;
        let issuer = self.cloudflare_issuer.as_deref().ok_or_else(|| {
            OAuthFailure::server("Cloudflare Access team domain is not configured")
        })?;
        let header = decode_header(assertion).map_err(|error| {
            OAuthFailure::server(format!(
                "Cloudflare Access JWT verification failed: {error}"
            ))
        })?;
        if header.alg != Algorithm::RS256 {
            return Err(OAuthFailure::server(format!(
                "Cloudflare Access JWT verification failed: unsupported algorithm {:?}",
                header.alg
            )));
        }
        let kid = header.kid.as_deref().ok_or_else(|| {
            OAuthFailure::server("Cloudflare Access JWT verification failed: missing kid")
        })?;
        let key = self.cloudflare_decoding_key(kid).await?;
        let mut validation = Validation::new(Algorithm::RS256);
        validation.set_issuer(&[issuer]);
        validation.set_audience(&[self.cloudflare_audience.as_str()]);
        let payload = decode::<Value>(assertion, &key, &validation)
            .map_err(|error| {
                OAuthFailure::server(format!(
                    "Cloudflare Access JWT verification failed: {error}"
                ))
            })?
            .claims;
        Ok(json!({
            "sub": payload.get("sub").and_then(Value::as_str).unwrap_or_default(),
            "email": payload
                .get("email")
                .and_then(Value::as_str)
                .or(fallback_email),
            "name": payload.get("name").and_then(Value::as_str),
            "aud": payload.get("aud").cloned().unwrap_or(Value::Null),
            "iss": payload.get("iss").cloned().unwrap_or(Value::Null),
        }))
    }

    async fn cloudflare_decoding_key(
        &self,
        kid: &str,
    ) -> std::result::Result<DecodingKey, OAuthFailure> {
        if let Some(key) = self.cached_decoding_key(kid).await? {
            return Ok(key);
        }
        self.refresh_jwks().await?;
        self.cached_decoding_key(kid).await?.ok_or_else(|| {
            OAuthFailure::server("Cloudflare Access JWT verification failed: unknown kid")
        })
    }

    async fn cached_decoding_key(
        &self,
        kid: &str,
    ) -> std::result::Result<Option<DecodingKey>, OAuthFailure> {
        let cache = self.jwks_cache.lock().await;
        let Some(cache) = cache.as_ref() else {
            return Ok(None);
        };
        if unix_millis().saturating_sub(cache.fetched_at_ms) >= JWKS_CACHE_TTL_MS {
            return Ok(None);
        }
        cache
            .set
            .find(kid)
            .map(DecodingKey::from_jwk)
            .transpose()
            .map_err(|error| {
                OAuthFailure::server(format!(
                    "Cloudflare Access JWT verification failed: {error}"
                ))
            })
    }

    async fn refresh_jwks(&self) -> std::result::Result<(), OAuthFailure> {
        let jwks_url = self
            .cloudflare_jwks_url
            .as_deref()
            .ok_or_else(|| OAuthFailure::server("Cloudflare Access JWKS URL is not configured"))?;
        let set = self
            .http
            .get(jwks_url)
            .send()
            .await
            .and_then(reqwest::Response::error_for_status)
            .map_err(|error| {
                OAuthFailure::server(format!(
                    "Cloudflare Access JWT verification failed: {error}"
                ))
            })?
            .json::<JwkSet>()
            .await
            .map_err(|error| {
                OAuthFailure::server(format!(
                    "Cloudflare Access JWT verification failed: {error}"
                ))
            })?;
        *self.jwks_cache.lock().await = Some(CachedJwks {
            fetched_at_ms: unix_millis(),
            set,
        });
        Ok(())
    }

    fn issuer_url(&self) -> String {
        Url::parse(&self.public_base_url)
            .map_or_else(|_| self.public_base_url.clone(), |url| url.to_string())
    }

    fn endpoint(&self, suffix: &str) -> String {
        Url::parse(&self.public_base_url)
            .and_then(|base| base.join(&format!("/{}", suffix.trim_start_matches('/'))))
            .map_or_else(
                |_| {
                    format!(
                        "{}/{}",
                        self.public_base_url.trim_end_matches('/'),
                        suffix.trim_start_matches('/')
                    )
                },
                |url| url.to_string(),
            )
    }

    async fn ensure_loaded_locked(&self, state: &mut OAuthState) -> Result<()> {
        if state.loaded {
            return Ok(());
        }
        match fs::read_to_string(&self.state_file).await {
            Ok(raw) => {
                let disk: DiskState =
                    serde_json::from_str(&raw).context("parse OAuth state file")?;
                *state = OAuthState::from_disk(disk);
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => state.loaded = true,
            Err(error) => return Err(error).context("read OAuth state file"),
        }
        if state.prune_expired(unix_millis()) {
            self.persist_locked(state).await?;
        }
        Ok(())
    }

    async fn persist_locked(&self, state: &OAuthState) -> Result<()> {
        let parent = self
            .state_file
            .parent()
            .context("OAuth state file has no parent directory")?;
        fs::create_dir_all(parent).await?;
        let temporary = self
            .state_file
            .with_extension(format!("{}.tmp", std::process::id()));
        let bytes = serde_json::to_vec_pretty(&state.snapshot())?;
        fs::write(&temporary, bytes).await?;
        replace_file_preserving_previous(&temporary, &self.state_file).await
    }
}

/// Build the OAuth authorization-server routes used by the JS MCP SDK router.
pub fn router(service: Arc<OAuthService>) -> Router {
    Router::new()
        .route(
            "/.well-known/oauth-authorization-server",
            get(authorization_server_metadata),
        )
        .route(
            "/.well-known/oauth-protected-resource",
            get(root_protected_resource_metadata),
        )
        .route(
            "/.well-known/oauth-protected-resource/mcp",
            get(legacy_protected_resource_metadata),
        )
        .route("/register", post(register_client))
        .route("/authorize", get(authorize_get).post(authorize_post))
        .route("/token", post(exchange_token))
        .route("/revoke", post(revoke_token))
        .with_state(service)
}

/// Protect POST/DELETE MCP transport calls with OAuth bearer validation when OAuth is enabled.
pub async fn mcp_bearer_guard(
    State(service): State<Option<Arc<OAuthService>>>,
    request: Request<Body>,
    next: Next,
) -> Response {
    let path = request.uri().path();
    let protected =
        matches!(*request.method(), Method::POST | Method::DELETE) && matches!(path, "/" | "/mcp");
    let Some(service) = service else {
        return next.run(request).await;
    };
    if !protected {
        return next.run(request).await;
    }

    let legacy = path == "/mcp";
    let metadata_url = service.resource_metadata_url(legacy);
    let token = match bearer_token(request.headers()) {
        Ok(token) => token,
        Err(failure) => return bearer_failure_response(&failure, &metadata_url),
    };
    match service.verify_access_token(token).await {
        Ok(info) => {
            let mut request = request;
            request.extensions_mut().insert(OAuthRequestInfo {
                client_id: info.client_id.clone(),
                scopes: info.scopes.clone(),
            });
            next.run(request).await
        }
        Err(failure) => bearer_failure_response(&failure, &metadata_url),
    }
}

async fn authorization_server_metadata(
    State(service): State<Arc<OAuthService>>,
) -> impl IntoResponse {
    Json(service.authorization_server_metadata())
}

async fn root_protected_resource_metadata(
    State(service): State<Arc<OAuthService>>,
) -> impl IntoResponse {
    Json(service.protected_resource_metadata(false))
}

async fn legacy_protected_resource_metadata(
    State(service): State<Arc<OAuthService>>,
) -> impl IntoResponse {
    Json(service.protected_resource_metadata(true))
}

async fn register_client(
    State(service): State<Arc<OAuthService>>,
    Json(metadata): Json<Value>,
) -> Response {
    match service.register_client(metadata).await {
        Ok(client) => oauth_json_response(StatusCode::CREATED, client),
        Err(failure) => oauth_failure_response(&failure),
    }
}

async fn authorize_get(
    State(service): State<Arc<OAuthService>>,
    headers: HeaderMap,
    Query(parameters): Query<HashMap<String, String>>,
) -> Response {
    authorize_from_parameters(service, headers, parameters).await
}

async fn authorize_post(
    State(service): State<Arc<OAuthService>>,
    headers: HeaderMap,
    Form(parameters): Form<HashMap<String, String>>,
) -> Response {
    authorize_from_parameters(service, headers, parameters).await
}

async fn authorize_from_parameters(
    service: Arc<OAuthService>,
    headers: HeaderMap,
    parameters: HashMap<String, String>,
) -> Response {
    let request = AuthorizationRequest {
        client_id: parameters.get("client_id").cloned().unwrap_or_default(),
        redirect_uri: parameters.get("redirect_uri").cloned(),
        response_type: parameters.get("response_type").cloned().unwrap_or_default(),
        code_challenge: parameters
            .get("code_challenge")
            .cloned()
            .unwrap_or_default(),
        code_challenge_method: parameters
            .get("code_challenge_method")
            .cloned()
            .unwrap_or_default(),
        scope: parameters.get("scope").cloned(),
        state: parameters.get("state").cloned(),
        resource: parameters.get("resource").cloned(),
    };
    let assertion = headers
        .get("cf-access-jwt-assertion")
        .and_then(|value| value.to_str().ok());
    let email = headers
        .get("cf-access-authenticated-user-email")
        .and_then(|value| value.to_str().ok());
    match service.authorize(request, assertion, email).await {
        Ok(redirect) => Response::builder()
            .status(StatusCode::FOUND)
            .header(header::LOCATION, redirect.location)
            .header(header::CACHE_CONTROL, "no-store")
            .body(Body::empty())
            .unwrap_or_else(|_| StatusCode::INTERNAL_SERVER_ERROR.into_response()),
        Err(failure) => authorization_failure_response(&parameters, &failure),
    }
}

async fn exchange_token(
    State(service): State<Arc<OAuthService>>,
    Form(parameters): Form<HashMap<String, String>>,
) -> Response {
    let request = TokenRequest {
        grant_type: parameters.get("grant_type").cloned().unwrap_or_default(),
        client_id: parameters.get("client_id").cloned().unwrap_or_default(),
        client_secret: parameters.get("client_secret").cloned(),
        code: parameters.get("code").cloned(),
        code_verifier: parameters.get("code_verifier").cloned(),
        redirect_uri: parameters.get("redirect_uri").cloned(),
        refresh_token: parameters.get("refresh_token").cloned(),
        scope: parameters.get("scope").cloned(),
        resource: parameters.get("resource").cloned(),
    };
    match service.exchange_token(request).await {
        Ok(tokens) => match serde_json::to_value(tokens) {
            Ok(value) => oauth_json_response(StatusCode::OK, value),
            Err(error) => oauth_failure_response(&OAuthFailure::server(error.to_string())),
        },
        Err(failure) => oauth_failure_response(&failure),
    }
}

async fn revoke_token(
    State(service): State<Arc<OAuthService>>,
    Form(parameters): Form<HashMap<String, String>>,
) -> Response {
    let Some(token) = parameters.get("token") else {
        return oauth_failure_response(&OAuthFailure::invalid_request("token is required"));
    };
    let client_id = parameters
        .get("client_id")
        .map(String::as_str)
        .unwrap_or_default();
    let client_secret = parameters.get("client_secret").map(String::as_str);
    match service.revoke(client_id, client_secret, token).await {
        Ok(()) => oauth_json_response(StatusCode::OK, json!({})),
        Err(failure) => oauth_failure_response(&failure),
    }
}

fn authorization_failure_response(
    parameters: &HashMap<String, String>,
    failure: &OAuthFailure,
) -> Response {
    let Some(mut target) = failure
        .safe_redirect_uri
        .as_deref()
        .and_then(|value| Url::parse(value).ok())
    else {
        return oauth_failure_response(failure);
    };
    target.query_pairs_mut().append_pair("error", failure.code);
    target
        .query_pairs_mut()
        .append_pair("error_description", &failure.message);
    if let Some(state) = parameters.get("state") {
        target.query_pairs_mut().append_pair("state", state);
    }
    Response::builder()
        .status(StatusCode::FOUND)
        .header(header::LOCATION, target.to_string())
        .header(header::CACHE_CONTROL, "no-store")
        .body(Body::empty())
        .unwrap_or_else(|_| StatusCode::INTERNAL_SERVER_ERROR.into_response())
}

fn bearer_token(headers: &HeaderMap) -> std::result::Result<&str, OAuthFailure> {
    let value = headers
        .get(header::AUTHORIZATION)
        .and_then(|value| value.to_str().ok())
        .ok_or_else(|| OAuthFailure::invalid_token("Missing Authorization header"))?;
    let mut pieces = value.splitn(2, ' ');
    let kind = pieces.next().unwrap_or_default();
    let token = pieces.next().unwrap_or_default();
    if !kind.eq_ignore_ascii_case("bearer") || token.is_empty() {
        return Err(OAuthFailure::invalid_token(
            "Invalid Authorization header format, expected 'Bearer TOKEN'",
        ));
    }
    Ok(token)
}

fn bearer_failure_response(failure: &OAuthFailure, metadata_url: &str) -> Response {
    let status = StatusCode::from_u16(failure.status).unwrap_or(StatusCode::UNAUTHORIZED);
    let authenticate = format!(
        "Bearer error=\"{}\", error_description=\"{}\", resource_metadata=\"{}\"",
        failure.code,
        escape_header_value(&failure.message),
        metadata_url
    );
    let mut response = oauth_json_response(status, failure.response());
    if let Ok(value) = authenticate.parse() {
        response
            .headers_mut()
            .insert(header::WWW_AUTHENTICATE, value);
    }
    response
}

fn oauth_failure_response(failure: &OAuthFailure) -> Response {
    let status = StatusCode::from_u16(failure.status).unwrap_or(StatusCode::BAD_REQUEST);
    oauth_json_response(status, failure.response())
}

fn oauth_json_response(status: StatusCode, value: Value) -> Response {
    let mut response = (status, Json(value)).into_response();
    response.headers_mut().insert(
        header::CACHE_CONTROL,
        header::HeaderValue::from_static("no-store"),
    );
    response
}

fn escape_header_value(value: &str) -> String {
    value.replace('\\', "\\\\").replace('"', "\\\"")
}

fn authenticate_client<'a>(
    state: &'a OAuthState,
    client_id: &str,
    client_secret: Option<&str>,
) -> std::result::Result<&'a Value, OAuthFailure> {
    let client = state
        .clients
        .get(client_id)
        .ok_or_else(|| OAuthFailure::invalid_client("Invalid client_id"))?;
    if let Some(expected) = client.get("client_secret").and_then(Value::as_str) {
        let supplied = client_secret
            .ok_or_else(|| OAuthFailure::invalid_client("Client secret is required"))?;
        if supplied != expected {
            return Err(OAuthFailure::invalid_client("Invalid client_secret"));
        }
        if client
            .get("client_secret_expires_at")
            .and_then(Value::as_u64)
            .is_some_and(|expiry| expiry < unix_seconds())
        {
            return Err(OAuthFailure::invalid_client("Client secret has expired"));
        }
    }
    Ok(client)
}

fn issue_tokens(
    state: &mut OAuthState,
    client_id: &str,
    scopes: Vec<String>,
    resource: Option<String>,
    identity: Option<Value>,
) -> TokenResponse {
    let access_token = Uuid::new_v4().to_string();
    let refresh_token = Uuid::new_v4().to_string();
    let scope = scopes.join(" ");
    let refresh_scopes = scopes.clone();
    state.access_tokens.insert(
        access_token.clone(),
        TokenRecord {
            client_id: client_id.to_owned(),
            scopes,
            resource: resource.clone(),
            identity: identity.clone(),
            expires_at: unix_millis().saturating_add(ACCESS_TOKEN_TTL_MS),
        },
    );
    state.refresh_tokens.insert(
        refresh_token.clone(),
        TokenRecord {
            client_id: client_id.to_owned(),
            scopes: refresh_scopes,
            resource,
            identity,
            expires_at: unix_millis().saturating_add(REFRESH_TOKEN_TTL_MS),
        },
    );
    TokenResponse {
        access_token,
        token_type: "bearer",
        expires_in: 3_600,
        refresh_token,
        scope,
    }
}

fn resolve_redirect_uri(
    requested: Option<&str>,
    registered: &[String],
) -> std::result::Result<String, OAuthFailure> {
    if let Some(requested) = requested {
        if registered
            .iter()
            .any(|value| redirect_uri_matches(requested, value))
        {
            return Ok(requested.to_owned());
        }
        return Err(OAuthFailure::invalid_request("Unregistered redirect_uri"));
    }
    if registered.len() == 1 {
        return Ok(registered[0].clone());
    }
    Err(OAuthFailure::invalid_request(
        "redirect_uri must be specified when client has multiple registered URIs",
    ))
}

fn redirect_uri_matches(requested: &str, registered: &str) -> bool {
    if requested == registered {
        return true;
    }
    let (Ok(requested), Ok(registered)) = (Url::parse(requested), Url::parse(registered)) else {
        return false;
    };
    let loopback =
        |host: Option<&str>| matches!(host, Some("localhost" | "127.0.0.1" | "[::1]" | "::1"));
    loopback(requested.host_str())
        && loopback(registered.host_str())
        && requested.scheme() == registered.scheme()
        && requested.host_str() == registered.host_str()
        && requested.path() == registered.path()
        && requested.query() == registered.query()
}

fn validate_client_metadata(
    object: &mut Map<String, Value>,
) -> std::result::Result<(), OAuthFailure> {
    let redirect_uris = string_array(object.get("redirect_uris")).ok_or_else(|| {
        OAuthFailure::invalid_client_metadata("redirect_uris must be an array of URLs")
    })?;
    if redirect_uris.is_empty() || redirect_uris.iter().any(|value| !is_safe_url(value)) {
        return Err(OAuthFailure::invalid_client_metadata(
            "redirect_uris must contain safe, valid URLs",
        ));
    }
    for field in [
        "token_endpoint_auth_method",
        "client_name",
        "scope",
        "policy_uri",
        "software_id",
        "software_version",
        "software_statement",
    ] {
        if object.get(field).is_some_and(|value| !value.is_string()) {
            return Err(OAuthFailure::invalid_client_metadata(format!(
                "{field} must be a string",
            )));
        }
    }
    for field in ["grant_types", "response_types", "contacts"] {
        if object
            .get(field)
            .is_some_and(|value| string_array(Some(value)).is_none())
        {
            return Err(OAuthFailure::invalid_client_metadata(format!(
                "{field} must be an array of strings",
            )));
        }
    }
    for field in ["client_uri", "logo_uri", "tos_uri", "jwks_uri"] {
        if object.get(field).is_some_and(|value| {
            value
                .as_str()
                .is_none_or(|url| !url.is_empty() && !is_safe_url(url))
        }) {
            return Err(OAuthFailure::invalid_client_metadata(format!(
                "{field} must be a safe, valid URL",
            )));
        }
        if object.get(field).and_then(Value::as_str) == Some("") {
            object.remove(field);
        }
    }
    Ok(())
}

fn is_safe_url(value: &str) -> bool {
    Url::parse(value).is_ok_and(|url| {
        !matches!(
            url.scheme().to_ascii_lowercase().as_str(),
            "javascript" | "data" | "vbscript"
        )
    })
}

fn sanitize_client_metadata(object: &mut Map<String, Value>) {
    const ALLOWED: &[&str] = &[
        "redirect_uris",
        "token_endpoint_auth_method",
        "grant_types",
        "response_types",
        "client_name",
        "client_uri",
        "logo_uri",
        "scope",
        "contacts",
        "tos_uri",
        "policy_uri",
        "jwks_uri",
        "jwks",
        "software_id",
        "software_version",
        "software_statement",
    ];
    object.retain(|key, _| ALLOWED.contains(&key.as_str()));
}

fn string_array(value: Option<&Value>) -> Option<Vec<String>> {
    value?
        .as_array()?
        .iter()
        .map(|entry| entry.as_str().map(str::to_owned))
        .collect()
}

fn split_scope(value: Option<&str>) -> Vec<String> {
    value
        .unwrap_or_default()
        .split(' ')
        .filter(|value| !value.is_empty())
        .map(str::to_owned)
        .collect()
}

fn pkce_challenge(verifier: &str) -> String {
    URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes()))
}

fn random_client_secret() -> String {
    format!("{}{}", Uuid::new_v4().simple(), Uuid::new_v4().simple())
}

fn unix_millis() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |value| {
            u64::try_from(value.as_millis()).unwrap_or(u64::MAX)
        })
}

fn unix_seconds() -> u64 {
    unix_millis() / 1_000
}

fn internal_failure(error: impl std::fmt::Display) -> OAuthFailure {
    OAuthFailure::server(error.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn oauth_client_capacity_prunes_oldest_unreferenced_clients() {
        let mut state = OAuthState {
            loaded: true,
            ..OAuthState::default()
        };
        state
            .clients
            .insert("active".to_owned(), json!({"client_id_issued_at": 1}));
        state
            .clients
            .insert("old".to_owned(), json!({"client_id_issued_at": 2}));
        state
            .clients
            .insert("new".to_owned(), json!({"client_id_issued_at": 3}));
        state.access_tokens.insert(
            "token".to_owned(),
            TokenRecord {
                client_id: "active".to_owned(),
                scopes: Vec::new(),
                resource: None,
                identity: None,
                expires_at: u64::MAX,
            },
        );
        let removed = prune_clients_for_capacity(&mut state, 2);
        assert_eq!(removed, 2);
        assert!(state.clients.contains_key("active"));
        assert_eq!(state.clients.len(), 1);
    }

    #[test]
    fn loopback_redirect_uri_relaxes_only_port() {
        assert!(redirect_uri_matches(
            "http://127.0.0.1:54321/callback?x=1",
            "http://127.0.0.1:1234/callback?x=1"
        ));
        assert!(!redirect_uri_matches(
            "http://localhost:54321/callback",
            "http://127.0.0.1:1234/callback"
        ));
        assert!(!redirect_uri_matches(
            "http://127.0.0.1:54321/other",
            "http://127.0.0.1:1234/callback"
        ));
    }

    #[test]
    fn pkce_matches_s256_encoding() {
        assert_eq!(
            pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
            "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
        );
    }
}
