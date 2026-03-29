import { randomUUID } from "node:crypto";
import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import path from "node:path";
import { createRemoteJWKSet, jwtVerify } from "jose";
import { InvalidRequestError, ServerError } from "@modelcontextprotocol/sdk/server/auth/errors.js";

const ACCESS_TOKEN_TTL_MS = 60 * 60 * 1000;
const REFRESH_TOKEN_TTL_MS = 7 * 24 * 60 * 60 * 1000;
const AUTHORIZATION_CODE_TTL_MS = 10 * 60 * 1000;

const normalizeUrl = (value) => {
  const rawValue = String(value ?? "").trim();
  if (!rawValue) {
    return "";
  }

  if (/^https?:\/\//i.test(rawValue)) {
    return rawValue.replace(/\/+$/, "");
  }

  return `https://${rawValue.replace(/\/+$/, "")}`;
};

const firstHeaderValue = (value) => {
  if (Array.isArray(value)) {
    return value.find((entry) => typeof entry === "string" && entry.trim()) ?? "";
  }

  return typeof value === "string" ? value : "";
};

const deserializeResource = (value) => {
  if (!value) {
    return undefined;
  }

  return new URL(value);
};

const serializeResource = (value) => {
  if (!value) {
    return null;
  }

  if (value instanceof URL) {
    return value.href;
  }

  return String(value);
};

const cloneIdentity = (identity) => {
  if (!identity || typeof identity !== "object") {
    return null;
  }

  return {
    ...identity,
  };
};

const summarizeAuthorizeRequest = (req) => ({
  method: req?.method ?? null,
  url: req?.originalUrl ?? req?.url ?? null,
  host: firstHeaderValue(req?.headers?.host) || null,
  hasCfAccessJwtAssertion: Boolean(firstHeaderValue(req?.headers?.["cf-access-jwt-assertion"])),
  cfAccessEmail: firstHeaderValue(req?.headers?.["cf-access-authenticated-user-email"]) || null,
  userAgent: firstHeaderValue(req?.headers?.["user-agent"]) || null,
});

const toOAuthServerError = (error, fallbackMessage) => {
  if (error instanceof InvalidRequestError || error instanceof ServerError) {
    return error;
  }

  if (error instanceof Error && error.message) {
    return new ServerError(error.message);
  }

  return new ServerError(fallbackMessage);
};

class PersistentOAuthState {
  constructor(filePath) {
    this.filePath = filePath;
    this.loaded = false;
    this.clients = new Map();
    this.authorizationCodes = new Map();
    this.accessTokens = new Map();
    this.refreshTokens = new Map();
    this.writePromise = Promise.resolve();
  }

  async ensureLoaded() {
    if (this.loaded) {
      return;
    }

    if (!this.filePath) {
      this.loaded = true;
      return;
    }

    try {
      const raw = await readFile(this.filePath, "utf8");
      const parsed = JSON.parse(raw);

      for (const [clientId, clientInfo] of parsed.clients ?? []) {
        this.clients.set(clientId, clientInfo);
      }

      for (const [code, record] of parsed.authorizationCodes ?? []) {
        const params = record.params ?? {};
        const clientId =
          typeof record.clientId === "string" && record.clientId.trim()
            ? record.clientId
            : typeof record.client?.client_id === "string"
              ? record.client.client_id
              : "";
        this.authorizationCodes.set(code, {
          clientId,
          expiresAt: Number.isFinite(record.expiresAt) ? record.expiresAt : Date.now() + AUTHORIZATION_CODE_TTL_MS,
          identity: cloneIdentity(record.identity),
          params: {
            ...params,
            scopes: Array.isArray(params.scopes) ? params.scopes : [],
            resource: deserializeResource(params.resource),
          },
        });
      }

      for (const [token, record] of parsed.accessTokens ?? []) {
        this.accessTokens.set(token, {
          clientId: record.clientId,
          scopes: Array.isArray(record.scopes) ? record.scopes : [],
          resource: deserializeResource(record.resource),
          identity: cloneIdentity(record.identity),
          expiresAt: record.expiresAt,
        });
      }

      for (const [token, record] of parsed.refreshTokens ?? []) {
        this.refreshTokens.set(token, {
          clientId: record.clientId,
          scopes: Array.isArray(record.scopes) ? record.scopes : [],
          resource: deserializeResource(record.resource),
          identity: cloneIdentity(record.identity),
          expiresAt: record.expiresAt,
        });
      }
    } catch (error) {
      if (error?.code !== "ENOENT") {
        throw error;
      }
    }

    this.loaded = true;
  }

