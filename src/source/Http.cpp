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
    request.setConnectTimeout(client_->getConnectTimeout());
    request.setReadTimeout(client_->getReadTimeout());
    request.setConnectionTimeout(client_->getConnectionTimeout());
    for (const auto & [name, value] : headers) request.addHeader(name, value);
    httpclient::HTTPResponse response;
    client_->request(request, client_->noAuth, response);
    HttpResult r;
    r.status = response.getResultCode();
    r.body = response.getContent();
    r.error = response.getErrorText();
    return r;
  }

 private:
  // Some providers refuse an empty or library user agent.
  static constexpr const char * kUserAgent = "Mozilla/5.0 (compatible; stocks)";
  std::unique_ptr<httpclient::HTTPClient> client_;
};

}  // namespace

std::unique_ptr<Http> makeHttp() { return std::make_unique<HttpClientHttp>(); }
