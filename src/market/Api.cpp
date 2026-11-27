// httplib first: the model declares a global seconds(Resolution) that its own `seconds` would otherwise be ambiguous with.
#include <httplib.h>

#include "market/Api.h"

#include "market/Bars.h"
#include "model/NodeKey.h"
#include "model/Summary.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#include <optional>
#include <tuple>
#include <vector>

namespace market {

namespace {

using nlohmann::json;

ApiResponse reply(int status, const json & body) { return {status, body.dump()}; }
ApiResponse failure(int status, const std::string & message) { return reply(status, {{"error", message}}); }

// A number as JSON: null where it is undefined (NaN is not JSON) or not finite.
json number(double v) { return std::isfinite(v) ? json(v) : json(nullptr); }

std::string nodeText(uint64_t key) { return text(NodeKey{key}); }

// The canonical id of an edge, which the API returns with each instrument and takes in `?edge=`: the texts of its four keys (NodeKey.h)
// joined by "/": base, quote, market, dataset. The base and the quote are an ISIN or a code, the market a MIC, the dataset a code, and none
// of these has a "/" in it.
std::string edgeId(const EdgeKey & e) {
  return nodeText(e.base.value) + "/" + nodeText(e.quote.value) + "/" + nodeText(e.market.value) + "/" + nodeText(e.dataset.value);
}

// The edge an id names, if the id is of the form (the edge need not be in the store); empty where it is not, with `why`.
std::optional<EdgeKey> parseEdgeId(const std::string & id, std::string & why) {
  std::vector<std::string> parts(1);
  for (char c : id) {
    if (c == '/') parts.emplace_back();
    else parts.back().push_back(c);
  }
  if (parts.size() != 4) {
    why = "an edge id is BASE/QUOTE/MARKET/DATASET";
    return std::nullopt;
  }
  const auto instrument = [](const std::string & t) { return t.size() == 12 ? instrumentKey(t) : currencyKey(t); };
  const auto base = instrument(parts[0]), quote = instrument(parts[1]);
  const auto market = marketKey(parts[2]);
  const auto dataset = datasetKey(parts[3]);
  if (!base || !quote || !market || !dataset) {
    why = std::string("not a valid ") + (!base ? "base (an ISIN or a code)" : !quote ? "quote (an ISIN or a code)" : !market ? "market (a MIC)" : "dataset (a code)");
    return std::nullopt;
  }
  const EdgeKey edge{InstrumentId{base->value}, InstrumentId{quote->value}, MarketId{market->value}, DatasetId{dataset->value}};
  if (edgeId(edge) != id) {  // the model reads a code in either case; the id is one string, so only the one the API returns is taken
    why = "the id is " + edgeId(edge);
    return std::nullopt;
  }
  return edge;
}

std::vector<EdgeKey> listedEdges(const Snapshot & s) {
  std::vector<std::pair<std::string, EdgeKey>> keyed;
  for (const EdgeKey & e : s.quoteRowsSince(0)) keyed.emplace_back(edgeId(e), e);
  // by symbol (what a person looks for), then by id, so that the order is the same on every run
  std::sort(keyed.begin(), keyed.end(), [&](const auto & a, const auto & b) {
    return std::tuple(s.symbol(a.second), a.first) < std::tuple(s.symbol(b.second), b.first);
  });
  std::vector<EdgeKey> edges;
  for (const auto & k : keyed) edges.push_back(k.second);
  return edges;
}

// The start of the latest bar of any queue of the edge (UTC seconds); 0 where it has none.
Timestamp lastBarTimeOf(const Snapshot & s, const EdgeKey & e) {
  Timestamp last = 0;
  for (Resolution r : s.queues(e)) {
    const auto c = s.coverage(e, r);
    if (c && c->count > 0) last = std::max<Timestamp>(last, barStart(r, c->last_key));
  }
  return last;
}

// What identifies an instrument and how it was made: the edge by its four keys, the listing as the dataset said it, the dataset's name, and
// of the base node what a client needs to make it as the server did (its kind; its name and ISIN where it is an instrument, whose key is
// its ISIN; a currency or a unit is its code alone).
json instrumentJson(const Snapshot & s, const EdgeKey & e) {
  const Listing listing = s.listing(e).value_or(Listing{e, {}, {}, {}, {}, {}});
  const bool isInstrument = NodeKey{e.base.value}.kind() == NodeKind::Instrument;
  json out = {{"id", edgeId(e)},
              {"edge", {{"base", nodeText(e.base.value)}, {"quote", nodeText(e.quote.value)}, {"market", nodeText(e.market.value)}, {"dataset", nodeText(e.dataset.value)}}},
              {"dataset", {{"code", datasetCode(e.dataset)}, {"name", s.name(e.dataset)}}},
              {"listing", {{"symbol", listing.symbol}, {"identifier", listing.identifier}, {"order_book", listing.orderBook}, {"type", listing.type}, {"name", listing.name}}},
              {"base", {{"kind", kindName(s.kind(e.base))}, {"name", isInstrument ? json(s.name(e.base)) : json(nullptr)}, {"isin", isInstrument ? json(s.isin(e.base)) : json(nullptr)}}},
              {"last_bar_time", nullptr}};
  if (const Timestamp last = lastBarTimeOf(s, e); last > 0) out["last_bar_time"] = last;
  return out;
}

json instrumentListJson(const Snapshot & s, const EdgeKey & e) {
  json out = instrumentJson(s, e);
  out["first_bar_time"] = nullptr;
  bool any = false;
  Timestamp first = 0;
  for (Resolution r : s.queues(e)) {
    const auto c = s.coverage(e, r);
    if (!c || c->count == 0) continue;
    const Timestamp t = barStart(r, c->first_key);
    first = any ? std::min(first, t) : t;
    any = true;
  }
  if (any) out["first_bar_time"] = first;
  return out;
}

// The queue as the store holds it, in columns, oldest first: the keys of the model's bars, then open, high, low, close and volume, each
// null where it is undefined. A scalar queue has one value a bar, which is the close; the rest are null.
json queueJson(const Snapshot & s, const EdgeKey & e, Resolution r, const QueueCoverage & c) {
  const bool scalar = c.kind == QueueKind::Scalar;
  json keys = json::array(), open = json::array(), high = json::array(), low = json::array(), close = json::array(), volume = json::array();
  for (const Candle & bar : s.queue(e, r)) {
    keys.push_back(bar.time);
    open.push_back(scalar ? json(nullptr) : number(bar.open));
    high.push_back(scalar ? json(nullptr) : number(bar.high));
    low.push_back(scalar ? json(nullptr) : number(bar.low));
    close.push_back(number(bar.close));
    volume.push_back(scalar ? json(nullptr) : number(bar.volume));
  }
  return {{"kind", scalar ? "scalar" : "candle"}, {"first_key", c.first_key}, {"last_key", c.last_key}, {"count", c.count}, {"fetched", c.fetched},
          {"keys", std::move(keys)}, {"open", std::move(open)}, {"high", std::move(high)}, {"low", std::move(low)}, {"close", std::move(close)},
          {"volume", std::move(volume)}};
}

json summaryJson(const Snapshot & s, const EdgeKey & e) {
  json out = instrumentJson(s, e);
  json queues = json::object();
  // The queues in memory, each whole: a resolution that is not in memory is left out.
  for (Resolution r : s.queues(e)) {
    const auto c = s.coverage(e, r);
    if (c && c->count > 0 && s.queueResident(e, r)) queues[name(r)] = queueJson(s, e, r, *c);
  }
  out["queues"] = std::move(queues);
  return out;
}

}  // namespace

void loadQueues(Store & store, int timeoutSeconds) {
  store.waitRestored();
  std::vector<SeriesKey> wanted;
  const auto snapshot = store.snapshot();
  for (const EdgeKey & e : snapshot->quoteRowsSince(0))
    for (Resolution r : snapshot->queues(e))
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
    const bool listing = path == "/instruments", summary = path == "/summary";
    if (!listing && !summary && path != "/health") return failure(404, "no such path: " + path);
    for (const auto & [key, value] : params)
      if ((!listing && !summary) || (key != "edge" && key != "dataset")) return failure(400, "no such parameter: " + key + (listing || summary ? " (there are edge and dataset)" : ""));
    if (path == "/health") return reply(200, {{"status", "ok"}, {"instruments", s.quoteRowCount()}});

    const std::vector<EdgeKey> all = listedEdges(s);
    std::vector<EdgeKey> chosen = all;
    // ?dataset=CODE: the edges that dataset wrote.
    if (const auto it = params.find("dataset"); it != params.end()) {
      const auto dataset = datasetKey(it->second);
      if (!dataset || text(*dataset) != it->second) return failure(400, "not a dataset code: '" + it->second + "'" + (dataset ? " (the code is " + text(*dataset) + ")" : ""));
      const auto of = [&](const EdgeKey & e) { return e.dataset.value == dataset->value; };
      if (std::none_of(all.begin(), all.end(), of)) return failure(404, "no instruments of the dataset " + text(*dataset));
      chosen.erase(std::remove_if(chosen.begin(), chosen.end(), [&](const EdgeKey & e) { return !of(e); }), chosen.end());
    }
    // ?edge=ID[,ID...]: those edges, each of which must be an instrument of the store (of the dataset too, if one is asked for: those that
    // are not of it are left out).
    if (const auto it = params.find("edge"); it != params.end()) {
      std::vector<EdgeKey> wanted;
      std::string rest = it->second + ",";
      for (size_t comma = rest.find(','); comma != std::string::npos; comma = rest.find(',')) {
        const std::string id = rest.substr(0, comma);
        rest.erase(0, comma + 1);
        std::string why;
        const auto edge = parseEdgeId(id, why);
        if (!edge) return failure(400, "'" + id + "' is not an edge id: " + why);
        if (std::find(all.begin(), all.end(), *edge) == all.end()) return failure(404, "no instrument with the edge " + edgeId(*edge));
        wanted.push_back(*edge);
      }
      chosen.erase(std::remove_if(chosen.begin(), chosen.end(), [&](const EdgeKey & e) { return std::find(wanted.begin(), wanted.end(), e) == wanted.end(); }), chosen.end());
    }
    json list = json::array();
    for (const EdgeKey & e : chosen) list.push_back(listing ? instrumentListJson(s, e) : summaryJson(s, e));
    return reply(200, {{"instruments", list}});
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