  snapshot() {
    return {
      clients: [...this.clients.entries()],
      authorizationCodes: [...this.authorizationCodes.entries()].map(([code, record]) => [
        code,
        {
          clientId: record.clientId,
          expiresAt: record.expiresAt,
          identity: cloneIdentity(record.identity),
          params: {
            ...record.params,
            scopes: Array.isArray(record.params?.scopes) ? record.params.scopes : [],
            resource: serializeResource(record.params?.resource),
          },
        },
      ]),
      accessTokens: [...this.accessTokens.entries()].map(([token, record]) => [
        token,
        {
          clientId: record.clientId,
          scopes: Array.isArray(record.scopes) ? record.scopes : [],
          resource: serializeResource(record.resource),
          identity: cloneIdentity(record.identity),
          expiresAt: record.expiresAt,
        },
      ]),
      refreshTokens: [...this.refreshTokens.entries()].map(([token, record]) => [
        token,
        {
          clientId: record.clientId,
          scopes: Array.isArray(record.scopes) ? record.scopes : [],
          resource: serializeResource(record.resource),
          identity: cloneIdentity(record.identity),
          expiresAt: record.expiresAt,
        },
      ]),
    };
  }

  async persist() {
    await this.ensureLoaded();
    if (!this.filePath) {
      return;
    }

    const snapshot = this.snapshot();
    this.writePromise = this.writePromise.then(async () => {
      await mkdir(path.dirname(this.filePath), { recursive: true });
      const tempFilePath = `${this.filePath}.${process.pid}.tmp`;
      await writeFile(tempFilePath, JSON.stringify(snapshot, null, 2), "utf8");
      await rename(tempFilePath, this.filePath);
    });

    await this.writePromise;
  }
}

export class DemoClientStore {
  constructor(state) {
    this.state = state;
  }

  async getClient(clientId) {
    await this.state.ensureLoaded();
    return this.state.clients.get(clientId);
  }

  async registerClient(clientMetadata) {
    await this.state.ensureLoaded();
    this.state.clients.set(clientMetadata.client_id, clientMetadata);
    await this.state.persist();
    return clientMetadata;
  }
}

export class DemoOAuthProvider {
  constructor({ stateFilePath } = {}) {
    this.state = new PersistentOAuthState(stateFilePath);
    this.clientsStore = new DemoClientStore(this.state);
  }

  async authorize(client, params, res) {
    if (!Array.isArray(client.redirect_uris) || !client.redirect_uris.includes(params.redirectUri)) {
      throw new InvalidRequestError("Unregistered redirect_uri.");
    }

    const code = randomUUID();
    await this.state.ensureLoaded();
    this.state.authorizationCodes.set(code, {
      clientId: client.client_id,
      params,
      expiresAt: Date.now() + AUTHORIZATION_CODE_TTL_MS,
    });
    await this.state.persist();

    const target = new URL(params.redirectUri);
    target.searchParams.set("code", code);
    if (params.state) {
      target.searchParams.set("state", params.state);
    }

    res.redirect(target.toString());
  }

  async challengeForAuthorizationCode(_client, authorizationCode) {
    const codeRecord = await this.getAuthorizationCodeRecord(authorizationCode);
    return codeRecord.params.codeChallenge;
  }

  async exchangeAuthorizationCode(client, authorizationCode) {
    const codeRecord = await this.getAuthorizationCodeRecord(authorizationCode);
    if (codeRecord.clientId !== client.client_id) {
      throw new Error("Authorization code was not issued to this client.");
    }

    this.state.authorizationCodes.delete(authorizationCode);
    await this.state.persist();
    return this.issueTokens(client.client_id, codeRecord.params.scopes ?? [], codeRecord.params.resource);
  }

  async exchangeRefreshToken(client, refreshToken, scopes, resource) {
    await this.state.ensureLoaded();
    const refreshRecord = this.state.refreshTokens.get(refreshToken);
    if (!refreshRecord) {
      throw new Error("Invalid refresh token.");
    }

    if (refreshRecord.clientId !== client.client_id) {
      throw new Error("Refresh token was not issued to this client.");
    }

    if (refreshRecord.expiresAt < Date.now()) {
      this.state.refreshTokens.delete(refreshToken);
      await this.state.persist();
      throw new Error("Refresh token expired.");
    }

    const nextScopes = scopes?.length ? scopes : refreshRecord.scopes;
    const nextResource = resource ?? refreshRecord.resource;
    return this.issueTokens(client.client_id, nextScopes, nextResource);
  }

