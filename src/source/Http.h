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

// A blocking GET, so a source can be tested with canned responses. The real one is httpclient's: WinHTTP on Windows,
// libcurl elsewhere.
class Http {
 public:
  virtual ~Http() = default;
  virtual HttpResult get(const std::string & url, const HttpHeaders & headers = {}) = 0;
  // A blocking POST of a form (`body` is already form-encoded; the content type is application/x-www-form-urlencoded). The answer's body
  // is whatever bytes came, a zip included. An Http that does not post says so in the error, so the sources that only read still use fakes
  // that only get.
  virtual HttpResult post(const std::string & /*url*/, const std::string & /*body*/, const HttpHeaders & /*headers*/ = {}) {
    HttpResult r;
    r.error = "this Http does not post";
    return r;
  }
  // For a thread that is to stop: the request in progress ends soon (httpclient asks about once a second) and every later
  // one at once, with no answer (status 0). Any thread may call it.
  void cancel() { cancelled_ = true; }
  bool cancelled() const { return cancelled_; }

 private:
  std::atomic<bool> cancelled_{false};
};

std::unique_ptr<Http> makeHttp();
