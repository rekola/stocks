#include "Quotes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace {

// What a market is called in a row: its name, else Nordnet's id for it (one it has no name for yet), else its number.
std::string labelOf(const Snapshot & snapshot, MarketId id) {
  if (auto m = snapshot.market(id); m && !m->name.empty()) return m->name;
  if (auto v = snapshot.attribute(id, predicate::nordnetId))
    if (auto text = asText(*v)) return "Nordnet " + *text;
  return "#" + std::to_string(id.value);
}

// A currency is its code ("EUR" of EUR/USD); anything else what the market calls it.
std::string baseOf(const Snapshot & snapshot, const EdgeKey & key) {
  auto base = snapshot.instrument(key.base);
  if (!base) return {};
  return base->kind == InstrumentKind::Currency ? base->symbol : snapshot.symbol(key);
}

// What the dataset calls it on that market: "EUR/USD" rather than the euro on every row of a base, and the name each market
// gives an instrument listed on several; the instrument's own where there is none.
std::string nameOf(const Snapshot & snapshot, const EdgeKey & key) {
  if (auto listing = snapshot.listing(key); listing && !listing->name.empty()) return listing->name;
  auto base = snapshot.instrument(key.base);
  return base ? base->name : std::string();
}

std::string quoteOf(const Snapshot & snapshot, const EdgeKey & key) {
  auto quote = snapshot.instrument(key.quote);
  return quote ? quote->symbol : std::string();
}

void appendEdges(const Snapshot & snapshot, MarketId market, std::vector<EdgeKey> & out) {
  for (const EdgeKey & key : snapshot.edges(market))
    if (snapshot.hasCandles(key, Resolution::Day)) out.push_back(key);  // no daily bars: a book alone has no row
}

void appendRows(const Snapshot & snapshot, MarketId market, size_t bars, std::vector<QuoteRow> & rows) {
  const std::string label = labelOf(snapshot, market);
  std::vector<EdgeKey> keys;
  appendEdges(snapshot, market, keys);
  for (const EdgeKey & key : keys) {
    CandleSeries series = snapshot.lastCandles(key, Resolution::Day, bars);  // the table shows daily bars
    QuoteRow r;
    r.key = key;
    r.market = label;
    r.base = baseOf(snapshot, key);
    if (auto url = snapshot.attribute(key.base, predicate::iconUrl)) r.icon_url = asText(*url).value_or("");
    r.name = nameOf(snapshot, key);
    r.quote = quoteOf(snapshot, key);
    if (auto dataset = snapshot.dataset(key.dataset)) r.dataset = dataset->name;
    r.history.reserve(series.candles.size());
    for (const Candle & c : series.candles) r.history.push_back(static_cast<float>(c.close));
    r.price = series.candles.back().close;
    if (series.candles.size() > 1) r.change = (r.price / series.candles[series.candles.size() - 2].close - 1.0) * 100.0;
    rows.push_back(std::move(r));
  }
}

}  // namespace

std::vector<QuoteRow> makeRows(const Snapshot & snapshot, MarketId market, size_t bars) {
  std::vector<QuoteRow> rows;
  appendRows(snapshot, market, bars, rows);
  return rows;
}

std::vector<QuoteRow> makeAllRows(const Snapshot & snapshot, size_t bars) {
  std::vector<QuoteRow> rows;
  for (const Market & m : snapshot.markets()) appendRows(snapshot, m.id, bars, rows);
  return rows;
}

std::vector<EdgeKey> quoteEdges(const Snapshot & snapshot, MarketId market) {
  std::vector<EdgeKey> keys;
  appendEdges(snapshot, market, keys);
  return keys;
}

std::vector<EdgeKey> allQuoteEdges(const Snapshot & snapshot) {
  std::vector<EdgeKey> keys;
  for (const Market & m : snapshot.markets()) appendEdges(snapshot, m.id, keys);
  return keys;
}

std::string fixed(double v, int decimals, bool sign) {
  // Whole numbers only: printf's %f writes the locale's decimal point, and a window toolkit sets the locale (a
  // decimal comma then reads back as another number when the table sorts).
  long long unit = 1;
  for (int i = 0; i < decimals; i++) unit *= 10;
  long long n = std::llround(std::abs(v) * static_cast<double>(unit));
  std::string out = std::to_string(n / unit);
  if (decimals > 0) {
    std::string fraction = std::to_string(n % unit);
    out += '.' + std::string(static_cast<size_t>(decimals) - fraction.size(), '0') + fraction;
  }
  if (v < 0 && n != 0) out.insert(out.begin(), '-');
  else if (sign) out.insert(out.begin(), '+');
  return out;
}

Quotes::Quotes(std::shared_ptr<const Snapshot> snapshot, std::vector<EdgeKey> keys, std::shared_ptr<IconLoader> icons)
    : icons_(std::move(icons)) {
  reset(std::move(snapshot), std::move(keys));
}

