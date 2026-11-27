#include "Quotes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

// What a market is called in a row: its name, else its code.
std::string labelOf(const Snapshot & snapshot, MarketId id) {
  if (std::string name = snapshot.name(id); !name.empty()) return name;
  return marketCode(id);
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
  return snapshot.name(key.base);
}

std::string quoteOf(const Snapshot & snapshot, const EdgeKey & key) {
  return snapshot.symbol(key.quote);
}

// The rows of a market in the order of their keys: the instrument, then the quote. That order is stable
// and costs nothing; a heading sorts by what a person looks for.
// One edge for each group: the first member (the edges are sorted by market, base, quote and dataset, so the members of a group are together).
void appendEdges(const Snapshot & snapshot, MarketId market, std::vector<EdgeKey> & out) {
  const size_t first = out.size();
  for (const EdgeKey & key : snapshot.edges(market)) {
    if (!snapshot.listed(key)) continue;  // no price and no daily queue: a book alone has no row
    if (out.size() > first && groupOf(out.back()) == groupOf(key)) continue;
    out.push_back(key);
  }
}

}  // namespace

std::vector<EdgeKey> quoteEdges(const Snapshot &snapshot, MarketId market,
                                KindSet kinds) {
  std::vector<EdgeKey> keys;
  appendEdges(snapshot, market, keys);
  std::erase_if(keys, [&](const EdgeKey &k) { return !kinds.admits(snapshot.kind(k.base)); });
  return keys;
}

std::vector<EdgeKey> allQuoteEdges(const Snapshot & snapshot) {
  std::vector<EdgeKey> keys;
  for (MarketId m : snapshot.edgeMarkets()) appendEdges(snapshot, m, keys);
  return keys;
}

namespace {

// The digits of a whole number held in a double (every double from 2^53 up is one), without the locale and without a
// printf: the mantissa doubled once for each power of two, in base 10^9.
std::string wholeDigits(double v) {
  int exponent = 0;
  const double fraction = std::frexp(v, &exponent);  // v = fraction * 2^exponent, fraction in [0.5, 1)
  uint64_t mantissa = static_cast<uint64_t>(std::ldexp(fraction, 53));
  exponent -= 53;  // v = mantissa * 2^exponent, and exponent is not negative for a value of 2^53 or more
  constexpr uint32_t kBase = 1'000'000'000;
  std::vector<uint32_t> limbs;  // least significant first
  for (; mantissa > 0; mantissa /= kBase) limbs.push_back(static_cast<uint32_t>(mantissa % kBase));
  for (int i = 0; i < exponent; i++) {
    uint32_t carry = 0;
    for (uint32_t & limb : limbs) {
      const uint64_t doubled = uint64_t{limb} * 2 + carry;
      limb = static_cast<uint32_t>(doubled % kBase);
      carry = static_cast<uint32_t>(doubled / kBase);
    }
    if (carry) limbs.push_back(carry);
  }
  std::string out = std::to_string(limbs.back());
  for (size_t i = limbs.size() - 1; i-- > 0;) {
    const std::string part = std::to_string(limbs[i]);
    out += std::string(9 - part.size(), '0') + part;
  }
  return out;
}

}  // namespace

std::string fixed(double v, int decimals, bool sign) {
  // No value, no text.
  if (!std::isfinite(v)) return "";
  // Whole numbers only: printf's %f writes the locale's decimal point, and a window toolkit sets the locale (a
  // decimal comma then reads back as another number when the table sorts).
  const double magnitude = std::abs(v);
  constexpr double kLimit = 9007199254740992.0;  // 2^53: from here a double is a whole number, and below it times a power of ten is exact enough
  decimals = std::max(decimals, 0);
  long long unit = 1;
  int kept = 0;  // the decimals that fit; the rest of what is asked for is zeros (a value this large has no more to say)
  while (kept < decimals && magnitude * static_cast<double>(unit) * 10 < kLimit) {
    unit *= 10;
    kept++;
  }
  std::string out;
  bool zero = false;
  if (magnitude >= kLimit) {
    out = wholeDigits(magnitude);  // 2^53 or more: a whole number
  } else {
    const long long n = std::llround(magnitude * static_cast<double>(unit));
    zero = n == 0;
    out = std::to_string(n / unit);
    if (kept > 0) {
      const std::string fraction = std::to_string(n % unit);
      out += '.' + std::string(static_cast<size_t>(kept) - fraction.size(), '0') + fraction;
    }
  }
  if (decimals > kept) out += (kept == 0 ? "." : "") + std::string(static_cast<size_t>(decimals - kept), '0');
  if (v < 0 && !zero) out.insert(out.begin(), '-');
  else if (sign) out.insert(out.begin(), '+');
  return out;
}

Quotes::Quotes(std::shared_ptr<const Snapshot> snapshot, std::vector<EdgeKey> keys, std::shared_ptr<IconLoader> icons)
    : icons_(std::move(icons)) {
  reset(std::move(snapshot), std::move(keys));
}

