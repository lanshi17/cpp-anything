// GET /health, GET|POST /slug and everything else (404) answered by the slug port.
#include "cppan/http.hpp"
#include "slug/port.hpp"

#include <string>

namespace {

std::string escape(const std::string& text) {
  std::string out;
  for (char ch : text) {
    if (ch == '"' || ch == '\\') out.push_back('\\');
    out.push_back(ch);
  }
  return out;
}

std::string query_value(const std::string& query, const std::string& key) {
  std::size_t at = 0;
  while (at < query.size()) {
    const std::size_t end = query.find('&', at);
    const std::string pair = query.substr(at, end == std::string::npos ? std::string::npos : end - at);
    const std::size_t equals = pair.find('=');
    if (equals != std::string::npos && pair.substr(0, equals) == key) {
      return cppan::http::decode(pair.substr(equals + 1));
    }
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return std::string();
}

bool body_text(const std::string& body, std::string& text, bool& invalid) {
  invalid = false;
  if (body.empty()) {
    invalid = true;
    return false;
  }
  try {
    const cppan::Json doc = cppan::parse(body);
    if (!doc.is_object()) return true;
    if (const cppan::Json* value = doc.find("text"); value != nullptr && value->is_string()) {
      text = value->as_string();
    }
    return true;
  } catch (const std::exception&) {
    invalid = true;
    return false;
  }
}

void send(cppan::http::Response& response, int status, const std::string& body) {
  response.status = status;
  response.content_type = "application/json";
  response.body = body;
}

}  // namespace

bool cppan_http_slug(const cppan::http::Request& request, cppan::http::Response& response) {
  if (request.method == "GET" && request.path == "/health") {
    send(response, 200, "{\"ok\": true}");
    return true;
  }
  if (request.path == "/slug") {
    std::string text;
    if (request.method == "GET") {
      text = query_value(request.query, "text");
    } else if (request.method == "POST") {
      bool invalid = false;
      body_text(request.body, text, invalid);
      if (invalid) {
        send(response, 400, "{\"error\": \"invalid json\"}");
        return true;
      }
    } else {
      return false;
    }
    const auto result = slug::slug(text);
    if (!result) {
      send(response, 500, "{\"error\": \"port error\"}");
      return true;
    }
    send(response, 200, "{\"slug\": \"" + escape(*result) + "\"}");
    return true;
  }
  // The catch-all route: the recorded server answers 404 for every other path.
  send(response, 404, "{\"error\": \"not found\"}");
  return true;
}
