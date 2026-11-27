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

// One browser, kept in step: the user agent and the client hints both follow the Chrome major version.
constexpr int kChromeMajor = 158;

bool sameName(const std::string & a, const std::string & b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

std::string lower(std::string s) {
  for (char & c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// "https://a.example.com:8443/x?y" -> "https://a.example.com:8443"; "" when it is not a URL.
std::string originOf(const std::string & url) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos || scheme == 0) return "";
  const size_t end = url.find_first_of("/?#", scheme + 3);
  return lower(url.substr(0, end));
}

// The site of an origin: its host without the port, cut to the last two labels ("query1.finance.yahoo.com" -> "yahoo.com"). Not the
// public suffix list, so a host under a two-label suffix (co.uk) is cut too short; none of the providers' is.
std::string siteOf(const std::string & origin) {
  std::string host = origin.substr(origin.find("://") + 3);
  host = host.substr(0, host.find(':'));
  const size_t last = host.rfind('.');
  if (last == std::string::npos || last == 0) return host;
  const size_t before = host.rfind('.', last - 1);
  return before == std::string::npos ? host : host.substr(before + 1);
}

// Chrome's sec-ch-ua: its two brands and a made-up one whose name, version and place follow from the major version (Chromium's
// GetGreasedUserAgentBrandVersion), so that the hints a version sends are the ones the real one sends.
std::string clientHintBrands(int major) {
  static const char * const kChars[] = {" ", "(", ":", "-", ".", "/", ")", ";", "=", "?", "_"};
  static const char * const kVersions[] = {"8", "99", "24"};
  static const int kOrders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  const std::string version = std::to_string(major);
  const std::string brands[3] = {"\"Not" + std::string(kChars[major % 11]) + "A" + kChars[(major + 1) % 11] + "Brand\";v=\"" + kVersions[major % 3] + "\"",
                                 "\"Chromium\";v=\"" + version + "\"", "\"Google Chrome\";v=\"" + version + "\""};
  std::string placed[3];
  for (int i = 0; i < 3; i++) placed[kOrders[major % 6][i]] = brands[i];
  return placed[0] + ", " + placed[1] + ", " + placed[2];
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
  // Some providers refuse an empty or library user agent; the adapters that need a browser's say so with browserHeaders().
  static constexpr const char * kUserAgent = "Mozilla/5.0 (compatible; stocks)";
  std::unique_ptr<httpclient::HTTPClient> client_;
};

}  // namespace

HttpHeaders browserHeaders(const std::string & url, const HttpHeaders & own, BrowserRequest kind) {
  const auto given = [&](const char * name) -> const std::string * {
    for (const auto & h : own)
      if (sameName(h.first, name)) return &h.second;
    return nullptr;
  };
  const std::string target = originOf(url);
  const std::string * referer = given("Referer");
  const std::string from = referer && !referer->empty() ? originOf(*referer) : (kind == BrowserRequest::Fetch ? target : "");
  const char * site = from.empty() ? "none" : from == target ? "same-origin" : siteOf(from) == siteOf(target) ? "same-site" : "cross-site";
  const std::string version = std::to_string(kChromeMajor);
  HttpHeaders headers = {{"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" + version +
                                            ".0.0.0 Safari/537.36"},
                         {"Accept-Language", "en-US,en;q=0.9"},
                         {"sec-ch-ua", clientHintBrands(kChromeMajor)},
                         {"sec-ch-ua-mobile", "?0"},
                         {"sec-ch-ua-platform", "\"Windows\""},
                         {"Sec-Fetch-Site", site}};
  if (kind == BrowserRequest::Fetch) {
    headers.insert(headers.end(), {{"Accept", "*/*"}, {"Sec-Fetch-Mode", "cors"}, {"Sec-Fetch-Dest", "empty"}});
    if (from != target) headers.push_back({"Origin", from});
  } else {
    headers.insert(headers.end(), {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8"},
                                   {"Sec-Fetch-Mode", "navigate"},
                                   {"Sec-Fetch-Dest", "document"},
                                   {"Sec-Fetch-User", "?1"},
                                   {"Upgrade-Insecure-Requests", "1"}});
  }
  headers.erase(std::remove_if(headers.begin(), headers.end(), [&](const auto & b) { return given(b.first.c_str()) != nullptr; }), headers.end());
  for (const auto & h : own)
    if (!h.second.empty()) headers.push_back(h);
  return headers;
}

std::unique_ptr<Http> makeHttp() { return std::make_unique<HttpClientHttp>(); }
