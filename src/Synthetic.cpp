#include "Synthetic.h"

#include <random>

namespace {

struct Seed {
  const char *base, *quote, *name;
};
struct MarketSeeds {
  const char * name;
  InstrumentKind kind;  // of the base
  std::vector<Seed> seeds;
};

const std::vector<MarketSeeds> & marketSeeds() {
  static const std::vector<MarketSeeds> m = {
    {"Commodities", InstrumentKind::Commodity,
     {{"XAU", "USD", "Gold"}, {"XAG", "USD", "Silver"}, {"BRENT", "USD", "Brent Crude"}, {"WTI", "USD", "WTI Crude"},
      {"HG", "USD", "Copper"}, {"NG", "USD", "Natural Gas"}, {"XPT", "USD", "Platinum"}, {"ZW", "USD", "Wheat"},
      {"ZC", "USD", "Corn"}, {"KC", "USD", "Coffee"}, {"SB", "USD", "Sugar"}, {"CT", "USD", "Cotton"}}},
    {"Nasdaq Helsinki", InstrumentKind::Company,
     {{"NOKIA", "EUR", "Nokia"}, {"FORTUM", "EUR", "Fortum"}, {"SAMPO", "EUR", "Sampo"}, {"KNEBV", "EUR", "Kone"},
      {"UPM", "EUR", "UPM-Kymmene"}, {"NESTE", "EUR", "Neste"}, {"STERV", "EUR", "Stora Enso"},
      {"WRT1V", "EUR", "Wartsila"}, {"ELISA", "EUR", "Elisa"}, {"ORNBV", "EUR", "Orion"}, {"NDA FI", "EUR", "Nordea"},
      {"METSO", "EUR", "Metso"}}},
    {"Currencies", InstrumentKind::Currency,
     {{"EUR", "USD", "Euro"}, {"USD", "JPY", "US dollar"}, {"GBP", "USD", "British pound"},
      {"USD", "CHF", "US dollar"}, {"AUD", "USD", "Australian dollar"}, {"USD", "CAD", "US dollar"},
      {"EUR", "SEK", "Euro"}, {"EUR", "GBP", "Euro"}, {"USD", "NOK", "US dollar"}, {"EUR", "CHF", "Euro"},
      {"NZD", "USD", "New Zealand dollar"}, {"USD", "CNY", "US dollar"}}}};
  return m;
}

// The names of the currencies that only ever appear as the quote.
const char * currencyName(const std::string & symbol) {
  static const std::pair<const char *, const char *> names[] = {
    {"USD", "US dollar"}, {"EUR", "Euro"}, {"JPY", "Japanese yen"}, {"CHF", "Swiss franc"}, {"CAD", "Canadian dollar"},
    {"SEK", "Swedish krona"}, {"GBP", "British pound"}, {"NOK", "Norwegian krone"}, {"CNY", "Chinese yuan"}};
  for (const auto & [s, n] : names)
    if (symbol == s) return n;
  return "";
}

}  // namespace

void fillSyntheticData(Store & store) {
  constexpr Timestamp kDay = 86400000, kStart = 1767225600000;  // 2026-01-01 00:00 UTC
  Dataset synthetic = store.addDataset({{}, "synthetic"});
  unsigned preset = 0;
  for (const MarketSeeds & m : marketSeeds()) {
    Market market = store.addMarket({{}, m.name});
    std::mt19937 rng(preset++ * 7919u + 1u);
    std::uniform_real_distribution<double> step(-0.03, 0.032), start(1.5, 300.0), wick(0.0, 0.01), volume(1e3, 1e6);
    for (const Seed & seed : m.seeds) {
      Instrument base = store.addInstrument({{}, seed.base, seed.name, m.kind});
      Instrument quote = store.addInstrument({{}, seed.quote, currencyName(seed.quote), InstrumentKind::Currency});
      std::vector<Candle> bars;
      double p = start(rng);
      for (int d = 0; d < 60; d++) {
        double open = p;
        p *= 1.0 + step(rng);
        bars.push_back({kStart + d * kDay, open, std::max(open, p) * (1 + wick(rng)), std::min(open, p) * (1 - wick(rng)), p,
                        volume(rng)});
      }
      store.appendCandles({base.id, quote.id, market.id, synthetic.id}, kDay, bars);
    }
  }
}
