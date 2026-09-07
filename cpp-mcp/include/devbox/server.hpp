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
    virtual asio::awaitable<Json> call_tool(std::string name, Json arguments, Cancel cancel) = 0;
    virtual asio::awaitable<Json> metadata(const HttpRequest& request) = 0;
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
    asio::any_io_executor executor() const;
    Cancel stop_token() const;
};
} // namespace devbox
