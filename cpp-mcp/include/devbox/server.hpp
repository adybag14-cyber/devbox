#pragma once
#include "async.hpp"
#include "gateway.hpp"
#include "oauth.hpp"
namespace devbox {
class McpBackend {
  public:
    virtual ~McpBackend() = default;
    virtual Json server_info() const = 0;
    virtual Json list_tools(std::string_view protocol) const = 0;
    virtual bool has_tool(std::string_view name) const {
        for (const auto& tool : list_tools("2025-11-25"))
            if (json_string(tool, "name") == name)
                return true;
        return false;
    }
    virtual asio::awaitable<Json> call_tool(std::string name, Json arguments, Cancel cancel) = 0;
    virtual asio::awaitable<Json> call_tool_authenticated(std::string name, Json arguments, Cancel cancel,
                                                          std::string principal) {
        (void)principal;
        return call_tool(std::move(name), std::move(arguments), std::move(cancel));
    }
    virtual asio::awaitable<Json> metadata(const HttpRequest& request) = 0;
    virtual Json extension_capabilities() const {
        return Json::object();
    }
    virtual bool handles_method(std::string_view) const {
        return false;
    }
    virtual asio::awaitable<Json> call_method(std::string, Json, Cancel, std::string) {
        throw Error("MCP_METHOD_UNAVAILABLE");
        co_return Json();
    }
    virtual bool ready() const = 0;
    virtual std::string tool_started(const std::string&, const Json&, const Json&) {
        return {};
    }
    virtual void tool_finished(const std::string&, const Json&) {}
    virtual void tool_failed(const std::string&, const std::string&) {}
    virtual void observe_http(const HttpRequest& request, int status, std::uint64_t bytes, Millis duration,
                              bool disconnected) = 0;
};
class HttpServer {
    struct Impl;
    std::shared_ptr<Impl> impl_;

  public:
    HttpServer(std::shared_ptr<const Config> config, std::shared_ptr<McpBackend> backend);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    std::uint16_t start();
    void stop();
    std::uint16_t port() const;
    std::size_t active_requests() const;
    Json resource_snapshot() const;
    asio::any_io_executor executor() const;
    Cancel stop_token() const;
};
} // namespace devbox
