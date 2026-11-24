// What start-up costs on a big file (build in Release: cmake -DCMAKE_BUILD_TYPE=Release, then run stocks_restore_bench [facts [edges [dir]]]).
// A synthetic facts file of `facts` facts (25 for each instrument) and, where the store keeps summaries, `edges` edges with a quote, a
// listing, a close before the session and a daily queue of 92 bars each, in `dir` (made once and kept for the next run). Then the store is
// made on them as the program makes it, and this says
//
//   made       how long `makeMemoryStore` took to return: the time until the screen can be shown and the sources can run
//   first row  how long until the first edge is a row of the table (the held ones come first)
//   restored   how long until everything is read
//
// The same file is built against the code before the summaries (STOCKS_RESTORE_BENCH_BASELINE: facts only, read in the constructor).
#include "model/Isin.h"
#include "model/NodeKey.h"
#include "model/SqliteFactStore.h"
#include "model/Store.h"
#ifndef STOCKS_RESTORE_BENCH_BASELINE
#include "model/SqliteSummaryStore.h"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static double since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static std::string isinOf(int n) {
  const std::string digits = std::to_string(n);
  const std::string first11 = "ZZ" + std::string(9 - digits.size(), '0') + digits;
  return first11 + isinCheckDigit(first11);
}

static size_t residentKiB() {
  std::FILE * f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  long size = 0, resident = 0;
  if (std::fscanf(f, "%ld %ld", &size, &resident) != 2) resident = 0;
  std::fclose(f);
  return static_cast<size_t>(resident) * 4;
}

