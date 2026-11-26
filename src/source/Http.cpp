#include "source/Http.h"

#include <HTTPClient.h>
#ifdef _WIN32
#include <WinHTTPClient.h>
#else
#include <CurlClient.h>
#endif

namespace {

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
    httpclient::HTTPRequest request(httpclient::Method::GET, url);
    request.setFollowLocation(true);
    return send(request, headers);
  }

  HttpResult post(const std::string & url, const std::string & body, const HttpHeaders & headers) override {
    httpclient::HTTPRequest request(httpclient::Method::POST, url);
    request.setContent(body);
    request.setContentType("application/x-www-form-urlencoded");
    return send(request, headers);
  }

 private:
  HttpResult send(httpclient::HTTPRequest & request, const HttpHeaders & headers) {
    if (cancelled()) return cancelledResult();
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
  // Some providers refuse an empty or library user agent.
  static constexpr const char * kUserAgent = "Mozilla/5.0 (compatible; stocks)";
  std::unique_ptr<httpclient::HTTPClient> client_;
};

}  // namespace

std::unique_ptr<Http> makeHttp() { return std::make_unique<HttpClientHttp>(); }
