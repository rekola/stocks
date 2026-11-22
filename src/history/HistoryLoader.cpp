#include "history/HistoryLoader.h"

#include "model/Dates.h"

#include <nlohmann/json.hpp>

#include <algorithm>

using nlohmann::json;

namespace {

// What the endpoint expects of a browser (the same for all its requests); no cookie is sent.
const HttpHeaders kHeaders = {{"Accept", "*/*"},
                              {"Origin", "https://www.nordnet.fi"},
                              {"Referer", "https://www.nordnet.fi/"},
                              {"x-locale", "fi-FI"}};

const char * const kUrl = "https://api.prod.nntech.io/market-data/v3/price-time-series/period/";

// The periods tried for daily bars, longest first, where the whole period came back coarser.
const char * const kDailyPeriods[] = {"YEAR_10", "YEAR_5", "YEAR_3", "YEAR_1"};

bool resolutionOf(const std::string & name, Resolution & out) {
  static const std::pair<const char *, Resolution> known[] = {{"DAY", Resolution::Day},
                                                               {"WEEK", Resolution::Week},
                                                               {"MONTH", Resolution::Month},
                                                               {"QUARTER", Resolution::Quarter}};
  for (const auto & [n, r] : known)
    if (name == n) {
      out = r;
      return true;
    }
  return false;
}

// The key of a bar: its date as a day number, and for a coarser bar the first day of the period it stands for.
int64_t keyOf(Resolution r, int64_t day) {
  switch (r) {
    case Resolution::Week: return weekStart(day);
    case Resolution::Month: return monthStart(day);
    case Resolution::Quarter: return quarterStart(day);
    default: return day;
  }
}

std::optional<double> number(const json & o, const char * key) {
  auto it = o.find(key);
  return it != o.end() && it->is_number() ? std::optional<double>(it->get<double>()) : std::nullopt;
}

}  // namespace

bool HistoryLoader::parse(const std::string & body, bool nav, Series & out, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("pricePoints") || !doc["pricePoints"].is_array()) {
    error = "not a price time series";
    return false;
  }
  out = Series();
  auto name = doc.find("returnedResolution");
  out.supported = name != doc.end() && name->is_string() && resolutionOf(name->get<std::string>(), out.resolution);
  if (auto periods = doc.find("availablePeriods"); periods != doc.end() && periods->is_array())
    for (const json & p : *periods)
      if (p.is_string()) out.periods.push_back(p.get<std::string>());
  if (!out.supported) return true;  // intraday and the like: not stored yet
  for (const json & p : doc["pricePoints"]) {
    auto stamp = p.find("timeStamp");
    if (!p.is_object() || stamp == p.end() || !stamp->is_number_integer()) continue;
    const int64_t key = keyOf(out.resolution, dayNumberOfInstant(stamp->get<int64_t>() / 1000));
    Candle c;
    c.time = key;
    c.open = c.high = c.low = c.volume = kUndefined;
    if (nav) {
      // A fund's NAV: only a value, which is the close.
      auto value = number(p, "value");
      if (!value) continue;
      c.close = *value;
    } else {
      auto last = number(p, "last");
      if (!last) continue;
      c.close = *last;
      c.open = number(p, "open").value_or(kUndefined);
      c.high = number(p, "high").value_or(kUndefined);
      c.low = number(p, "low").value_or(kUndefined);
      // A volume of 0 is what a market with none (a commodity, an ETC) gives: no volume, not no trades.
      if (auto volume = number(p, "volume"); volume && *volume > 0) c.volume = *volume;
    }
    if (!out.bars.empty() && out.bars.back().time == c.time) out.bars.back() = c;  // the same period twice: the later
    else out.bars.push_back(c);
  }
  return true;
}

HistoryLoader::HistoryLoader(std::shared_ptr<Store> store, std::unique_ptr<Http> http, Options options, std::function<void()> changed,
                             std::function<void(std::string)> message)
    : store_(std::move(store)), http_(std::move(http)), options_(options), changed_(std::move(changed)), message_(std::move(message)) {
  thread_ = std::thread([this] { run(); });
}

HistoryLoader::~HistoryLoader() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool HistoryLoader::refused() const {
  std::lock_guard lock(mutex_);
  return refused_;
}

void HistoryLoader::waitIdle() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [&] { return (queue_.empty() && !busy_) || refused_; });
}

