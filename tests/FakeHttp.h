#pragma once

#include "source/Http.h"
#include "source/Source.h"

#include <functional>
#include <string>
#include <vector>

// An Http that answers from a function and records what it was asked.
class FakeHttp : public Http {
 public:
  std::function<HttpResult(const std::string &)> answer;
  std::vector<std::string> urls;
  std::vector<HttpHeaders> headers;
  HttpResult get(const std::string & url, const HttpHeaders & h) override {
    urls.push_back(url);
    headers.push_back(h);
    return answer(url);
  }
};

inline HttpResult ok(std::string body) {
  HttpResult r;
  r.status = 200;
  r.body = std::move(body);
  return r;
}
inline HttpResult failed(int status) {
  HttpResult r;
  r.status = status;
  return r;
}

// A listener that counts and keeps what it was told.
struct Listener : SourceListener {
  int changes = 0;
  std::vector<std::string> messages;
  void changed() override { changes++; }
  void message(const std::string & m) override { messages.push_back(m); }
};