  async verifyAccessToken(token) {
    await this.state.ensureLoaded();
    const tokenRecord = this.state.accessTokens.get(token);
    if (!tokenRecord) {
      throw new Error("Unknown access token.");
    }

    if (tokenRecord.expiresAt < Date.now()) {
      this.state.accessTokens.delete(token);
      await this.state.persist();
      throw new Error("Access token expired.");
    }

    return {
      token,
      clientId: tokenRecord.clientId,
      scopes: tokenRecord.scopes,
      expiresAt: Math.floor(tokenRecord.expiresAt / 1000),
      resource: tokenRecord.resource,
    };
  }

  async revokeToken(token) {
    await this.state.ensureLoaded();
    this.state.accessTokens.delete(token);
    this.state.refreshTokens.delete(token);
    await this.state.persist();
  }

  async getAuthorizationCodeRecord(authorizationCode) {
    await this.state.ensureLoaded();
    const codeRecord = this.state.authorizationCodes.get(authorizationCode);
    if (!codeRecord) {
      throw new Error("Invalid authorization code.");
    }

    if (codeRecord.expiresAt < Date.now()) {
      this.state.authorizationCodes.delete(authorizationCode);
      await this.state.persist();
      throw new Error("Authorization code expired.");
    }

    return codeRecord;
  }

  issueTokens(clientId, scopes, resource) {
    const accessToken = randomUUID();
    const refreshToken = randomUUID();
    const accessExpiresAt = Date.now() + ACCESS_TOKEN_TTL_MS;
    const refreshExpiresAt = Date.now() + REFRESH_TOKEN_TTL_MS;

    this.state.accessTokens.set(accessToken, {
      clientId,
      scopes,
      resource,
      expiresAt: accessExpiresAt,
    });

    this.state.refreshTokens.set(refreshToken, {
      clientId,
      scopes,
      resource,
      expiresAt: refreshExpiresAt,
    });

    this.state.persist().catch(() => {});

    return {
      access_token: accessToken,
      token_type: "bearer",
      expires_in: 3600,
      refresh_token: refreshToken,
      scope: scopes.join(" "),
    };
  }
}

export class CloudflareAccessOAuthProvider {
  constructor({ teamDomain, audience, jwksUrl, stateFilePath }) {
    const issuer = normalizeUrl(teamDomain);
    if (!issuer) {
      throw new Error("Cloudflare Access team domain is required.");
    }

    if (!audience) {
      throw new Error("Cloudflare Access application AUD is required.");
    }

    this.state = new PersistentOAuthState(stateFilePath);
    this.clientsStore = new DemoClientStore(this.state);
    this.issuer = issuer;
    this.audience = audience;
    this.jwksUrl = new URL(normalizeUrl(jwksUrl) || `${issuer}/cdn-cgi/access/certs`);
    this.jwks = createRemoteJWKSet(this.jwksUrl);
  }

  async authorize(client, params, res) {
    try {
      if (!Array.isArray(client.redirect_uris) || !client.redirect_uris.includes(params.redirectUri)) {
        throw new InvalidRequestError("Unregistered redirect_uri.");
      }

      const identity = await this.getAuthenticatedIdentityFromRequest(res.req);
      const code = randomUUID();
      await this.state.ensureLoaded();
      this.state.authorizationCodes.set(code, {
        clientId: client.client_id,
        params,
        identity,
        expiresAt: Date.now() + AUTHORIZATION_CODE_TTL_MS,
      });
      await this.state.persist();

      const target = new URL(params.redirectUri);
      target.searchParams.set("code", code);
      if (params.state) {
        target.searchParams.set("state", params.state);
      }

      res.redirect(target.toString());
    } catch (error) {
      console.error("Cloudflare Access OAuth authorize failed.", {
        request: summarizeAuthorizeRequest(res.req),
        clientId: client?.client_id ?? null,
        redirectUri: params?.redirectUri ?? null,
        error: error instanceof Error ? error.message : String(error),
      });
      throw toOAuthServerError(error, "Cloudflare Access authorization failed.");
    }
  }

  async challengeForAuthorizationCode(client, authorizationCode) {
    const codeRecord = await this.getAuthorizationCodeRecord(authorizationCode);
    if (codeRecord.clientId !== client.client_id) {
      throw new Error("Authorization code was not issued to this client.");
    }

    return codeRecord.params.codeChallenge;
  }