std::string iconSymbolOf(const Snapshot & snapshot, InstrumentId instrument) {
  if (auto symbol = snapshot.attribute(instrument, predicate::iconSymbol)) return asText(*symbol).value_or("");
  switch (snapshot.kind(instrument)) {
    case InstrumentKind::Index: return "📈";
    case InstrumentKind::Currency: return "💱";
    default: return {};
  }
}

std::optional<cellui::Color> iconColorOf(const Snapshot & snapshot, InstrumentId instrument) {
  if (auto color = snapshot.attribute(instrument, predicate::iconColor))
    if (auto text = asText(*color); text && !text->empty()) return cellui::Color(*text);
  return std::nullopt;
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

std::string Quotes::iconFallback(int r, int c) const { return c == ICON ? iconSymbolOf(*snapshot_, edge(r).base) : std::string(); }

std::optional<cellui::Color> Quotes::iconFallbackColor(int r, int c) const { return c == ICON ? iconColorOf(*snapshot_, edge(r).base) : std::nullopt; }

void Quotes::reset(std::shared_ptr<const Snapshot> snapshot, std::vector<EdgeKey> keys) {
  keys_ = std::move(keys);
  groups_.clear();
  for (const EdgeKey & k : keys_) groups_.insert(groupOf(k));
  // `keys` are made from this snapshot; with none, the store's rows are read again when one comes (a market not found yet has none)
  rows_seen_ = keys_.empty() ? 0 : snapshot->quoteRowCount();
  setSnapshot(std::move(snapshot));
}

bool Quotes::addRows(std::shared_ptr<const Snapshot> snapshot, const Scope & scope) {
  bool added = false;
  if (scope.resolved()) {  // until it is, the rows stay unread: the market may be the one they are in
    for (const EdgeKey & key : snapshot->quoteRowsSince(rows_seen_))
      if (scope.has(*snapshot, key) && groups_.insert(groupOf(key)).second) {  // an edge of a group that is a row is no new row
        keys_.push_back(key);
        added = true;
      }
    rows_seen_ = snapshot->quoteRowCount();
  }
  setSnapshot(std::move(snapshot));
  return added;
}

void Quotes::setSnapshot(std::shared_ptr<const Snapshot> snapshot) {
  snapshot_ = std::move(snapshot);
  marketLabels_.clear();
  mixes_.clear();
  traces_.clear();
  for (bool & b : built_) b = false;
}

const std::string & Quotes::marketLabel(MarketId id) const {
  auto it = marketLabels_.find(id.value);
  if (it == marketLabels_.end()) it = marketLabels_.emplace(id.value, labelOf(*snapshot_, id)).first;
  return it->second;
}

const Mix & Quotes::mix(int r) const {
  auto it = mixes_.find(r);
  if (it == mixes_.end()) {
    if (mixes_.size() > 65536) mixes_.clear();
    it = mixes_.emplace(r, mixOf(*snapshot_, group(r), filter_)).first;
  }
  return it->second;
}

std::vector<EdgeKey> Quotes::members(int r) const {
  std::vector<EdgeKey> out;
  for (const Member & m : mix(r).members)
    if (m.state == MemberState::Used) out.push_back(m.edge);
  return out;
}

void Quotes::setFilter(DatasetFilter filter) {
  filter_ = std::move(filter);
  mixes_.clear();
  traces_.clear();
  for (bool & b : built_) b = false;
}

// The price of a row is the datasets' together; none: 0.
double Quotes::price(int r) const {
  const double p = mix(r).price;
  return isDefined(p) ? p : 0;
}

// The members' own numbers for the period (Summary.h) made one: there from the first list a source gave, so a table sorts by it before any
// history is loaded, and the bars keep it up to date.
double Quotes::change(int r) const { return groupChange(*snapshot_, mix(r), period_); }

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
    case PRICE: return isRate(*snapshot_, edge(r)) ? fixed(price(r), 3) + "%" : fixed(price(r), 2);
    case CHANGE: {
      const double v = change(r);
      if (!isDefined(v)) return "";  // no number yet: nothing, not a zero
      if (!isRate(*snapshot_, edge(r))) return fixed(v, 2, true) + "%";
      // A rate: basis points, whole, or one decimal below 10 bp.
      return fixed(v, std::abs(v) < 9.95 ? 1 : 0, true) + " bp";
    }
    default: return "";
  }
}

std::span<const float> Quotes::trace(int r, int) const { return traceOf(r).closes; }
std::span<const float> Quotes::traceLight(int r, int) const { return traceOf(r).light; }
double Quotes::traceSpan(int r, int) const { return traceOf(r).span; }

const PeriodView & Quotes::traceOf(int r) const {
  auto it = traces_.find(r);
  if (it == traces_.end()) {
    if (traces_.size() > 1024) traces_.clear();  // far more than a screen draws at once
    it = traces_.emplace(r, groupPeriodView(*snapshot_, group(r), period_, true, filter_, &mix(r))).first;
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