std::string Quotes::iconUrl(int r) const {
  auto url = snapshot_->attribute(edge(r).base, predicate::iconUrl);
  return url ? asText(*url).value_or("") : std::string();
}

std::shared_ptr<const cellui::TableIcon> Quotes::icon(int r, int c) const {
  if (!icons_ || c != ICON) return nullptr;
  const std::string url = iconUrl(r);
  return url.empty() ? nullptr : icons_->get(url);
}

void Quotes::reset(std::shared_ptr<const Snapshot> snapshot, std::vector<EdgeKey> keys) {
  snapshot_ = std::move(snapshot);
  keys_ = std::move(keys);
  marketLabels_.clear();
  datasetNames_.clear();
  traces_.clear();
  for (bool & b : built_) b = false;
}

const std::string & Quotes::marketLabel(MarketId id) const {
  auto it = marketLabels_.find(id.value);
  if (it == marketLabels_.end()) it = marketLabels_.emplace(id.value, labelOf(*snapshot_, id)).first;
  return it->second;
}

const std::string & Quotes::datasetName(DatasetId id) const {
  auto it = datasetNames_.find(id.value);
  if (it == datasetNames_.end()) {
    auto d = snapshot_->dataset(id);
    it = datasetNames_.emplace(id.value, d ? d->name : std::string()).first;
  }
  return it->second;
}

Quotes::Latest Quotes::latest(int r) const {
  CandleSeries last = snapshot_->lastCandles(edge(r), Resolution::Day, 2);  // the table shows daily bars
  Latest out;
  if (last.candles.empty()) return out;
  out.price = last.candles.back().close;
  if (last.candles.size() > 1) out.change = (out.price / last.candles[0].close - 1.0) * 100.0;
  return out;
}

std::string Quotes::text(int r, int c) const {
  switch (c) {
    case BASE: return baseOf(*snapshot_, edge(r));
    case QUOTE: return quoteOf(*snapshot_, edge(r));
    case NAME: return nameOf(*snapshot_, edge(r));
    case MARKET: return marketLabel(edge(r).market);
    case DATASET: return datasetName(edge(r).dataset);
    case PRICE: return fixed(latest(r).price, 2);
    case CHANGE: return fixed(latest(r).change, 2, true) + "%";
    default: return "";
  }
}

std::span<const float> Quotes::trace(int r, int) const {
  auto it = traces_.find(r);
  if (it == traces_.end()) {
    if (traces_.size() > 1024) traces_.clear();  // far more than a screen draws at once
    std::vector<float> closes;
    for (const Candle & c : snapshot_->lastCandles(edge(r), Resolution::Day, 60).candles) closes.push_back(static_cast<float>(c.close));
    it = traces_.emplace(r, std::move(closes)).first;
  }
  return it->second;
}

std::optional<cellui::Color> Quotes::color(int r, int c) const {
  bool up;
  if (c == CHANGE) {
    up = latest(r).change >= 0;
  } else if (c == HISTORY) {
    std::span<const float> t = trace(r, c);
    if (t.empty()) return std::nullopt;
    up = t.back() >= t.front();
  } else {
    return std::nullopt;
  }
  return up ? cellui::Color(0x3f, 0xb9, 0x50) : cellui::Color(0xf8, 0x51, 0x49);
}

std::optional<double> Quotes::number(int r, int c) const {
  if (c == PRICE) return latest(r).price;
  if (c == CHANGE) return latest(r).change;
  return std::nullopt;
}

// A sort asks for the same column of every row many times: the column is read once and kept.
void Quotes::build(int column) const {
  const int n = rowCount();
  if (column == PRICE || column == CHANGE) {
    std::vector<double> & v = numbers_[column == PRICE ? 0 : 1];
    v.resize(static_cast<size_t>(n));
    for (int r = 0; r < n; r++) {
      Latest l = latest(r);
      v[static_cast<size_t>(r)] = column == PRICE ? l.price : l.change;
    }
  } else if (column < HISTORY) {
    std::vector<std::string> & v = texts_[column];
    v.resize(static_cast<size_t>(n));
    for (int r = 0; r < n; r++) {
      std::string t = text(r, column);
      for (char & ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      v[static_cast<size_t>(r)] = std::move(t);
    }
  }
  built_[column] = true;
}

int Quotes::compare(int a, int b, int column) const {
  if (column <= ICON || column >= HISTORY) return 0;  // the icon and the trace sort as equal
  if (!built_[column]) build(column);
  if (column == PRICE || column == CHANGE) {
    const std::vector<double> & v = numbers_[column == PRICE ? 0 : 1];
    const double x = v[static_cast<size_t>(a)], y = v[static_cast<size_t>(b)];
    return x < y ? -1 : x > y ? 1 : 0;
  }
  const std::vector<std::string> & v = texts_[column];
  return v[static_cast<size_t>(a)].compare(v[static_cast<size_t>(b)]);
}
