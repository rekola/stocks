#include "Quotes.h"

#include <cstdio>

std::vector<QuoteRow> makeRows(const Snapshot & snapshot, MarketId market, size_t bars) {
  std::vector<QuoteRow> rows;
  for (const EdgeKey & key : snapshot.edges(market)) {
    CandleSeries series = snapshot.lastCandles(key, bars);
    if (series.candles.empty()) continue;  // a book alone has no price to show
    QuoteRow r;
    r.key = key;
    if (auto base = snapshot.instrument(key.base)) {
      r.base = base->symbol;
      r.name = base->name;
    }
    if (auto quote = snapshot.instrument(key.quote)) r.quote = quote->symbol;
    if (auto dataset = snapshot.dataset(key.dataset)) r.dataset = dataset->name;
    for (const Candle & c : series.candles) r.history.push_back(static_cast<float>(c.close));
    r.price = series.candles.back().close;
    r.change = 0;
    if (series.candles.size() > 1) r.change = (r.price / series.candles[series.candles.size() - 2].close - 1.0) * 100.0;
    rows.push_back(std::move(r));
  }
  return rows;
}

std::string fixed(double v, int decimals, bool sign) {
  char buf[32];
  std::snprintf(buf, sizeof buf, sign ? "%+.*f" : "%.*f", decimals, v);
  return buf;
}
