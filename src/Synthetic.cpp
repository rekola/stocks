#include "Synthetic.h"

#include "model/Dates.h"
#include "source/InstrumentLists.h"

#include <random>

void fillSyntheticData(Store & store, const InstrumentList & list) {
  const int64_t kStart = dayNumber(2026, 1, 1);  // the first trading date: a day number
  Dataset synthetic = store.addDataset({{}, "synthetic"});
  std::string market;
  unsigned preset = 0;
  std::mt19937 rng;
  std::uniform_real_distribution<double> step(-0.03, 0.032), start(1.5, 300.0), wick(0.0, 0.01), volume(1e3, 1e6);
  for (const ListEntry & e : list) {
    if (market != e.market) {  // one random walk sequence per market, the same every run
      market = e.market;
      rng.seed(preset++ * 7919u + 1u);
    }
    Market m = store.addMarket({{}, e.market});
    Instrument base = store.addInstrument({{}, e.base, e.name, e.kind});
    Instrument quote = currencyNode(store, e.quote);
    std::vector<Candle> bars;
    double p = start(rng);
    for (int d = 0; d < 60; d++) {
      double open = p;
      p *= 1.0 + step(rng);
      bars.push_back({kStart + d, open, std::max(open, p) * (1 + wick(rng)), std::min(open, p) * (1 - wick(rng)), p,
                      volume(rng)});
    }
    store.appendCandles({base.id, quote.id, m.id, synthetic.id}, Resolution::Day, bars);
  }
}
