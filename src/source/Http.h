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

// How a browser asks for a URL: a page's script with fetch(), or the page itself (typed in, or followed from a link).
enum class BrowserRequest { Fetch, Page };

// The headers of a current Chrome on Windows asking for `url` (user agent, client hints, language, the Sec-Fetch-* of `kind`), for the
// adapters whose providers turn scripts away; each adapter names this itself, no other source is disguised. Sec-Fetch-Site follows
// from the page that asks, `own`'s Referer, and `url`: without a Referer a fetch is the URL's own page's and a page is typed in. A
// cross-origin fetch carries that page's Origin. `own` is added to them: a header of the same name (any case) replaces the
// browser's, and one with an empty value is not sent.
HttpHeaders browserHeaders(const std::string & url, const HttpHeaders & own = {}, BrowserRequest kind = BrowserRequest::Fetch);

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
