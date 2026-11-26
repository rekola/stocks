#include "market/Api.h"

#include "model/Dates.h"
#include "model/NodeKey.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace market {

namespace {

using nlohmann::json;

struct Window {
  const char * id;
  Resolution level;
  int64_t days;  // 0: the stored window of the level
};

constexpr Window kWindows[] = {
    {"day", Resolution::Min5, 0},   {"week", Resolution::Hour1, 7},   {"month", Resolution::Hour1, 30},   {"3months", Resolution::Day, 90},
    {"1year", Resolution::Day, 365}, {"3years", Resolution::Day, 1095}, {"10years", Resolution::Day, 3650},
};

std::string lower(std::string s) {
  for (char & c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

ApiResponse reply(int status, const json & body) { return {status, body.dump()}; }
ApiResponse failure(int status, const std::string & message) { return reply(status, {{"error", message}}); }

// The first key of a window that ends at `last`: the keys are consecutive, so the days are counted in the level's own unit.
int64_t firstKeyOf(const Window & w, int64_t last) {
  if (w.days == 0) return windowFirstKey(w.level, last);
  const int64_t perDay = w.level == Resolution::Hour1 ? 24 : 1;
  return last - w.days * perDay + 1;
}

Timestamp lastBarTimeOf(const InstrumentInfo & info) {
  Timestamp last = 0;
  for (const LevelState & s : info.levels) last = std::max(last, s.lastBarTime);
  return last;
}

const LevelState * stateOf(const InstrumentInfo & info, Resolution level) {
  for (const LevelState & s : info.levels)
    if (s.level == level) return &s;
  return nullptr;
}

json instrumentJson(const InstrumentInfo & info) {
  const InstrumentRecord & r = info.instrument;
  json out = {{"code", r.code},
              {"name", r.name},
              {"asset_class", r.assetClass},
              {"base", text(NodeKey{r.edge.base})},
              {"quote", text(NodeKey{r.edge.quote})},
              {"market", text(NodeKey{r.edge.market})},
              {"dataset", text(NodeKey{r.edge.dataset})},
              {"first_bar_time", nullptr},
              {"last_bar_time", nullptr}};
  bool any = false;
  Timestamp first = 0;
  for (const LevelState & s : info.levels) {
    if (s.barCount == 0) continue;
    const Timestamp t = barStart(s.level, s.firstKey);
    first = any ? std::min(first, t) : t;
    any = true;
  }
  if (any) {
    out["first_bar_time"] = first;
    out["last_bar_time"] = lastBarTimeOf(info);
  }
  return out;
}

json summaryJson(MarketStore & store, const InstrumentInfo & info) {
  json out = {{"code", info.instrument.code},
              {"name", info.instrument.name},
              {"asset_class", info.instrument.assetClass},
              {"last_bar_time", nullptr},
              {"windows", json::object()}};
  const Timestamp anchor = lastBarTimeOf(info);
  bool loaded = false;
  for (const LevelState & s : info.levels) loaded = loaded || s.barCount > 0;
  if (loaded) out["last_bar_time"] = anchor;
  for (const Window & w : kWindows) {
    json window = {{"level", name(w.level)}, {"keys", json::array()}, {"close", json::array()}};
    const LevelState * state = stateOf(info, w.level);
    if (loaded && state && state->barCount > 0) {
      const int64_t last = barKey(w.level, anchor);
      const std::vector<Bar> bars = store.bars(info.instrument.edge, w.level, firstKeyOf(w, last), last);
      json & keys = window["keys"];
      json & closes = window["close"];
      for (const Bar & bar : bars) keys.push_back(bar.key), closes.push_back(bar.close);
      if (w.level == Resolution::Min5) {
        // What the day's change is measured from: the close before the day of the last bar.
        window["previous_close"] = nullptr;
        const int64_t day = barKey(Resolution::Day, anchor);
        const std::vector<Bar> before = store.bars(info.instrument.edge, Resolution::Day, day - 10, day - 1);
        if (!before.empty()) window["previous_close"] = before.back().close;
      }
    } else if (w.level == Resolution::Min5) {
      window["previous_close"] = nullptr;
    }
    out["windows"][w.id] = std::move(window);
  }
  return out;
}

}  // namespace

ApiResponse handleApi(MarketStore & store, const std::string & path, const std::map<std::string, std::string> & params) {
  try {
    if (path == "/health") {
      if (!store.healthy()) return failure(503, "the database does not answer");
      return reply(200, {{"status", "ok"}});
    }
    if (path == "/instruments") {
      json list = json::array();
      for (const InstrumentInfo & info : store.instruments()) list.push_back(instrumentJson(info));
      return reply(200, {{"instruments", list}});
    }
    if (path == "/summary") {
      std::vector<InstrumentInfo> all = store.instruments();
      std::vector<InstrumentInfo> chosen;
      const auto codes = params.find("codes");
      if (codes == params.end() || codes->second.empty()) {
        chosen = std::move(all);
      } else {
        std::set<std::string> done;
        std::string rest = codes->second + ",";
        for (size_t comma = rest.find(','); comma != std::string::npos; comma = rest.find(',')) {
          std::string code = lower(rest.substr(0, comma));
          rest.erase(0, comma + 1);
          code.erase(std::remove_if(code.begin(), code.end(), [](unsigned char c) { return std::isspace(c); }), code.end());
          if (code.empty() || !done.insert(code).second) continue;
          const auto it = std::find_if(all.begin(), all.end(), [&](const InstrumentInfo & i) { return i.instrument.code == code; });
          if (it == all.end()) return failure(404, "no instrument " + code);
          chosen.push_back(*it);
        }
      }
      json list = json::array();
      for (const InstrumentInfo & info : chosen) list.push_back(summaryJson(store, info));
      return reply(200, {{"instruments", list}});
    }
    return failure(404, "no such path: " + path);
  } catch (const std::exception & e) {
    return failure(503, std::string("the store failed: ") + e.what());
  }
}

struct ApiServer::Impl {
  httplib::Server server;
  int port = 0;
};

ApiServer::ApiServer(MarketStore & store) : impl_(std::make_unique<Impl>()) {
  httplib::Server & s = impl_->server;
  // Read-only: any method but GET (and HEAD) is refused before it is routed.
  s.set_pre_routing_handler([](const httplib::Request & req, httplib::Response & res) {
    if (req.method == "GET" || req.method == "HEAD") return httplib::Server::HandlerResponse::Unhandled;
    res.status = 405;
    res.set_header("Allow", "GET, HEAD");
    res.set_content(json{{"error", "read-only: only GET"}}.dump(), "application/json");
    return httplib::Server::HandlerResponse::Handled;
  });
  auto serve = [&store](const httplib::Request & req, httplib::Response & res) {
    std::map<std::string, std::string> params;
    for (const auto & [key, value] : req.params) params[key] = value;
    const ApiResponse answer = handleApi(store, req.path, params);
    res.status = answer.status;
    res.set_content(answer.body, "application/json");
  };
  s.Get("/health", serve);
  s.Get("/instruments", serve);
  s.Get("/summary", serve);
  s.set_error_handler([](const httplib::Request &, httplib::Response & res) {
    if (res.body.empty()) res.set_content(json{{"error", "no such path"}}.dump(), "application/json");
  });
}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::bind(const std::string & host, int port, std::string & error) {
  impl_->port = port == 0 ? impl_->server.bind_to_any_port(host) : (impl_->server.bind_to_port(host, port) ? port : -1);
  if (impl_->port <= 0) {
    error = "cannot listen on " + host + ":" + std::to_string(port);
    return false;
  }
  return true;
}

void ApiServer::serve() { impl_->server.listen_after_bind(); }

bool ApiServer::listen(const std::string & host, int port, std::string & error) {
  if (!bind(host, port, error)) return false;
  serve();
  return true;
}

int ApiServer::port() const { return impl_->port; }
void ApiServer::stop() { impl_->server.stop(); }

}  // namespace market
