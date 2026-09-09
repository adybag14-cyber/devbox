#include "devbox/oauth.hpp"
#include <boost/asio.hpp>
#include <future>
#include <iostream>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <thread>

using namespace devbox;
namespace asio = boost::asio;
using Tcp = asio::ip::tcp;
void require(bool value, const char* message) {
    if (!value)
        throw Error(message);
}
template <class F> OAuthFailure failure(F&& fn, std::string_view code) {
    try {
        fn();
    } catch (const OAuthFailure& e) {
        require(e.code == code, ("Unexpected OAuth failure: " + e.code + ": " + e.what()).c_str());
        return e;
    }
    throw Error("Expected OAuth failure " + std::string(code));
}
struct HttpFixture {
    asio::io_context io;
    Tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    std::atomic_bool stop{false}, received{false}, stalled{false};
    std::atomic_size_t calls{0};
    std::string body;
    std::thread worker;
    explicit HttpFixture(std::string data) : body(std::move(data)) {
        acceptor.non_blocking(true);
        worker = std::thread([this] {
            while (!stop) {
                boost::system::error_code ec;
                Tcp::socket socket(io);
                acceptor.accept(socket, ec);
                if (ec) {
                    std::this_thread::sleep_for(Millis(2));
                    continue;
                }
                asio::streambuf buffer(16384);
                asio::read_until(socket, buffer, "\r\n\r\n", ec);
                if (ec)
                    continue;
                received = true;
                ++calls;
                while (stalled && !stop)
                    std::this_thread::sleep_for(Millis(2));
                if (stop)
                    break;
                const auto response =
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                asio::write(socket, asio::buffer(response), ec);
            }
        });
    }
    ~HttpFixture() {
        stop = true;
        if (worker.joinable())
            worker.join();
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/keys";
    }
};
struct RsaKey {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr, EVP_PKEY_free};
    RsaKey() {
        key.reset(EVP_RSA_gen(2048));
        require(static_cast<bool>(key), "RSA key generation");
    }
    std::string component(const char* field) const {
        BIGNUM* raw = nullptr;
        require(EVP_PKEY_get_bn_param(key.get(), field, &raw) == 1, "RSA component");
        std::unique_ptr<BIGNUM, decltype(&BN_free)> value(raw, BN_free);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(BN_num_bytes(value.get())));
        BN_bn2bin(value.get(), bytes.data());
        return base64_encode(bytes, true);
    }
    Json jwk() const {
        return Json{{"kty", "RSA"},
                    {"alg", "RS256"},
                    {"use", "sig"},
                    {"kid", "fixture"},
                    {"n", component(OSSL_PKEY_PARAM_RSA_N)},
                    {"e", component(OSSL_PKEY_PARAM_RSA_E)}};
    }
    std::string sign(const Json& claims, std::string kid = "fixture") const {
        const auto data = base64_encode(Json{{"alg", "RS256"}, {"kid", kid}}.dump(), true) + "." +
                          base64_encode(claims.dump(), true);
        const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),
                                                                              EVP_MD_CTX_free);
        require(EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1,
                "sign init");
        std::size_t size = 0;
        require(EVP_DigestSign(context.get(), nullptr, &size,
                               reinterpret_cast<const unsigned char*>(data.data()), data.size()) == 1,
                "sign size");
        std::vector<std::uint8_t> signature(size);
        require(EVP_DigestSign(context.get(), signature.data(), &size,
                               reinterpret_cast<const unsigned char*>(data.data()), data.size()) == 1,
                "sign");
        signature.resize(size);
        return data + "." + base64_encode(signature, true);
    }
};
Json public_metadata() {
    return Json{{"redirect_uris", {"http://127.0.0.1:54321/callback?x=1"}},
                {"token_endpoint_auth_method", "none"}};
}
const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
Json authorization(const Json& client) {
    return Json{{"client_id", client["client_id"]},
                {"response_type", "code"},
                {"code_challenge", pkce_challenge(verifier)},
                {"code_challenge_method", "S256"},
                {"scope", "mcp:devbox:read"},
                {"state", "space & + ~"}};
}
Json exchange_request(const Json& client, const std::string& redirect) {
    return Json{{"client_id", client["client_id"]},
                {"grant_type", "authorization_code"},
                {"code", query_parameters(Url::parse(redirect).query)["code"]},
                {"code_verifier", verifier}};
}
int main() {
    const auto root = fs::temp_directory_path() / ("devbox-cpp-oauth-" + uuid());
    try {
        fs::create_directories(root);
        Config config;
        config.project_root = root;
        config.auth_mode = AuthMode::demo_oauth;
        config.public_base_url = "https://example.test";
        config.oauth_state_file_path = root / "oauth-state.json";
        config.platform = Platform::detect();
        require(pkce_challenge(verifier) == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", "PKCE RFC vector");
        require(Url::parse("https://bücher.example:443/a/../b?#").str() ==
                    "https://xn--bcher-kva.example/b?#",
                "WHATWG URL semantics");
        require(Url::parse("http://user:password@[::1]:0/?").str() == "http://user:password@[::1]:0/?",
                "URL credentials port zero and query");
        require(!redirect_uri_matches("http://127.0.0.1:1/callback?", "http://127.0.0.1:2/callback"),
                "empty query differs from absent query");
        require(redirect_uri_matches("http://[::1]:54321/callback", "http://[::1]:1234/callback"),
                "loopback port relaxation");
        require(!redirect_uri_matches("http://localhost:54321/callback", "http://127.0.0.1:1234/callback"),
                "loopback hostname remains bound");
        OAuthService service(config);
        require(service.authorization_server_metadata()["issuer"] == "https://example.test/",
                "normalized OAuth issuer");
        failure([&] { service.register_client(Json::array()); }, "invalid_client_metadata");
        failure([&] { service.register_client(Json{{"redirect_uris", {"javascript:alert(1)"}}}); },
                "invalid_client_metadata");
        auto metadata = public_metadata();
        metadata["client_id"] = "injected";
        metadata["client_secret"] = "injected";
        metadata["logo_uri"] = "";
        const auto client = service.register_client(metadata);
        require(client["client_id"] != "injected" && !client.contains("client_secret") &&
                    !client.contains("logo_uri"),
                "client metadata sanitization");
        auto auth = authorization(client);
        auth["redirect_uri"] = "https://attacker.invalid";
        const auto unsafe = failure([&] { service.authorize(auth); }, "invalid_request");
        require(!unsafe.safe_redirect_uri, "untrusted redirect never used for errors");
        auth = authorization(client);
        auth["response_type"] = "token";
        const auto safe = failure([&] { service.authorize(auth); }, "invalid_request");
        require(safe.safe_redirect_uri == "http://127.0.0.1:54321/callback?x=1",
                "validated redirect carries OAuth error");
        auth = authorization(client);
        const auto redirect = service.authorize(auth);
        require(query_parameters(Url::parse(redirect).query)["state"] == auth["state"],
                "authorization state roundtrip");
        auto exchange = exchange_request(client, redirect);
        auto wrong = exchange;
        wrong["code_verifier"] = "wrong";
        failure([&] { service.exchange_token(wrong); }, "invalid_grant");
        const auto tokens = service.exchange_token(exchange);
        failure([&] { service.exchange_token(exchange); }, "invalid_grant");
        const auto access = json_string(tokens, "access_token");
        require(service.verify_access_token(access)["scopes"] == Json::array({"mcp:devbox:read"}),
                "narrow scopes preserved");
        OAuthService restarted(config);
        require(restarted.verify_access_token(access)["clientId"] == client["client_id"],
                "Rust-shaped OAuth journal survives restart");
        const auto disk = read_json(config.oauth_state_file_path);
        require(disk["clients"].is_array() && disk["accessTokens"][0].size() == 2 &&
                    disk["refreshTokens"][0][1].contains("expiresAt"),
                "persistent tuple contract");
        Json refresh{{"client_id", client["client_id"]},
                     {"grant_type", "refresh_token"},
                     {"refresh_token", tokens["refresh_token"]}};
        const auto renewed = restarted.exchange_token(refresh);
        failure([&] { restarted.exchange_token(refresh); }, "invalid_grant");
        restarted.revoke(Json{{"client_id", client["client_id"]}, {"token", renewed["access_token"]}});
        failure([&] { restarted.verify_access_token(json_string(renewed, "access_token")); },
                "invalid_token");
        require(restarted.verify_access_token(access)["clientId"] == client["client_id"],
                "refresh preserves existing access tokens");
        auto secret_metadata = public_metadata();
        secret_metadata.erase("token_endpoint_auth_method");
        const auto secret_client = restarted.register_client(secret_metadata);
        auto secret_exchange =
            exchange_request(secret_client, restarted.authorize(authorization(secret_client)));
        failure([&] { restarted.exchange_token(secret_exchange); }, "invalid_client");
        secret_exchange["client_secret"] = secret_client["client_secret"];
        require(restarted.exchange_token(secret_exchange).contains("access_token"),
                "confidential client authenticated");
        require(oauth_scope_allows({"mcp:tools"}, "mcp:admin") &&
                    !oauth_scope_allows({"mcp:devbox:read"}, "mcp:devbox:exec"),
                "scope enforcement");
        const auto contract = read_json(path_from_utf8(DEVBOX_CONTRACT_FILE));
        for (const auto& tool : contract["profiles"]["host"])
            require(required_tool_scope(json_string(tool, "name")).has_value(),
                    "every schema has an OAuth scope");

        RsaKey key;
        HttpFixture jwks(Json{{"keys", Json::array({key.jwk()})}}.dump());
        auto cfconfig = config;
        cfconfig.auth_mode = AuthMode::cloudflare_access;
        cfconfig.cloudflare_access_team_domain = "https://fixture.cloudflareaccess.com";
        cfconfig.cloudflare_access_aud = "test-audience";
        cfconfig.cloudflare_access_jwks_url = jwks.url();
        OAuthService cloudflare(cfconfig);
        Json claims{{"exp", unix_millis() / 1000 + 300},
                    {"iss", *cfconfig.cloudflare_access_team_domain},
                    {"aud", {"test-audience"}},
                    {"sub", "fixture-subject"},
                    {"email", "fixture@example.test"}};
        const auto cf_redirect = cloudflare.authorize(authorization(client), key.sign(claims));
        const auto cf_tokens = cloudflare.exchange_token(exchange_request(client, cf_redirect));
        require(cloudflare.verify_access_token(json_string(cf_tokens, "access_token"))["identity"]["email"] ==
                    "fixture@example.test",
                "verified RSA identity");
        auto bad_claims = claims;
        bad_claims["aud"] = "wrong";
        failure([&] { cloudflare.authorize(authorization(client), key.sign(bad_claims)); }, "server_error");
        bad_claims = claims;
        bad_claims["iss"] = "https://attacker.invalid";
        failure([&] { cloudflare.authorize(authorization(client), key.sign(bad_claims)); }, "server_error");
        bad_claims = claims;
        bad_claims["exp"] = unix_millis() / 1000 - 90;
        failure([&] { cloudflare.authorize(authorization(client), key.sign(bad_claims)); }, "server_error");
        auto forged = key.sign(claims);
        const auto signature_start = forged.rfind('.') + 1;
        forged[signature_start] = forged[signature_start] == 'A' ? 'B' : 'A';
        failure([&] { cloudflare.authorize(authorization(client), forged); }, "server_error");
        failure([&] { cloudflare.authorize(authorization(client), key.sign(claims, "unknown")); },
                "server_error");
        require(jwks.calls == 1, "JWKS cache and unknown-key refresh cooldown");
        failure([&] { cloudflare.authorize(authorization(client)); }, "invalid_request");

        HttpFixture blocked(Json{{"keys", Json::array({key.jwk()})}}.dump());
        blocked.stalled = true;
        cfconfig.cloudflare_access_jwks_url = blocked.url();
        OAuthService stalled(cfconfig);
        const auto cancel = std::make_shared<Cancellation>();
        auto pending = std::async(std::launch::async, [&] {
            try {
                stalled.authorize(authorization(client), key.sign(claims), {}, cancel);
            } catch (const Cancelled&) {
                return true;
            }
            return false;
        });
        const auto deadline = Clock::now() + Millis(3000);
        while (!blocked.received && Clock::now() < deadline)
            std::this_thread::sleep_for(Millis(2));
        require(blocked.received, "JWKS request reached fixture");
        const auto started = Clock::now();
        require(stalled.verify_access_token(access)["clientId"] == client["client_id"],
                "existing token during stalled JWKS");
        require(stalled.register_client(public_metadata()).contains("client_id"),
                "registration during stalled JWKS");
        require(Clock::now() - started < Millis(500), "JWKS never holds state lock");
        cancel->cancel();
        require(pending.wait_for(Millis(2000)) == std::future_status::ready && pending.get(),
                "JWKS bounded cancellation");
        blocked.stalled = false;
        HttpFixture oversized(std::string(256 * 1024 + 1, 'x'));
        cfconfig.cloudflare_access_jwks_url = oversized.url();
        OAuthService large(cfconfig);
        const auto oversize =
            failure([&] { large.authorize(authorization(client), key.sign(claims)); }, "server_error");
        require(std::string(oversize.what()).find("limit") != std::string::npos, "JWKS byte bound");
        config.oauth_state_file_path = root / "capacity.json";
        config.oauth_max_clients = 1;
        OAuthService capacity(config);
        const auto occupied = capacity.register_client(public_metadata());
        capacity.authorize(authorization(occupied));
        failure([&] { capacity.register_client(public_metadata()); }, "invalid_client_metadata");
        config.oauth_state_file_path = root / "bad.json";
        write_file(config.oauth_state_file_path, "{broken");
        OAuthService corrupted(config);
        failure([&] { corrupted.register_client(public_metadata()); }, "server_error");
        fs::remove_all(root);
        std::cout
            << "OAuth redirect, persistence, PKCE, scopes, RSA, bounded JWKS and concurrency checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\nFixture retained at " << root << '\n';
        return 1;
    }
}
