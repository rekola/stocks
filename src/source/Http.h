#pragma once

#include <memory>
#include <string>

struct HttpResult {
  int status = 0;  // 0 when nothing came back
  std::string body, error;
  bool ok() const { return status >= 200 && status < 300; }
};

// A blocking GET, so a source can be tested with canned responses. The real one is httpclient's: WinHTTP on Windows,
// libcurl elsewhere.
class Http {
 public:
  virtual ~Http() = default;
  virtual HttpResult get(const std::string & url) = 0;
};

std::unique_ptr<Http> makeHttp();
