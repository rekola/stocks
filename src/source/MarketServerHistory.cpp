#include "source/MarketServerHistory.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>
#include <unordered_map>

using json = nlohmann::json;
using Outcome = HistoryProvider::Outcome;

std::string marketServerEdgeId(const EdgeKey & e) {
  return text(NodeKey{e.base.value}) + "/" + text(NodeKey{e.quote.value}) + "/" + text(NodeKey{e.market.value}) + "/" + text(NodeKey{e.dataset.value});
}

namespace {

// A query value with everything but the unreserved characters as %XX: an id has "/" and its dataset may have "#".
std::string encoded(const std::string & s) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  for (char ch : s) {
    const unsigned c = static_cast<unsigned char>(ch);
    if (std::isalnum(static_cast<int>(c)) || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

// What the server says of a failure: its "error", else the status.
std::string errorOf(const HttpResult & r) {
  const json doc = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_object() && doc.contains("error") && doc["error"].is_string()) return doc["error"].get<std::string>();
  return r.body.empty() ? "no message" : r.body.substr(0, 200);
}

[[noreturn]] void malformed(const std::string & what) { throw std::runtime_error("market-server: the answer is not a summary: " + what); }

// One column of an answer: `count` numbers, each null where undefined.
std::vector<double> column(const json & queue, const char * key, size_t count) {
  const auto it = queue.find(key);
  if (it == queue.end() || !it->is_array() || it->size() != count) malformed(std::string("the column ") + key);
  std::vector<double> out;
  out.reserve(count);
  for (const json & v : *it) {
    if (v.is_null()) out.push_back(kUndefined);
    else if (v.is_number() && std::isfinite(v.get<double>())) out.push_back(v.get<double>());
    else malformed(std::string("a value in ") + key);
  }
  return out;
}

struct Queue {
  Resolution resolution = Resolution::Day;
  QueueKind kind = QueueKind::Candle;
  std::vector<Candle> bars;
};

Queue queueOf(Resolution resolution, const json & queue) {
  if (!queue.is_object()) malformed("a queue");
  Queue out;
  out.resolution = resolution;
  const auto kind = queue.find("kind");
  if (kind == queue.end() || !kind->is_string()) malformed("a queue with no kind");
  const std::string k = kind->get<std::string>();
  if (k == "scalar") out.kind = QueueKind::Scalar;
  else if (k != "candle") malformed("the queue kind " + k);
  const auto keys = queue.find("keys");
  if (keys == queue.end() || !keys->is_array()) malformed("the column keys");
  const size_t count = keys->size();
  const std::vector<double> open = column(queue, "open", count), high = column(queue, "high", count), low = column(queue, "low", count),
                            close = column(queue, "close", count), volume = column(queue, "volume", count);
  for (size_t i = 0; i < count; i++) {
    const json & key = (*keys)[i];
    if (!key.is_number_integer()) malformed("a key");
    // As the server says it, null for null: nothing is made up (an open, a high or a low a source did not give stays undefined).
    out.bars.push_back({key.get<int64_t>(), open[i], high[i], low[i], close[i], volume[i]});
  }
  return out;
}

}  // namespace

MarketServerHistory::MarketServerHistory(std::string baseUrl, std::chrono::milliseconds lifetime) : url_(std::move(baseUrl)), lifetime_(lifetime) {
  while (!url_.empty() && url_.back() == '/') url_.pop_back();
}

void MarketServerHistory::setDatasets(std::vector<std::string> codes) {
  std::lock_guard lock(mutex_);
  datasets_ = std::move(codes);
}

std::string MarketServerHistory::dataset() const {
  std::lock_guard lock(mutex_);
  return datasets_.empty() ? std::string() : datasets_.front();
}

std::vector<std::string> MarketServerHistory::datasets() const {
  std::lock_guard lock(mutex_);
  return datasets_;
}

MarketServerHistory::Request MarketServerHistory::requestOf(Period period) {
  switch (period) {
    case Period::Day: return Intraday;
    case Period::Week: return Hourly;
    case Period::Month:
    case Period::Quarter: return Daily;
    case Period::Year:
    case Period::Year3:
    case Period::Year10: return Weekly;
  }
  return Daily;
}

std::vector<Resolution> MarketServerHistory::resolutionsOf(int request) {
  switch (request) {
    case Intraday: return {Resolution::Min5, Resolution::Day};
    case Hourly: return {Resolution::Hour1};
    case Weekly: return {Resolution::Week};
    default: return {Resolution::Day};
  }
}

std::optional<HistoryProvider::Need> MarketServerHistory::need(const Snapshot & snapshot, const EdgeKey & edge, Period period) const {
  if (!snapshot.listing(edge)) return std::nullopt;
  // What the store has for the period already (in memory, or the summary on the disk) is used until it has expired.
  const Covered covered = periodCovered(snapshot, edge, period);
  return Need{requestOf(period), covered.covered, covered.fetched};
}

std::optional<std::chrono::seconds> MarketServerHistory::keep(Resolution resolution) const {
  switch (resolution) {
    case Resolution::Min5:
    case Resolution::Hour1:
    case Resolution::Day:
    case Resolution::Week: return defaultKeep(resolution);
    default: return std::nullopt;  // the server keeps no queue of any other
  }
}

HistoryProvider::Outcome MarketServerHistory::load(Store & store, Http & http, const EdgeKey & edge, int request, const Wait & wait) {
  return loadBatch(store, http, {edge}, request, wait).front();
}

std::vector<HistoryProvider::Outcome> MarketServerHistory::loadBatch(Store & store, Http & http, const std::vector<EdgeKey> & edges, int request,
                                                                     const Wait &) {
  if (edges.empty()) return {};
  const std::vector<Resolution> resolutions = resolutionsOf(request);
  std::vector<std::string> ids;
  std::string url = url_ + "/summary?edge=";
  for (const EdgeKey & e : edges) {
    ids.push_back(marketServerEdgeId(e));
    if (ids.size() > 1) url += ',';
    url += encoded(ids.back());
  }
  url += "&resolution=";
  for (size_t i = 0; i < resolutions.size(); i++) url += std::string(i ? "," : "") + name(resolutions[i]);

  const HttpResult r = http.get(url);
  if (r.status == 403 || r.status == 429) return std::vector<Outcome>(edges.size(), Outcome::Refused);
  if (r.status == 0 || r.status >= 500) return std::vector<Outcome>(edges.size(), Outcome::Offline);  // no answer, or the server is failing: again after the loader's pause
  if (r.status == 400) throw std::runtime_error("market-server: the request was refused as wrong (a bug of the application): " + errorOf(r));
  if (!r.ok()) throw std::runtime_error("market-server: HTTP " + std::to_string(r.status) + " from " + url_ + " (is it a market server?)");

  // Read the whole answer before anything is written: an answer that is not one writes nothing.
  const json doc = json::parse(r.body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) malformed("not JSON");
  const auto instruments = doc.find("instruments"), missing = doc.find("missing");
  if (instruments == doc.end() || !instruments->is_array() || missing == doc.end() || !missing->is_array()) malformed("no instruments and missing");
  std::set<std::string> gone;
  for (const json & id : *missing)
    if (id.is_string()) gone.insert(id.get<std::string>());
  std::unordered_map<std::string, const json *> present;
  for (const json & instrument : *instruments) {
    const auto id = instrument.is_object() ? instrument.find("id") : instrument.end();
    if (instrument.is_object() && id != instrument.end() && id->is_string()) present[id->get<std::string>()] = &instrument;
  }

  struct Found {
    size_t edge;
    std::vector<Queue> queues;
  };
  std::vector<Outcome> outcomes(edges.size(), Outcome::Missing);
  std::vector<Found> writes;
  for (size_t i = 0; i < edges.size(); i++) {
    if (gone.count(ids[i])) continue;  // Missing: the server has no such edge
    const auto at = present.find(ids[i]);
    if (at == present.end()) continue;  // in neither list: nothing is said of it, and it is not asked for again
    const auto queues = at->second->find("queues");
    Found found{i, {}};
    bool primary = false;
    if (queues != at->second->end() && queues->is_object())
      for (size_t k = 0; k < resolutions.size(); k++) {
        const auto queue = queues->find(name(resolutions[k]));
        if (queue == queues->end()) continue;
        found.queues.push_back(queueOf(resolutions[k], *queue));
        if (k == 0) primary = true;
      }
    if (!found.queues.empty()) writes.push_back(std::move(found));
    if (primary) outcomes[i] = Outcome::Ok;
    else {
      // Listed, and without the queue asked for: on the server's disk and not in memory yet, so again later, a few times (it may be that it has
      // none of this resolution, and never will).
      outcomes[i] = Outcome::Offline;
    }
  }
  if (!writes.empty())
    store.batch([&](Batch & b) {
      for (const Found & f : writes)
        for (const Queue & q : f.queues) b.appendCandles(edges[f.edge], q.resolution, q.bars, q.kind);
    });
  {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < edges.size(); i++) {
      const std::string counted = ids[i] + " " + std::to_string(request);
      if (outcomes[i] == Outcome::Ok) later_.erase(counted);
      else if (outcomes[i] == Outcome::Offline && ++later_[counted] >= kTries) {
        later_.erase(counted);
        outcomes[i] = Outcome::Missing;  // given up: it is not asked for again this session
      }
    }
  }
  return outcomes;
}
