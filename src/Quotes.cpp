#include "Quotes.h"

#include <cmath>
#include <cstdlib>

std::vector<QuoteRow> makeRows(const Snapshot & snapshot, MarketId market, size_t bars) {
  std::vector<QuoteRow> rows;
  for (const EdgeKey & key : snapshot.edges(market)) {
    CandleSeries series = snapshot.lastCandles(key, Resolution::Day, bars);  // the table shows daily bars
    if (series.candles.empty()) continue;  // no daily bars: a book alone, or only intraday, has no row
    QuoteRow r;
    r.key = key;
    if (auto base = snapshot.instrument(key.base)) {
      r.base = snapshot.symbol(key);
      r.name = base->name;
    }
    if (auto quote = snapshot.instrument(key.quote)) r.quote = quote->symbol;
    if (auto dataset = snapshot.dataset(key.dataset)) r.dataset = dataset->name;
    for (const Candle & c : series.candles) r.history.push_back(static_cast<float>(c.close));
    r.price = series.candles.back().close;
    if (series.candles.size() > 1) r.change = (r.price / series.candles[series.candles.size() - 2].close - 1.0) * 100.0;
    rows.push_back(std::move(r));
  }
  return rows;
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

std::string Quotes::text(int r, int c) const {
  const QuoteRow & i = at(r);
  switch (c) {
    case BASE: return i.base;
    case QUOTE: return i.quote;
    case NAME: return i.name;
    case DATASET: return i.dataset;
    case PRICE: return fixed(i.price, 2);
    case CHANGE: return fixed(i.change, 2, true) + "%";
    default: return "";
  }
}

std::optional<cellui::Color> Quotes::color(int r, int c) const {
  const QuoteRow & i = at(r);
  bool up;
  if (c == CHANGE) up = i.change >= 0;
  else if (c == HISTORY && !i.history.empty()) up = i.history.back() >= i.history.front();
  else return std::nullopt;
  return up ? cellui::Color(0x3f, 0xb9, 0x50) : cellui::Color(0xf8, 0x51, 0x49);
}

std::optional<double> Quotes::number(int r, int c) const {
  if (c == PRICE) return at(r).price;
  if (c == CHANGE) return at(r).change;
  return std::nullopt;
}