int main(int argc, char ** argv) {
  const int facts = argc > 1 ? std::atoi(argv[1]) : 1'000'000;
  const int edges = argc > 2 ? std::atoi(argv[2]) : 30'000;
  const fs::path dir = argc > 3 ? fs::path(argv[3]) : fs::temp_directory_path() / ("stocks-restore-bench-" + std::to_string(facts) + "-" + std::to_string(edges));
  const int instruments = facts / 25;
  const fs::path factsFile = dir / "facts.sqlite";
  fs::create_directories(dir);
  if (!fs::exists(factsFile)) {
    auto t = Clock::now();
    auto store = openSqliteFactStore(factsFile);
    std::mt19937 rng(1);
    for (int from = 0; from < instruments; from += 2000) {
      FactBatch batch;
      for (int i = from; i < std::min(instruments, from + 2000); i++) {
        const uint64_t node = instrumentKey(isinOf(i))->value;
        auto put = [&](const std::string & predicate, Value v) { batch.changed.push_back(StoredFact{node, predicate, std::move(v), 1'700'000'000'000 + i}); };
        put(predicate::name, "Instrument number " + std::to_string(i));
        put(predicate::symbol, "S" + std::to_string(i));
        put(predicate::isin, isinOf(i));
        put(predicate::kind, std::string(kindName(InstrumentKind::Company)));
        put(predicate::type, std::string("STOCK"));
        put(predicate::country, std::string(i % 2 ? "FI" : "SE"));
        put(predicate::iconUrl, "https://example.invalid/icons/" + std::to_string(i) + ".png");
        put(predicate::webUrl, "https://example.invalid/i/" + std::to_string(i));
        put(predicate::firstTradeDay, static_cast<int64_t>(730000 + i % 3000));
        for (int p = 10; p < 26; p++) {
          if (p % 3 == 0) put("fact-" + std::to_string(p), std::string("text ") + std::to_string(rng() % 500));
          else if (p % 3 == 1) put("fact-" + std::to_string(p), static_cast<int64_t>(rng() % 100000));
          else put("fact-" + std::to_string(p), static_cast<double>(rng() % 100000) / 7.0);
        }
      }
      store->apply(batch);
    }
    FactBatch rest;
    rest.changed.push_back(StoredFact{currencyKey("EUR")->value, predicate::symbol, std::string("EUR"), 1});
    rest.changed.push_back(StoredFact{currencyKey("EUR")->value, predicate::kind, std::string("currency"), 1});
    rest.changed.push_back(StoredFact{marketKey("XHEL")->value, predicate::mic, std::string("XHEL"), 1});
    rest.changed.push_back(StoredFact{marketKey("XHEL")->value, predicate::name, std::string("Nasdaq Helsinki"), 1});
    rest.changed.push_back(StoredFact{datasetKey("nordnet")->value, predicate::name, std::string("Nordnet"), 1});
    store->apply(rest);
    std::printf("made %d facts in %.0f ms\n", instruments * 25, since(t));
  }
#ifndef STOCKS_RESTORE_BENCH_BASELINE
  const fs::path summaries = dir / "summaries";
  if (!fs::exists(summaries / "nordnet.sqlite")) {
    auto t = Clock::now();
    auto store = openSqliteSummaries(summaries)->open("nordnet");
    std::mt19937 rng(2);
    const EdgeKey none{InstrumentId{}, InstrumentId{currencyKey("EUR")->value}, MarketId{marketKey("XHEL")->value}, DatasetId{datasetKey("nordnet")->value}};
    for (int from = 0; from < edges; from += 1000) {
      SummaryBatch batch;
      for (int i = from; i < std::min(edges, from + 1000); i++) {
        EdgeKey k = none;
        k.base = InstrumentId{instrumentKey(isinOf(i % instruments))->value};
        StoredEdge e;
        e.listing = {k, "S" + std::to_string(i), "id" + std::to_string(i), "book" + std::to_string(i), "STOCK", "Instrument number " + std::to_string(i)};
        e.quote = {10.0 + i % 90, 9.9 + i % 90, 10.1 + i % 90, 1'800'000'000};
        e.previous_close = 10.0 + i % 90;
        e.previous_close_day = 740000;
        batch.edges.push_back(e);
        StoredQueue q;
        q.edge = k;
        q.resolution = Resolution::Day;
        q.fetched = 1'800'000'000'000;
        double p = 50;
        for (int j = 0; j < 92; j++) q.bars.push_back(Candle{739900 + j, p, p + 1, p - 1, p += (static_cast<double>(rng() % 200) - 100) / 100, 1000.0 + j});
        batch.queues.push_back(std::move(q));
      }
      store->apply(batch);
    }
    std::printf("made %d edges in %.0f ms\n", edges, since(t));
  }
#endif

  StoreOptions o;
  o.facts = openSqliteFactStore(factsFile);
  o.invent_isins = false;
#ifndef STOCKS_RESTORE_BENCH_BASELINE
  if (!std::getenv("STOCKS_BENCH_NO_SUMMARIES")) o.summaries = openSqliteSummaries(summaries);  // with none, the facts alone are read, all of them
  o.restore = StoreOptions::Restore::InBackground;
#endif
  const auto start = Clock::now();
  auto store = makeMemoryStore(std::move(o));
  const double made = since(start);
  double first = -1;
#ifndef STOCKS_RESTORE_BENCH_BASELINE
  while (store->restoring()) {
    if (first < 0 && store->snapshot()->quoteRowCount() > 0) first = since(start);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  store->waitRestored();
  if (first < 0 && store->snapshot()->quoteRowCount() > 0) first = since(start);
#endif
  const double restored = since(start);
  const auto s = store->snapshot();
  const auto parts = Clock::now();
  const size_t read = openSqliteFactStore(factsFile)->loadAll().size();  // for reference: what reading the facts alone costs
  const double readAll = since(parts);
  std::printf("%d facts, %d edges: made %.0f ms, first row %s ms, restored %.0f ms; %zu instruments, %zu rows, %zu MiB resident\n", instruments * 25, edges, made,
              first < 0 ? "-" : std::to_string(static_cast<int>(first)).c_str(), restored, s->nodeCount(NodeType::Instrument), s->quoteRowCount(), residentKiB() / 1024);
  std::printf("(reading the %zu facts from the file alone: %.0f ms)\n", read, readAll);
  return 0;
}