void HistoryLoader::want(const std::vector<EdgeKey> & edges) {
  auto snapshot = store_->snapshot();
  {
    std::lock_guard lock(mutex_);
    queue_.clear();
    std::unordered_set<EdgeKey> seen;
    for (const EdgeKey & k : edges) {
      if (done_.count(k) || failed_.count(k) || !seen.insert(k).second) continue;
      auto dataset = snapshot->dataset(k.dataset);
      auto listing = snapshot->listing(k);
      if (!dataset || dataset->name != "nordnet" || !listing || listing->orderBook.empty()) continue;
      // More than today's bar: the history is there already (loaded before, or from elsewhere).
      if (snapshot->lastCandles(k, Resolution::Day, 6).candles.size() >= 6) continue;
      queue_.push_back(k);
    }
  }
  wake_.notify_all();
}

void HistoryLoader::run() {
  std::unique_lock lock(mutex_);
  while (true) {
    wake_.wait(lock, [&] { return stop_ || (!queue_.empty() && !refused_); });
    if (stop_) return;
    if (std::chrono::steady_clock::now() < offline_until_) {
      // No answer lately: wait it out (a destructor cuts it short).
      wake_.wait_until(lock, offline_until_, [&] { return stop_; });
      continue;
    }
    const EdgeKey edge = queue_.front();
    queue_.pop_front();
    busy_ = true;
    lock.unlock();
    bool network = false;
    const bool wrote = load(edge, network);
    if (wrote && changed_) changed_();
    lock.lock();
    busy_ = false;
    if (network && options_.pause.count() > 0) wake_.wait_for(lock, options_.pause, [&] { return stop_; });
    if (queue_.empty()) idle_.notify_all();
  }
}

HistoryLoader::Outcome HistoryLoader::request(const std::string & book, const std::string & period, bool nav, Series & out) {
  std::string url = std::string(kUrl) + period + "/identifier/" + book + "?resolution=DAY";
  if (nav) url += "&fundType=FUND_NAV";
  HttpResult r = http_->get(url, kHeaders);
  if (r.status == 403 || r.status == 429) return Outcome::Refused;
  if (r.status == 0 || r.status >= 500) return Outcome::Offline;
  if (!r.ok()) return Outcome::Missing;
  std::string error;
  if (!parse(r.body, nav, out, error)) return Outcome::Missing;
  return Outcome::Ok;
}

bool HistoryLoader::load(const EdgeKey & edge, bool & network) {
  auto snapshot = store_->snapshot();
  auto listing = snapshot->listing(edge);
  auto base = snapshot->instrument(edge.base);
  if (!listing || !base) return false;
  const bool nav = base->kind == InstrumentKind::Fund;

  bool wrote = false, complete = false;
  auto ask = [&](const std::string & period) -> bool {  // false: stop for this edge
    Series series;
    network = true;
    switch (request(listing->orderBook, period, nav, series)) {
      case Outcome::Refused: {
        {
          std::lock_guard lock(mutex_);
          refused_ = true;
          queue_.clear();
        }
        if (message_) message_("history: Nordnet refused the request, stopping");
        idle_.notify_all();
        return false;
      }
      case Outcome::Offline: {
        std::lock_guard lock(mutex_);
        offline_until_ = std::chrono::steady_clock::now() + options_.offline_pause;
        queue_.push_front(edge);  // again afterwards
        return false;
      }
      case Outcome::Missing: {
        std::lock_guard lock(mutex_);
        failed_.insert(edge);
        return false;
      }
      case Outcome::Ok: break;
    }
    if (series.supported && !series.bars.empty()) {
      store_->appendCandles(edge, series.resolution, series.bars);
      wrote = true;
    }
    // Daily bars are what a model wants: the whole period came back coarser, so ask for the longest that gives days.
    complete = series.supported && series.resolution == Resolution::Day;
    return true;
  };

  if (!ask("ALL")) return wrote;
  for (const char * period : kDailyPeriods) {
    if (complete) break;
    if (!ask(period)) return wrote;
    if (options_.pause.count() > 0) {  // between requests of one edge too
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, options_.pause, [&] { return stop_; });
      if (stop_) return wrote;
    }
  }
  std::lock_guard lock(mutex_);
  done_.insert(edge);
  return wrote;
}
