#include "source/Http.h"

#include <HTTPClient.h>
#ifdef _WIN32
#include <WinHTTPClient.h>
#else
#include <CurlClient.h>
#endif

#include <algorithm>
#include <cctype>

namespace {

// One browser, kept in step: the user agent, the client hints and the Chrome major version agree.
constexpr const char * kBrowserUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

bool sameName(const std::string & a, const std::string & b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

class HttpClientHttp : public Http {
 public:
  HttpClientHttp() {
#ifdef _WIN32
    httpclient::WinHTTPClientFactory factory(kUserAgent);
#else
    httpclient::CurlClientFactory factory(kUserAgent);  // libcurl is initialized when the program starts
#endif
    client_ = factory.createClient(/*enable_cookies=*/false, /*enable_keepalive=*/true);
    client_->setConnectTimeout(10);
    client_->setReadTimeout(20);
    client_->setConnectionTimeout(30);
  }

  HttpResult get(const std::string & url, const HttpHeaders & headers) override {
    if (cancelled()) return cancelledResult();
    httpclient::HTTPRequest request(httpclient::Method::GET, url);
    request.setFollowLocation(true);
    request.setConnectTimeout(client_->getConnectTimeout());
    request.setReadTimeout(client_->getReadTimeout());
    request.setConnectionTimeout(client_->getConnectionTimeout());
    for (const auto & [name, value] : headers) request.addHeader(name, value);
    Response response(*this);
    client_->request(request, client_->noAuth, response);
    if (cancelled()) return cancelledResult();  // what came before is not an answer: it may be cut short
    HttpResult r;
    r.status = response.getResultCode();
    r.body = response.getContent();
    r.error = response.getErrorText();
    return r;
  }

 private:
  // A response that ends the transfer when the Http is cancelled: httpclient asks onIdle() about once a second while it waits
  // (connecting, or for data), and a chunk that is not taken ends it at once.
  class Response : public httpclient::HTTPResponse {
   public:
    explicit Response(const Http & http) : http_(http) {}
    bool onIdle() override { return !http_.cancelled(); }
    bool handleChunk(std::string_view chunk) override { return !http_.cancelled() && HTTPResponse::handleChunk(chunk); }

   private:
    const Http & http_;
  };
  static HttpResult cancelledResult() {
    HttpResult r;
    r.error = "cancelled";
    return r;
  }
  // Some providers refuse an empty or library user agent; the adapters that need a browser's say so with browserHeaders().
  static constexpr const char * kUserAgent = "Mozilla/5.0 (compatible; stocks)";
  std::unique_ptr<httpclient::HTTPClient> client_;
};

}  // namespace

HttpHeaders browserHeaders(const HttpHeaders & own) {
  HttpHeaders headers = {{"User-Agent", kBrowserUserAgent},
                         {"Accept", "*/*"},
                         {"Accept-Language", "en-US,en;q=0.9"},
                         {"sec-ch-ua", "\"Google Chrome\";v=\"131\", \"Chromium\";v=\"131\", \"Not_A Brand\";v=\"24\""},
                         {"sec-ch-ua-mobile", "?0"},
                         {"sec-ch-ua-platform", "\"Windows\""},
                         {"Sec-Fetch-Dest", "empty"},
                         {"Sec-Fetch-Mode", "cors"}};
  for (const auto & h : own) {
    headers.erase(std::remove_if(headers.begin(), headers.end(), [&](const auto & b) { return sameName(b.first, h.first); }), headers.end());
    headers.push_back(h);
  }
  return headers;
}

std::unique_ptr<Http> makeHttp() { return std::make_unique<HttpClientHttp>(); }
