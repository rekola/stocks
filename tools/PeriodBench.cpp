// Measures what a period costs on a table of tens of thousands of rows (build in Release: cmake -DCMAKE_BUILD_TYPE=Release,
// then run stocks_period_bench [rows]). Every row has 60 daily bars; with `full` each has also the series the day, the week and
// the year are drawn from (100 five-minute bars, 45 hourly, 53 weekly), so that no period is measured on a miss.
//
//   build      the Quotes over the listed edges (what showing the preset costs, first draw)
//   refresh    what a store change costs: the edges listed again and the rows reset, unsorted / sorted by Change
//   period     what choosing a period costs: the caches cleared, unsorted / sorted by Change (the sort keys read again)
//   frame      the cells of 50 rows read as a frame does: text, colour, trace of the Change and trace columns
#include "Quotes.h"
#include "model/Dates.h"
#include "model/Isin.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>

using Clock = std::chrono::steady_clock;
// A valid ISIN for the row: ZZ (private use), the number as nine digits and the check digit.
static std::string isinOf(int n) {
  const std::string digits = std::to_string(n);
  const std::string first11 = "ZZ" + std::string(9 - digits.size(), '0') + digits;
  return first11 + isinCheckDigit(first11);
}
static double since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

int main(int argc, char ** argv) {
  const int rows = argc > 1 ? std::atoi(argv[1]) : 50000;
  const bool full = argc > 2 && std::string(argv[2]) == "full";
  auto store = makeMemoryStore();
  Instrument usd = store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  const MarketId market = store->addMarket("Nasdaq Helsinki");
  const DatasetId dataset = store->addDataset("nordnet");
  const int64_t today = dayNumber(2026, 12, 1), dayStart = (today - kUnixEpochDayNumber) * 86400 + 7 * 3600;
  std::mt19937 rng(1);
  std::uniform_real_distribution<double> step(-0.02, 0.021);
  for (int i = 0; i < rows; i++) {
    Instrument inst = store->addInstrument({{}, "", "Stock " + std::to_string(i), InstrumentKind::Company, isinOf(i)});
    EdgeKey k{inst.id, usd.id, market, dataset};
    double p = 50;
    auto series = [&](int n, auto key) {
      std::vector<Candle> bars;
      for (int j = n; j >= 0; j--) bars.push_back({key(j), p, p, p, p *= 1.0 + step(rng), 1000});
      return bars;
    };
    store->appendCandles(k, Resolution::Day, series(60, [&](int j) { return today - j; }));
    if (full) {
      store->appendCandles(k, Resolution::Min5, series(100, [&](int j) { return slotOfInstant(Resolution::Min5, dayStart + (100 - j) * 300); }));
      store->appendCandles(k, Resolution::Hour1, series(45, [&](int j) { return slotOfInstant(Resolution::Hour1, dayStart - j * 3600); }));
      store->appendCandles(k, Resolution::Week, series(53, [&](int j) { return weekStart(today) - 7 * j; }));
      store->setPreviousClose(k, today, p);
    }
    store->setListing({k, "S" + std::to_string(i), "", "book", "ESH", "Stock"});
  }
  auto snapshot = store->snapshot();
  std::printf("%d rows, %s\n", rows, full ? "60 daily + 100 5m + 45 1h + 53 1w bars each" : "60 daily bars each");

  auto t = Clock::now();
  std::vector<EdgeKey> keys = allQuoteEdges(*snapshot);
  const double listing = since(t);
  t = Clock::now();
  Quotes quotes(snapshot, keys);
  std::printf("build (list %.1f ms, rows %.1f ms)\n", listing, since(t));

  // A sort: the order of the rows by a column, as the table does it (the keys read once, then compares).
  auto sorted = [&](int column) {
    std::vector<int> order(static_cast<size_t>(quotes.rowCount()));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return quotes.compare(a, b, column) < 0; });
    return order[0];
  };
  // For reference: the same sort by the price, which no period touches (its keys are read once, then the compares).
  t = Clock::now();
  quotes.reset(snapshot, allQuoteEdges(*snapshot));
  sorted(Quotes::PRICE);
  std::printf("reference: refresh sorted by Price %.1f ms\n", since(t));
  for (size_t p = 0; p < kPeriodCount; p++) {
    const Period period = static_cast<Period>(p);
    // Choosing the period.
    t = Clock::now();
    quotes.setPeriod(period);
    const double choose = since(t);
    t = Clock::now();
    quotes.setPeriod(period);
    sorted(Quotes::CHANGE);
    const double chooseSorted = since(t);
    // A refresh: the edges listed again, the rows reset.
    t = Clock::now();
    quotes.reset(snapshot, allQuoteEdges(*snapshot));
    const double refresh = since(t);
    t = Clock::now();
    quotes.reset(snapshot, allQuoteEdges(*snapshot));
    sorted(Quotes::CHANGE);
    const double refreshSorted = since(t);
    // A frame.
    t = Clock::now();
    size_t sink = 0;
    for (int r = 0; r < 50; r++) {
      sink += quotes.text(r, Quotes::CHANGE).size() + quotes.trace(r, Quotes::HISTORY).size() + quotes.color(r, Quotes::CHANGE).has_value() +
              quotes.color(r, Quotes::HISTORY).has_value() + quotes.text(r, Quotes::PRICE).size();
    }
    const double frame = since(t);
    std::printf("%-7s period %.3f ms, sorted by Change %.1f ms | refresh %.1f ms, sorted by Change %.1f ms | frame of 50 rows %.3f ms (%zu)\n",
                info(period).id, choose, chooseSorted, refresh, refreshSorted, frame, sink);
  }
}
