#include "TestFramework.h"
#include "MarketTestSupport.h"
#include "market/Store.h"
#include "model/NodeKey.h"

using namespace market;

namespace {

Edge edgeOf(const char * base, const char * quote) {
  return {currencyKey(base)->value, currencyKey(quote)->value, marketKey("XXXX")->value, datasetKey("histdata")->value};
}

// What every store has to do, run on each.
void storeContract(MarketStore & store) {
  CHECK(store.healthy());
  CHECK(store.instruments().empty());
  const uint64_t dataset = store.putDataset("histdata", "HistData");
  CHECK(dataset == datasetKey("histdata")->value);
  CHECK(store.putDataset("histdata", "HistData again") == dataset);  // the same dataset
  const uint64_t v1 = store.beginBatch(dataset, "first"), v2 = store.beginBatch(dataset, "second");
  CHECK(v1 > 0 && v2 > v1);

  const Edge eur = edgeOf("EUR", "USD"), gbp = edgeOf("GBP", "USD");
  store.putInstrument({eur, "eurusd", "EUR/USD", "currency", 200005});
  store.putInstrument({gbp, "gbpusd", "GBP/USD", "currency", 200005});
  store.putInstrument({eur, "eurusd", "EUR/USD", "currency", 200006});  // said again: one instrument, the latest said
  auto all = store.instruments();
  CHECK(all.size() == 2);
  CHECK(all.size() == 2 && all[0].instrument.code == "eurusd" && all[0].instrument.firstPeriod == 200006 && all[0].levels.empty());
  CHECK(all.size() == 2 && all[1].instrument.code == "gbpusd");

  std::vector<Bar> bars;
  for (int64_t k = 100; k < 110; k++) bars.push_back({k, 1.0 + k * 1e-7, 2.0 / 3.0, 0.1 + 0.2, 1e-9 * k});
  store.replaceBars(eur, Resolution::Hour1, bars, 123456, v1);
  store.replaceBars(gbp, Resolution::Hour1, {{7, 5, 6, 4, 5.5}}, 777, v1);
  CHECK(store.bars(eur, Resolution::Hour1, 0, 1000) == bars);  // doubles come back exactly
  CHECK(store.bars(eur, Resolution::Hour1, 103, 105).size() == 3);
  CHECK(store.bars(eur, Resolution::Day, 0, 1000).empty());  // another level
  CHECK(store.bars(gbp, Resolution::Hour1, 0, 1000).size() == 1);

  // replaced as a whole: nothing of the old remains
  store.replaceBars(eur, Resolution::Hour1, {{200, 1, 2, 0.5, 1.5}}, 999, v2);
  CHECK(store.bars(eur, Resolution::Hour1, 0, 1000).size() == 1);
  store.replaceBars(eur, Resolution::Day, {{734534, 1, 1, 1, 1}}, 999, v2);

  all = store.instruments();
  CHECK(all.size() == 2 && all[0].levels.size() == 2);
  if (all.size() == 2 && all[0].levels.size() == 2) {
    const LevelState & h = all[0].levels[0];
    CHECK(h.level == Resolution::Hour1 && h.version == v2 && h.barCount == 1 && h.firstKey == 200 && h.lastKey == 200 && h.lastBarTime == 999);
    CHECK(all[0].levels[1].level == Resolution::Day);
    CHECK(all[1].levels.size() == 1 && all[1].levels[0].lastBarTime == 777);
  }
  std::vector<Bar> many;
  for (int64_t k = 0; k < 3650; k++) many.push_back({734000 + k, 1, 2, 0.5, 1.5});
  store.replaceBars(gbp, Resolution::Day, many, 5, v2);
  CHECK(store.bars(gbp, Resolution::Day, 0, INT64_MAX).size() == 3650);
}

}  // namespace

TEST(market_memory_store_keeps_its_contract) {
  auto store = makeMemoryStore();
  storeContract(*store);
}

TEST(market_mariadb_store_keeps_the_same_contract) {
  ScratchDb db;
  if (!db.available) return skipWithoutDatabase();
  auto store = openMariaDbStore(db.config, OpenMode::ReadWrite);
  storeContract(*store);
}

TEST(market_mariadb_data_is_kept_between_opens_and_read_only_open_needs_the_schema) {
  ScratchDb db;
  if (!db.available) return skipWithoutDatabase();
  bool refused = false;
  try {
    openMariaDbStore(db.config, OpenMode::ReadOnly);  // nothing there yet
  } catch (const std::exception &) {
    refused = true;
  }
  CHECK(refused);
  {
    auto store = openMariaDbStore(db.config, OpenMode::ReadWrite);
    store->putInstrument({edgeOf("EUR", "USD"), "eurusd", "EUR/USD", "currency", 200005});
  }
  std::vector<std::string> log;
  auto again = openMariaDbStore(db.config, OpenMode::ReadWrite, [&](const std::string & l) { log.push_back(l); });
  CHECK(again->instruments().size() == 1 && log.empty());
  CHECK(openMariaDbStore(db.config, OpenMode::ReadOnly)->instruments().size() == 1);
}
