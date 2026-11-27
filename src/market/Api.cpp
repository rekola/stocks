// httplib first: the model declares a global seconds(Resolution) that its own `seconds` would otherwise be ambiguous with.
#include <httplib.h>

#include "market/Api.h"

#include "market/Bars.h"
#include "model/NodeKey.h"
#include "model/Period.h"
#include "model/SummaryRecords.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <unordered_set>

namespace market {

namespace {

using nlohmann::json;

std::string lower(std::string s) {
  for (char & c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

ApiResponse reply(int status, const json & body) { return {status, body.dump()}; }
ApiResponse failure(int status, const std::string & message) { return reply(status, {{"error", message}}); }

constexpr Period kPeriods[] = {Period::Day, Period::Week, Period::Month, Period::Quarter, Period::Year, Period::Year3, Period::Year10};

// What the API calls an edge: the dataset's symbol for it, in lower case ("eurusd").
std::string codeOf(const Snapshot & s, const EdgeKey & e) { return lower(s.symbol(e)); }

std::vector<EdgeKey> listedEdges(const Snapshot & s) {
  std::vector<EdgeKey> edges = s.quoteRowsSince(0);
  std::sort(edges.begin(), edges.end(), [&](const EdgeKey & a, const EdgeKey & b) { return codeOf(s, a) < codeOf(s, b); });
  return edges;
}

// The instant of the last bar of any queue of the edge (UTC seconds); 0 where it has none.
Timestamp lastBarTimeOf(const Snapshot & s, const EdgeKey & e) {
  Timestamp last = 0;
  for (Resolution r : kResolutions) {
    const auto c = s.coverage(e, r);
    if (c && c->count > 0) last = std::max<Timestamp>(last, barStart(r, c->last_key));
  }
  return last;
}

json instrumentJson(const Snapshot & s, const EdgeKey & e) {
  json out = {{"code", codeOf(s, e)},
              {"name", s.listing(e) ? s.listing(e)->name : std::string()},
              {"asset_class", kindName(s.kind(e.base))},
              {"base", text(NodeKey{e.base.value})},
              {"quote", text(NodeKey{e.quote.value})},
              {"market", text(NodeKey{e.market.value})},
              {"dataset", text(NodeKey{e.dataset.value})},
              {"first_bar_time", nullptr},
              {"last_bar_time", nullptr}};
  bool any = false;
  Timestamp first = 0;
  for (Resolution r : kResolutions) {
    const auto c = s.coverage(e, r);
    if (!c || c->count == 0) continue;
    const Timestamp t = barStart(r, c->first_key);
    first = any ? std::min(first, t) : t;
    any = true;
  }
  if (any) {
    out["first_bar_time"] = first;
    out["last_bar_time"] = lastBarTimeOf(s, e);
  }
  return out;
}

json summaryJson(const Snapshot & s, const EdgeKey & e) {
  json out = {{"code", codeOf(s, e)},
              {"name", s.listing(e) ? s.listing(e)->name : std::string()},
              {"asset_class", kindName(s.kind(e.base))},
              {"last_bar_time", nullptr},
              {"periods", json::object()}};
  const Timestamp anchor = lastBarTimeOf(s, e);
  if (anchor > 0) out["last_bar_time"] = anchor;
  for (Period p : kPeriods) {
    const PeriodInfo & in = info(p);
    json period = {{"resolution", nullptr}, {"keys", json::array()}, {"close", json::array()}};
    if (p == Period::Day) period["previous_close"] = nullptr;
    // The first of the period's own feeds that has bars in memory (not the stand-ins the application falls back on: a resolution that was not
    // loaded is an empty period).
    for (size_t f = 0; f < in.primary && anchor > 0; f++) {
      const Resolution r = in.feeds[f];
      const Seq<Candle> queue = s.queue(e, r);
      if (queue.empty()) continue;
      period["resolution"] = name(r);
      const int64_t from = anchor - in.days * 86400;
      for (const Candle & c : queue) {
        if (summary::instantOf(r, c.time) < from || summary::instantOf(r, c.time) > anchor) continue;
        period["keys"].push_back(c.time);
        period["close"].push_back(c.close);
      }
      break;
    }
    if (p == Period::Day && anchor > 0) {
      // What the day's change is measured from: the close of the last day before the day of the last bar.
      const int64_t day = barKey(Resolution::Day, anchor);
      for (const Candle & c : s.queue(e, Resolution::Day))
        if (c.time < day) period["previous_close"] = c.close;
    }
    out["periods"][in.id] = std::move(period);
  }
  return out;
}

}  // namespace

void loadQueues(Store & store, int timeoutSeconds) {
  store.waitRestored();
  std::vector<SeriesKey> wanted;
  const auto snapshot = store.snapshot();
  for (const EdgeKey & e : snapshot->quoteRowsSince(0))
    for (Resolution r : kResolutions)
      if (const auto c = snapshot->coverage(e, r); c && c->count > 0) wanted.push_back({e, r});
  store.want(wanted);
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  for (;;) {
    const auto now = store.snapshot();
    const bool done = std::all_of(wanted.begin(), wanted.end(), [&](const SeriesKey & k) { return now->queueResident(k.edge, k.resolution); });
    if (done || std::chrono::steady_clock::now() > until) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

ApiResponse handleApi(Store & store, const std::string & path, const std::map<std::string, std::string> & params) {
  try {
    const auto snapshot = store.snapshot();
    const Snapshot & s = *snapshot;
    if (path == "/health") return reply(200, {{"status", "ok"}, {"instruments", s.quoteRowCount()}});
    if (path == "/instruments") {
      json list = json::array();
      for (const EdgeKey & e : listedEdges(s)) list.push_back(instrumentJson(s, e));
      return reply(200, {{"instruments", list}});
    }
    if (path == "/summary") {
      std::vector<EdgeKey> all = listedEdges(s), chosen;
      const auto codes = params.find("codes");
      if (codes == params.end() || codes->second.empty()) {
        chosen = std::move(all);
      } else {
        std::unordered_set<std::string> done;
        std::string rest = codes->second + ",";
        for (size_t comma = rest.find(','); comma != std::string::npos; comma = rest.find(',')) {
          std::string code = lower(rest.substr(0, comma));
          rest.erase(0, comma + 1);
          code.erase(std::remove_if(code.begin(), code.end(), [](unsigned char c) { return std::isspace(c); }), code.end());
          if (code.empty() || !done.insert(code).second) continue;
          const auto it = std::find_if(all.begin(), all.end(), [&](const EdgeKey & e) { return codeOf(s, e) == code; });
          if (it == all.end()) return failure(404, "no instrument " + code);
          chosen.push_back(*it);
        }
      }
      json list = json::array();
      for (const EdgeKey & e : chosen) list.push_back(summaryJson(s, e));
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

ApiServer::ApiServer(std::function<std::shared_ptr<Store>()> current) : impl_(std::make_unique<Impl>()) {
  httplib::Server & s = impl_->server;
  // Read-only: any method but GET (and HEAD) is refused before it is routed.
  s.set_pre_routing_handler([](const httplib::Request & req, httplib::Response & res) {
    if (req.method == "GET" || req.method == "HEAD") return httplib::Server::HandlerResponse::Unhandled;
    res.status = 405;
    res.set_header("Allow", "GET, HEAD");
    res.set_content(json{{"error", "read-only: only GET"}}.dump(), "application/json");
    return httplib::Server::HandlerResponse::Handled;
  });
  auto serve = [current](const httplib::Request & req, httplib::Response & res) {
    std::map<std::string, std::string> params;
    for (const auto & [key, value] : req.params) params[key] = value;
    const std::shared_ptr<Store> store = current();
    const ApiResponse answer = store ? handleApi(*store, req.path, params) : failure(503, "the store is not there yet");
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
int ApiServer::port() const { return impl_->port; }
void ApiServer::stop() { impl_->server.stop(); }

}  // namespace market
