#include "TestFramework.h"
#include "model/SqliteBarStore.h"
#include "model/Store.h"

#include <filesystem>
#include <unistd.h>

namespace {

struct TempFile {
  std::filesystem::path path;
  TempFile() {
    static int n = 0;
    path = std::filesystem::temp_directory_path() / ("stocks_bars_" + std::to_string(getpid()) + "_" + std::to_string(n++) + ".sqlite");
  }
  ~TempFile() {
    std::error_code ec;
    for (const char * suffix : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + suffix, ec);
  }
};

EdgeKey someEdge() {
  return EdgeKey{InstrumentId{1}, InstrumentId{2}, MarketId{3}, DatasetId{4}};
}

std::vector<Candle> readAll(BarStore & store, const EdgeKey & e, Resolution r, int64_t first = 0, int64_t last = INT64_MAX) {
  std::vector<Candle> out;
  store.read(e, r, first, last, [&](const Candle & c) { out.push_back(c); });
  return out;
}

}  // namespace

TEST(a_bar_store_puts_bars_in_place_of_those_with_the_same_time_and_reads_a_range_in_order) {
  TempFile file;
  auto store = openSqliteBarStore(file.path);
  const EdgeKey e = someEdge();
  BarBatch batch;
  batch.series.push_back({e, Resolution::Day, {{10, 1, 2, 0.5, 1.5, 100}, {11, 2, 3, 1.5, 2.5, kUndefined}, {12, 3, 4, 2.5, 3.5, 0}}});
  store->apply(batch);
  BarBatch again;
  again.series.push_back({e, Resolution::Day, {{11, 20, 30, 15, 25, 7}, {13, 4, 5, 3, 4, 1}}});
  store->apply(again);
  const auto all = readAll(*store, e, Resolution::Day);
  CHECK(all.size() == 4);
  CHECK(all.size() == 4 && all[0].time == 10 && all[1].time == 11 && all[2].time == 12 && all[3].time == 13);
  CHECK(all.size() == 4 && all[1].open == 20 && all[1].volume == 7 && all[0].volume == 100);
  CHECK(readAll(*store, e, Resolution::Day, 11, 12).size() == 2);
  CHECK(readAll(*store, e, Resolution::Hour1).empty());
  CHECK(readAll(*store, EdgeKey{InstrumentId{9}, InstrumentId{2}, MarketId{3}, DatasetId{4}}, Resolution::Day).empty());
}

TEST(a_bar_store_keeps_what_it_was_given_between_opens) {
  TempFile file;
  const EdgeKey e = someEdge();
  {
    auto store = openSqliteBarStore(file.path);
    BarBatch batch;
    batch.series.push_back({e, Resolution::Min5, {{5, 1.1, 1.2, 1.0, 1.15, kUndefined}}});
    store->apply(batch);
  }
  auto store = openSqliteBarStore(file.path);
  const auto bars = readAll(*store, e, Resolution::Min5);
  CHECK(bars.size() == 1 && bars[0].open == 1.1 && bars[0].close == 1.15 && !isDefined(bars[0].volume));
}

