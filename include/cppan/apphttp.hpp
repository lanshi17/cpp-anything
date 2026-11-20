// HTTP servers: start the original (or a build) on a port, send the recorded
// requests, and stop it. No HTTP client crate: the requests a case stores are the
// ones this module writes, so a raw socket keeps the comparison honest.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppan/appcase.hpp"
#include "cppan/util.hpp"

namespace cppan::apphttp {

using HttpRequest = appcase::HttpRequest;
using HttpExpect = appcase::HttpExpect;

/// The started server: owning the child means the destructor stops it.
class Server {
 public:
  Server() = default;
  Server(int child, std::uint16_t listen_port) : port(listen_port), child_(child) {}
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  Server(Server&& other) noexcept;
  Server& operator=(Server&& other) noexcept;
  ~Server();

  void stop();

  std::uint16_t port = 0;

 private:
  int child_ = -1;
};

struct Response {
  std::uint16_t status = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

/// A port nobody is listening on: bind to 0, read the port, let go of it.
[[nodiscard]] Result<std::uint16_t> free_port();

[[nodiscard]] Result<Server> start(const std::vector<std::string>& command,
                                   const std::filesystem::path& repo,
                                   const std::filesystem::path& cwd, std::uint16_t port,
                                   std::uint64_t timeout_secs,
                                   const std::vector<std::pair<std::string, std::string>>& extra);

[[nodiscard]] Result<Response> request(std::uint16_t port, const appcase::HttpRequest& request);

[[nodiscard]] std::string content_type(const Response& response);

[[nodiscard]] appcase::HttpExpect expectation(Response response);

/// Requests written in the docs as `curl` lines, plus the JSONL `--inputs` shape.
[[nodiscard]] std::vector<appcase::HttpRequest> requests_in(std::string_view text);

}  // namespace cppan::apphttp