  async exchangeAuthorizationCode(client, authorizationCode) {
    const codeRecord = await this.getAuthorizationCodeRecord(authorizationCode);
    if (codeRecord.clientId !== client.client_id) {
      throw new Error("Authorization code was not issued to this client.");
    }

    this.state.authorizationCodes.delete(authorizationCode);
    await this.state.persist();
    return this.issueTokens(client.client_id, codeRecord.params.scopes ?? [], codeRecord.params.resource, codeRecord.identity);
  }

  async exchangeRefreshToken(client, refreshToken, scopes, resource) {
    await this.state.ensureLoaded();
    const refreshRecord = this.state.refreshTokens.get(refreshToken);
    if (!refreshRecord) {
      throw new Error("Invalid refresh token.");
    }

    if (refreshRecord.clientId !== client.client_id) {
      throw new Error("Refresh token was not issued to this client.");
    }

    if (refreshRecord.expiresAt < Date.now()) {
      this.state.refreshTokens.delete(refreshToken);
      await this.state.persist();
      throw new Error("Refresh token expired.");
    }

    const nextScopes = scopes?.length ? scopes : refreshRecord.scopes;
    const nextResource = resource ?? refreshRecord.resource;
    return this.issueTokens(client.client_id, nextScopes, nextResource, refreshRecord.identity);
  }

  async verifyAccessToken(token) {
    await this.state.ensureLoaded();
    const tokenRecord = this.state.accessTokens.get(token);
    if (!tokenRecord) {
      throw new Error("Unknown access token.");
    }

    if (tokenRecord.expiresAt < Date.now()) {
      this.state.accessTokens.delete(token);
      await this.state.persist();
      throw new Error("Access token expired.");
    }

    return {
      token,
      clientId: tokenRecord.clientId,
      scopes: tokenRecord.scopes,
      expiresAt: Math.floor(tokenRecord.expiresAt / 1000),
      resource: tokenRecord.resource,
      extra: {
        identity: tokenRecord.identity,
      },
    };
  }

  async revokeToken(token) {
    await this.state.ensureLoaded();
    this.state.accessTokens.delete(token);
    this.state.refreshTokens.delete(token);
    await this.state.persist();
  }

  async getAuthorizationCodeRecord(authorizationCode) {
    await this.state.ensureLoaded();
    const codeRecord = this.state.authorizationCodes.get(authorizationCode);
    if (!codeRecord) {
      throw new Error("Invalid authorization code.");
    }

    if (codeRecord.expiresAt < Date.now()) {
      this.state.authorizationCodes.delete(authorizationCode);
      this.state.persist().catch(() => {});
      throw new Error("Authorization code expired.");
    }

    return codeRecord;
  }

  async getAuthenticatedIdentityFromRequest(req) {
    const accessJwt = firstHeaderValue(req?.headers?.["cf-access-jwt-assertion"]);
    if (!accessJwt) {
      throw new InvalidRequestError(
        "Cloudflare Access authentication is required on /authorize. Protect that path with a Cloudflare Access application.",
      );
    }

    let payload;
    try {
      ({ payload } = await jwtVerify(accessJwt, this.jwks, {
        issuer: this.issuer,
        audience: this.audience,
      }));
    } catch (error) {
      throw new ServerError(`Cloudflare Access JWT verification failed: ${error instanceof Error ? error.message : String(error)}`);
    }

    return {
      sub: typeof payload.sub === "string" ? payload.sub : "",
      email: typeof payload.email === "string" ? payload.email : firstHeaderValue(req?.headers?.["cf-access-authenticated-user-email"]) || null,
      name: typeof payload.name === "string" ? payload.name : null,
      aud: payload.aud,
      iss: payload.iss,
    };
  }

  issueTokens(clientId, scopes, resource, identity) {
    const accessToken = randomUUID();
    const refreshToken = randomUUID();
    const accessExpiresAt = Date.now() + ACCESS_TOKEN_TTL_MS;
    const refreshExpiresAt = Date.now() + REFRESH_TOKEN_TTL_MS;

    this.state.accessTokens.set(accessToken, {
      clientId,
      scopes,
      resource,
      identity,
      expiresAt: accessExpiresAt,
    });

    this.state.refreshTokens.set(refreshToken, {
      clientId,
      scopes,
      resource,
      identity,
      expiresAt: refreshExpiresAt,
    });

    this.state.persist().catch(() => {});

    return {
      access_token: accessToken,
      token_type: "bearer",
      expires_in: 3600,
      refresh_token: refreshToken,
      scope: scopes.join(" "),
    };
  }
}
