#include "model/Store.h"

#include <algorithm>
#include <map>

namespace {

// Put `item` into the time-ordered `v`: in place of an element at the same time that `same` calls the same one,
// otherwise after the elements at that time.
template <class T, class Same>
void upsert(std::vector<T> & v, const T & item, Same same) {
  auto byTime = [](const T & a, const T & b) { return a.time < b.time; };
  auto [lo, hi] = std::equal_range(v.begin(), v.end(), item, byTime);
  for (auto it = lo; it != hi; ++it) {
    if (same(*it, item)) {
      *it = item;
      return;
    }
  }
  v.insert(hi, item);
}

template <class T>
std::vector<T> slice(const std::vector<T> & v, TimeRange r) {
  auto lo = std::lower_bound(v.begin(), v.end(), r.from, [](const T & a, Timestamp t) { return a.time < t; });
  auto hi = std::lower_bound(lo, v.end(), r.to, [](const T & a, Timestamp t) { return a.time < t; });
  return {lo, hi};
}

template <class T>
const T * find(const std::vector<T> & v, uint32_t id) {
  return id == 0 || id > v.size() ? nullptr : &v[id - 1];
}

class MemoryStore : public Store {
 public:
  Instrument addInstrument(Instrument i) override {
    for (const Instrument & e : instruments_)
      if (e.symbol == i.symbol) return e;
    i.id.value = static_cast<uint32_t>(instruments_.size() + 1);
    instruments_.push_back(i);
    return i;
  }
  Market addMarket(Market m) override {
    for (const Market & e : markets_)
      if (e.name == m.name) return e;
    m.id.value = static_cast<uint32_t>(markets_.size() + 1);
    markets_.push_back(m);
    return m;
  }
  Dataset addDataset(Dataset d) override {
    for (const Dataset & e : datasets_)
      if (e.name == d.name) return e;
    d.id.value = static_cast<uint32_t>(datasets_.size() + 1);
    datasets_.push_back(d);
    return d;
  }

  std::optional<Instrument> instrument(InstrumentId id) const override { return get(instruments_, id.value); }
  std::optional<Market> market(MarketId id) const override { return get(markets_, id.value); }
  std::optional<Dataset> dataset(DatasetId id) const override { return get(datasets_, id.value); }
  std::optional<Instrument> findInstrument(const std::string & s) const override {
    for (const Instrument & e : instruments_)
      if (e.symbol == s) return e;
    return std::nullopt;
  }
  std::optional<Market> findMarket(const std::string & s) const override {
    for (const Market & e : markets_)
      if (e.name == s) return e;
    return std::nullopt;
  }
  std::optional<Dataset> findDataset(const std::string & s) const override {
    for (const Dataset & e : datasets_)
      if (e.name == s) return e;
    return std::nullopt;
  }
  std::vector<Instrument> instruments() const override { return instruments_; }
  std::vector<Market> markets() const override { return markets_; }
  std::vector<Dataset> datasets() const override { return datasets_; }

  std::vector<EdgeKey> edges(MarketId m) const override {
    std::vector<EdgeKey> out;
    for (const auto & [k, s] : candles_)
      if (k.market == m) out.push_back(k);
    for (const auto & [k, s] : books_)
      if (k.market == m && !candles_.count(k)) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
  }

  bool appendCandles(const EdgeKey & k, int64_t intervalMs, const std::vector<Candle> & in) override {
    if (intervalMs <= 0) return false;
    if (in.empty()) return true;
    CandleSeries & s = candles_[k];
    if (s.intervalMs == 0) s.intervalMs = intervalMs;
    if (s.intervalMs != intervalMs) return false;
    for (const Candle & c : in) upsert(s.candles, c, [](const Candle &, const Candle &) { return true; });
    return true;
  }
  CandleSeries candles(const EdgeKey & k, TimeRange r) const override {
    auto it = candles_.find(k);
    if (it == candles_.end()) return {};
    return {it->second.intervalMs, slice(it->second.candles, r)};
  }
  CandleSeries lastCandles(const EdgeKey & k, size_t n) const override {
    auto it = candles_.find(k);
    if (it == candles_.end()) return {};
    const auto & v = it->second.candles;
    return {it->second.intervalMs, {v.end() - static_cast<std::ptrdiff_t>(std::min(n, v.size())), v.end()}};
  }

  void appendBooks(const EdgeKey & k, const std::vector<BookSnapshot> & in) override {
    auto & v = books_[k];
    for (const BookSnapshot & b : in) upsert(v, b, [](const BookSnapshot &, const BookSnapshot &) { return true; });
  }
  std::vector<BookSnapshot> books(const EdgeKey & k, TimeRange r) const override {
    auto it = books_.find(k);
    return it == books_.end() ? std::vector<BookSnapshot>{} : slice(it->second, r);
  }
  std::optional<BookSnapshot> bookAt(const EdgeKey & k, Timestamp t) const override {
    auto it = books_.find(k);
    if (it == books_.end()) return std::nullopt;
    const auto & v = it->second;
    auto hi = std::upper_bound(v.begin(), v.end(), t, [](Timestamp x, const BookSnapshot & b) { return x < b.time; });
    if (hi == v.begin()) return std::nullopt;
    return *(hi - 1);
  }

  void appendAnnouncements(const EventKey & k, const std::vector<Announcement> & in) override {
    auto & v = announcements_[k];
    for (const Announcement & a : in)
      upsert(v, a, [](const Announcement & x, const Announcement & y) { return x.headline == y.headline; });
  }
  std::vector<Announcement> announcements(const EventKey & k, TimeRange r) const override {
    auto it = announcements_.find(k);
    return it == announcements_.end() ? std::vector<Announcement>{} : slice(it->second, r);
  }
  void appendActions(const EventKey & k, const std::vector<CorporateAction> & in) override {
    auto & v = actions_[k];
    for (const CorporateAction & a : in)
      upsert(v, a, [](const CorporateAction & x, const CorporateAction & y) { return x.kind == y.kind; });
  }
  std::vector<CorporateAction> actions(const EventKey & k, TimeRange r) const override {
    auto it = actions_.find(k);
    return it == actions_.end() ? std::vector<CorporateAction>{} : slice(it->second, r);
  }
  std::vector<EventKey> events(InstrumentId id) const override {
    std::vector<EventKey> out;
    for (const auto & [k, v] : announcements_)
      if (k.instrument == id) out.push_back(k);
    for (const auto & [k, v] : actions_)
      if (k.instrument == id && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
  }

 private:
  template <class T>
  static std::optional<T> get(const std::vector<T> & v, uint32_t id) {
    const T * p = find(v, id);
    return p ? std::optional<T>(*p) : std::nullopt;
  }

  std::vector<Instrument> instruments_;
  std::vector<Market> markets_;
  std::vector<Dataset> datasets_;
  std::map<EdgeKey, CandleSeries> candles_;
  std::map<EdgeKey, std::vector<BookSnapshot>> books_;
  std::map<EventKey, std::vector<Announcement>> announcements_;
  std::map<EventKey, std::vector<CorporateAction>> actions_;
};

}  // namespace

std::unique_ptr<Store> makeMemoryStore() { return std::make_unique<MemoryStore>(); }
