#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct HttpResult {
  int status = 0;  // 0 when nothing came back
  std::string body, error;
  bool ok() const { return status >= 200 && status < 300; }
};

// Headers to send, as name and value.
using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

// The headers of a current Chrome on Windows making a fetch() call (user agent, client hints, language), for the adapters whose
// providers turn scripts away; each adapter names this itself, no other source is disguised. `own` is added to them, and a header
// of the same name (any case) in `own` replaces the browser's.
HttpHeaders browserHeaders(const HttpHeaders & own = {});

// A blocking GET, so a source can be tested with canned responses. The real one is httpclient's: WinHTTP on Windows,
// libcurl elsewhere.
class Http {
 public:
  virtual ~Http() = default;
  virtual HttpResult get(const std::string & url, const HttpHeaders & headers = {}) = 0;
  // For a thread that is to stop: the request in progress ends soon (httpclient asks about once a second) and every later
  // one at once, with no answer (status 0). Any thread may call it.
  void cancel() { cancelled_ = true; }
  bool cancelled() const { return cancelled_; }

 private:
  std::atomic<bool> cancelled_{false};
};

std::unique_ptr<Http> makeHttp();
