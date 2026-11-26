#include "TestFramework.h"
#include "MarketTestSupport.h"
#include "market/Store.h"
#include "model/NodeKey.h"

#include <SQLiteCpp/SQLiteCpp.h>

using namespace market;
using market::Level;

namespace {

Edge edgeOf(const char * base, const char * quote) {
  return {currencyKey(base)->value, currencyKey(quote)->value, marketKey("XXXX")->value, datasetKey("HISTDATA")->value};
}

// What every store has to do, run on each.
void storeContract(MarketStore & store) {
  CHECK(store.healthy());
  CHECK(store.instruments().empty());
  const uint64_t dataset = store.putDataset("HISTDATA", "HistData");
  CHECK(dataset == datasetKey("HISTDATA")->value);
  CHECK(store.putDataset("HISTDATA", "HistData again") == dataset);  // the same dataset
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
  store.replaceBars(eur, Level::Week, bars, 123456, v1);
  store.replaceBars(gbp, Level::Week, {{7, 5, 6, 4, 5.5}}, 777, v1);
  CHECK(store.bars(eur, Level::Week, 0, 1000) == bars);  // doubles come back exactly
  CHECK(store.bars(eur, Level::Week, 103, 105).size() == 3);
  CHECK(store.bars(eur, Level::Year, 0, 1000).empty());  // another level
  CHECK(store.bars(gbp, Level::Week, 0, 1000).size() == 1);

  // replaced as a whole: nothing of the old remains
  store.replaceBars(eur, Level::Week, {{200, 1, 2, 0.5, 1.5}}, 999, v2);
  CHECK(store.bars(eur, Level::Week, 0, 1000).size() == 1);
  store.replaceBars(eur, Level::Year, {{734534, 1, 1, 1, 1}}, 999, v2);

  all = store.instruments();
  CHECK(all.size() == 2 && all[0].levels.size() == 2);
  if (all.size() == 2 && all[0].levels.size() == 2) {
    const LevelState & h = all[0].levels[0];
    CHECK(h.level == Level::Week && h.version == v2 && h.barCount == 1 && h.firstKey == 200 && h.lastKey == 200 && h.lastBarTime == 999);
    CHECK(all[0].levels[1].level == Level::Year);
    CHECK(all[1].levels.size() == 1 && all[1].levels[0].lastBarTime == 777);
  }
  // a streamed read hands the bars over in order, and a range cuts it
  std::vector<int64_t> keys;
  store.forEachBar(eur, Level::Week, 0, 1000, [&](const Bar & b) { keys.push_back(b.key); });
  CHECK(keys == std::vector<int64_t>{200});
  // forgetting an instrument takes its bars and loads with it
  store.removeInstrument(eur);
  CHECK(store.instruments().size() == 1 && store.bars(eur, Level::Week, 0, 1000).empty() && store.bars(eur, Level::Year, 0, 1000).empty());
  CHECK(store.bars(gbp, Level::Week, 0, 1000).size() == 1);
  store.removeInstrument(eur);  // not there: nothing
  std::vector<Bar> many;
  for (int64_t k = 0; k < 3650; k++) many.push_back({734000 + k, 1, 2, 0.5, 1.5});
  store.replaceBars(gbp, Level::Year, many, 5, v2);
  CHECK(store.bars(gbp, Level::Year, 0, INT64_MAX).size() == 3650);
}

}  // namespace

TEST(market_stores_keep_their_contract_in_memory_in_sqlite_and_in_mariadb) {
  forEachStore([](MarketStore & store, const char * backend) {
    std::printf("  %s\n", backend);
    storeContract(store);
  });
}

TEST(market_sqlite_data_is_kept_between_opens_and_read_only_open_needs_the_schema) {
  ScratchSqlite db;
  bool refused = false;
  try {
    openStore(db.config, OpenMode::ReadOnly);  // no file
  } catch (const std::exception &) {
    refused = true;
  }
  CHECK(refused);
  {
    auto store = openStore(db.config, OpenMode::ReadWrite);
    store->putInstrument({edgeOf("EUR", "USD"), "eurusd", "EUR/USD", "currency", 200005});
    store->replaceBars(edgeOf("EUR", "USD"), Level::Year, {{1, 1.5, 2.5, 0.5, 1.25}}, 7, 1);
  }
  std::vector<std::string> log;
  auto again = openStore(db.config, OpenMode::ReadWrite, [&](const std::string & l) { log.push_back(l); });
  CHECK(again->instruments().size() == 1 && log.empty());
  again.reset();
  auto readOnly = openStore(db.config, OpenMode::ReadOnly);
  CHECK(readOnly->instruments().size() == 1);
  const auto bars = readOnly->bars(edgeOf("EUR", "USD"), Level::Year, 0, 10);
  CHECK(bars.size() == 1 && bars[0] == (Bar{1, 1.5, 2.5, 0.5, 1.25}));
}

TEST(market_a_store_of_another_schema_version_starts_over_and_a_read_only_open_refuses_it) {
  ScratchSqlite db;
  {
    auto store = openStore(db.config, OpenMode::ReadWrite);
    store->putInstrument({edgeOf("EUR", "USD"), "eurusd", "EUR/USD", "currency", 200005});
  }
  {
    SQLite::Database file(db.config.path, SQLite::OPEN_READWRITE);
    file.exec("UPDATE meta SET value = '1' WHERE name = 'schema_version'");
  }
  bool refused = false;
  try {
    openStore(db.config, OpenMode::ReadOnly);
  } catch (const std::exception & e) {
    refused = std::string(e.what()).find("version 1") != std::string::npos;
  }
  CHECK(refused);
  std::vector<std::string> log;
  auto store = openStore(db.config, OpenMode::ReadWrite, [&](const std::string & l) { log.push_back(l); });
  CHECK(store->instruments().empty());
  CHECK(log.size() == 1 && log[0].find("starting over") != std::string::npos);
}

TEST(market_batches_count_up_across_opens) {
  ScratchSqlite db;
  uint64_t first = 0;
  {
    auto store = openStore(db.config, OpenMode::ReadWrite);
    const uint64_t dataset = store->putDataset("HISTDATA", "HistData");
    first = store->beginBatch(dataset, "one");
    CHECK(store->beginBatch(dataset, "two") == first + 1);
  }
  auto store = openStore(db.config, OpenMode::ReadWrite);
  CHECK(store->beginBatch(datasetKey("HISTDATA")->value, "three") == first + 2);
}
