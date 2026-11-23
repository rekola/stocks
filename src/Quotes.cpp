#include "Quotes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

namespace {

// What a market is called in a row: its name, else Nordnet's id for it (one it has no name for yet), else its number.
std::string labelOf(const Snapshot & snapshot, MarketId id) {
  if (auto m = snapshot.market(id); m && !m->name.empty()) return m->name;
  if (auto v = snapshot.attribute(id, predicate::nordnetId))
    if (auto text = asText(*v)) return "Nordnet " + *text;
  return "#" + describe(NodeKey{id.value});
}

// A currency is its code ("EUR" of EUR/USD); anything else what the market calls it.
std::string baseOf(const Snapshot & snapshot, const EdgeKey & key) {
  // Two facts and no view of the instrument: this is read for every row, and to put them in order.
  const auto kind = snapshot.attribute(key.base, predicate::kind);
  if (kind && asText(*kind).value_or("") == kindName(InstrumentKind::Currency))
    return asText(snapshot.attribute(key.base, predicate::symbol).value_or(Value{})).value_or("");
  return snapshot.symbol(key);
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

// The rows of a market in the order of their keys: an instrument's ISIN, then the quote, then the dataset. That order is stable
// and costs nothing; a heading sorts by what a person looks for.
void appendEdges(const Snapshot & snapshot, MarketId market, std::vector<EdgeKey> & out) {
  for (const EdgeKey & key : snapshot.edges(market))
    if (snapshot.hasCandles(key, Resolution::Day)) out.push_back(key);  // no daily bars: a book alone has no row
}

void appendRows(const Snapshot & snapshot, MarketId market, Period period, std::vector<QuoteRow> & rows) {
  const std::string label = labelOf(snapshot, market);
  std::vector<EdgeKey> keys;
  appendEdges(snapshot, market, keys);
  for (const EdgeKey & key : keys) {
    const CandleSeries last = snapshot.lastCandles(key, Resolution::Day, 1);  // the price is the latest daily close
    PeriodView view = periodView(snapshot, key, period, true);
    QuoteRow r;
    r.key = key;
    r.market = label;
    r.base = baseOf(snapshot, key);
    if (auto url = snapshot.attribute(key.base, predicate::iconUrl)) r.icon_url = asText(*url).value_or("");
    r.name = nameOf(snapshot, key);
    r.quote = quoteOf(snapshot, key);
    if (auto dataset = snapshot.dataset(key.dataset)) r.dataset = dataset->name;
    r.history = std::move(view.closes);
    r.price = last.candles.back().close;
    r.change = view.change();
    rows.push_back(std::move(r));
  }
}

}  // namespace

std::vector<QuoteRow> makeRows(const Snapshot & snapshot, MarketId market, Period period) {
  std::vector<QuoteRow> rows;
  appendRows(snapshot, market, period, rows);
  return rows;
}

std::vector<QuoteRow> makeAllRows(const Snapshot & snapshot, Period period) {
  std::vector<QuoteRow> rows;
  for (MarketId m : snapshot.edgeMarkets()) appendRows(snapshot, m, period, rows);
  return rows;
}

std::vector<EdgeKey> quoteEdges(const Snapshot & snapshot, MarketId market) {
  std::vector<EdgeKey> keys;
  appendEdges(snapshot, market, keys);
  return keys;
}

std::vector<EdgeKey> allQuoteEdges(const Snapshot & snapshot) {
  std::vector<EdgeKey> keys;
  for (MarketId m : snapshot.edgeMarkets()) appendEdges(snapshot, m, keys);
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
  keys_ = std::move(keys);
  setSnapshot(std::move(snapshot));
}

bool Quotes::follow(std::shared_ptr<const Snapshot> snapshot, const std::vector<EdgeKey> & edges) {
  std::vector<EdgeKey> ordered;
  ordered.reserve(edges.size());
  const std::set<EdgeKey> wanted(edges.begin(), edges.end());
  std::set<EdgeKey> placed;
  for (const EdgeKey & key : keys_)
    if (wanted.count(key) && placed.insert(key).second) ordered.push_back(key);
  for (const EdgeKey & key : edges)
    if (placed.insert(key).second) ordered.push_back(key);
  const bool changed = ordered != keys_;
  if (changed) keys_ = std::move(ordered);
  setSnapshot(std::move(snapshot));
  return changed;
}

void Quotes::setSnapshot(std::shared_ptr<const Snapshot> snapshot) {
  snapshot_ = std::move(snapshot);
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

double Quotes::price(int r) const {
  CandleSeries last = snapshot_->lastCandles(edge(r), Resolution::Day, 1);  // the price is the latest daily close
  return last.candles.empty() ? 0 : last.candles.back().close;
}

double Quotes::change(int r) const { return periodView(*snapshot_, edge(r), period_, false).change(); }

void Quotes::setPeriod(Period period) {
  period_ = period;
  traces_.clear();
  built_[CHANGE] = false;  // the change is another number; every other sort key stands
}

std::string changeTitle(Period period) { return std::string("Change ") + info(period).label; }

std::string Quotes::text(int r, int c) const {
  switch (c) {
    case BASE: return baseOf(*snapshot_, edge(r));
    case QUOTE: return quoteOf(*snapshot_, edge(r));
    case NAME: return nameOf(*snapshot_, edge(r));
    case MARKET: return marketLabel(edge(r).market);
    case DATASET: return datasetName(edge(r).dataset);
    case PRICE: return fixed(price(r), 2);
    case CHANGE: {
      const double v = change(r);
      return isDefined(v) ? fixed(v, 2, true) + "%" : "";  // no number yet: nothing, not a zero
    }
    default: return "";
  }
}

std::span<const float> Quotes::trace(int r, int) const {
  auto it = traces_.find(r);
  if (it == traces_.end()) {
    if (traces_.size() > 1024) traces_.clear();  // far more than a screen draws at once
    it = traces_.emplace(r, periodView(*snapshot_, edge(r), period_, true).closes).first;
  }
  return it->second;
}

// The change and the trace beside it are one colour, the sign of the period's change (trendUp: the only place the rule is).
std::optional<cellui::Color> Quotes::color(int r, int c) const {
  if (c != CHANGE && c != HISTORY) return std::nullopt;
  const std::optional<bool> up = trendUp(change(r));
  if (!up) return std::nullopt;
  return *up ? cellui::Color(0x3f, 0xb9, 0x50) : cellui::Color(0xf8, 0x51, 0x49);
}

std::optional<double> Quotes::number(int r, int c) const {
  if (c == PRICE) return price(r);
  if (c == CHANGE) {
    const double v = change(r);
    return isDefined(v) ? std::optional<double>(v) : std::nullopt;
  }
  return std::nullopt;
}

// A sort asks for the same column of every row many times: the column is read once and kept.
void Quotes::build(int column) const {
  const int n = rowCount();
  if (column == PRICE || column == CHANGE) {
    std::vector<double> & v = numbers_[column == PRICE ? 0 : 1];
    v.resize(static_cast<size_t>(n));
    for (int r = 0; r < n; r++) {
      v[static_cast<size_t>(r)] = column == PRICE ? price(r) : change(r);
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
    if (!isDefined(x) || !isDefined(y)) return isDefined(x) - isDefined(y);  // rows with no number sort before every number
    return x < y ? -1 : x > y ? 1 : 0;
  }
  const std::vector<std::string> & v = texts_[column];
  return v[static_cast<size_t>(a)].compare(v[static_cast<size_t>(b)]);
}