TEST(the_store_writes_every_bar_it_is_given_to_its_bar_store_and_only_the_ones_that_changed) {
  TempFile file;
  auto bars = openSqliteBarStore(file.path);
  StoreOptions options;
  options.bars = bars;
  options.bar_writer.delay = std::chrono::milliseconds(0);
  auto store = makeMemoryStore(options);
  EdgeKey e;
  store->batch([&](Batch & b) {
    const DatasetId dataset = b.addDataset("TESTDS", "Test");
    const MarketId market = b.addMarket("XXXX", "none");
    const InstrumentId base = b.addInstrument("EUR", "Euro", InstrumentKind::Currency);
    const InstrumentId quote = b.addInstrument("USD", "US dollar", InstrumentKind::Currency);
    e = EdgeKey{base, quote, market, dataset};
    b.setListing({e, "EURUSD", "", "", "", "EUR/USD"});
    // many more bars than a queue of the day resolution keeps: the bar store has them all
    std::vector<Candle> days;
    for (int64_t d = 0; d < 1000; d++) days.push_back({734000 + d, 1.0 + d, 2.0 + d, 0.5 + d, 1.5 + d, kUndefined});
    b.appendCandles(e, Resolution::Day, days);
  });
  store->flush();
  CHECK(readAll(*bars, e, Resolution::Day).size() == 1000);
  store->batch([&](Batch & b) { b.appendCandles(e, Resolution::Day, {{734999, 9, 9, 9, 9, kUndefined}, {735000, 1, 1, 1, 1, kUndefined}}); });
  store->flush();
  const auto all = readAll(*bars, e, Resolution::Day);
  CHECK(all.size() == 1001);
  CHECK(all.size() == 1001 && all[999].time == 734999 && all[999].open == 9 && all[1000].time == 735000);
}

namespace {

struct ThrowingBars : BarStore {
  void apply(const BarBatch &) override { throw std::runtime_error("the medium is full"); }
  void read(const EdgeKey &, Resolution, int64_t, int64_t, const std::function<void(const Candle &)> &) override {}
};

struct ThrowingFacts : FactStore {
  std::vector<StoredFact> loadAll() override { return {}; }
  void apply(const FactBatch &) override { throw std::runtime_error("the medium is full"); }
};

struct ThrowingSummary : SummaryStore {
  std::vector<StoredEdge> loadEdges() override { return {}; }
  std::optional<StoredQueue> readQueue(const EdgeKey &, Resolution) override { return std::nullopt; }
  void apply(const SummaryBatch &) override { throw std::runtime_error("the medium is full"); }
};

struct ThrowingSummaries : SummaryStores {
  std::vector<std::string> datasets() override { return {}; }
  std::shared_ptr<SummaryStore> open(const std::string &) override { return std::make_shared<ThrowingSummary>(); }
};

void sayABar(Store & store) {
  store.batch([&](Batch & b) {
    const DatasetId dataset = b.addDataset("TESTDS", "Test");
    const MarketId market = b.addMarket("XXXX", "none");
    const InstrumentId base = b.addInstrument("EUR", "Euro", InstrumentKind::Currency);
    const InstrumentId quote = b.addInstrument("USD", "US dollar", InstrumentKind::Currency);
    const EdgeKey e{base, quote, market, dataset};
    b.setListing({e, "EURUSD", "", "", "", "EUR/USD"});
    b.appendCandles(e, Resolution::Day, {{734000, 1, 2, 0.5, 1.5, kUndefined}});
  });
  store.flush();
}

}  // namespace

TEST(a_store_counts_the_batches_its_bar_summary_and_fact_stores_throw_on) {
  {
    StoreOptions options;
    options.bar_writer.delay = std::chrono::milliseconds(0);
    auto healthy = makeMemoryStore(options);
    sayABar(*healthy);
    CHECK(healthy->failedWrites() == 0);
  }
  {
    StoreOptions options;
    options.bars = std::make_shared<ThrowingBars>();
    options.bar_writer.delay = std::chrono::milliseconds(0);
    auto store = makeMemoryStore(options);
    sayABar(*store);
    CHECK(store->failedWrites() >= 1);
  }
  {
    StoreOptions options;
    options.summaries = std::make_shared<ThrowingSummaries>();
    options.summary_writer.delay = std::chrono::milliseconds(0);
    options.summary_writer.queue_delay = std::chrono::milliseconds(0);
    auto store = makeMemoryStore(options);
    sayABar(*store);
    CHECK(store->failedWrites() >= 1);
  }
  {
    StoreOptions options;
    options.facts = std::make_shared<ThrowingFacts>();
    options.writer.changed_delay = std::chrono::milliseconds(0);
    auto store = makeMemoryStore(options);
    sayABar(*store);
    CHECK(store->failedWrites() >= 1);
  }
}
