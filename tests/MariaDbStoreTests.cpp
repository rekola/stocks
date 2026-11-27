#include "TestFramework.h"
#include "model/MariaDbStores.h"
#include "model/NodeKey.h"
#include "model/SqliteBarStore.h"
#include "model/SqliteFactStore.h"
#include "model/SqliteSummaryStore.h"
#include "model/Store.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>

// The MariaDB stores against the SQLite ones: the same cases on each. They run where MARKET_TEST_DB_USER names a user that may make and drop
// schemas named stocks_test_* (with MARKET_TEST_DB_PASSWORD, _HOST, _PORT and _SOCKET as needed), and say so where it is not set.

namespace {

struct ScratchSchema {
  maria::Config config;
  bool available = false;
  ScratchSchema() {
    const char * user = std::getenv("MARKET_TEST_DB_USER");
    if (!user) return;
    available = true;
    config.user = user;
    if (const char * v = std::getenv("MARKET_TEST_DB_PASSWORD")) config.password = v;
    if (const char * v = std::getenv("MARKET_TEST_DB_HOST")) config.host = v;
    if (const char * v = std::getenv("MARKET_TEST_DB_PORT")) config.port = std::atoi(v);
    if (const char * v = std::getenv("MARKET_TEST_DB_SOCKET")) config.socket = v;
    static int n = 0;
    config.database = "stocks_test_" + std::to_string(getpid()) + "_" + std::to_string(n++);
    maria::createDatabase(config);
  }
  ~ScratchSchema() {
    if (!available) return;
    try {
      maria::dropDatabase(config);
    } catch (...) {
    }
  }
};

struct TempDir {
  std::filesystem::path path;
  TempDir() {
    static int n = 0;
    path = std::filesystem::temp_directory_path() / ("stocks_maria_" + std::to_string(getpid()) + "_" + std::to_string(n++));
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

void skipped() { std::printf("  SKIPPED: MARKET_TEST_DB_USER is not set, so there is no MariaDB to test against\n"); }

EdgeKey edge() { return EdgeKey{InstrumentId{currencyKey("EUR")->value}, InstrumentId{currencyKey("USD")->value},
                                MarketId{marketKey("XXXX")->value}, DatasetId{datasetKey("TESTDS")->value}}; }

// The same cases on a fact store, whichever it is.
void factCases(FactStore & store) {
  FactBatch batch;
  const uint64_t node = currencyKey("EUR")->value;
  batch.changed.push_back({node, "name", std::string("Euro"), 100});
  batch.changed.push_back({node, "decimals", int64_t{2}, 100});
  batch.changed.push_back({node, "rate", 1.0 / 3.0, 100});
  batch.changed.push_back({node, "tradable", true, 100});
  batch.changed.push_back({node, "issuer", NodeKey{currencyKey("USD")->value}, 100});
  batch.changed.push_back({node, "note", std::monostate{}, 100});
  batch.changed.push_back({currencyKey("USD")->value, "name", std::string("Dollar \xC3\xA4"), 100});
  store.apply(batch);
  FactBatch later;
  later.changed.push_back({node, "name", std::string("Euro again"), 200});
  store.apply(later);
  auto all = store.loadAll();
  CHECK(all.size() == 7);
  std::map<std::string, StoredFact> byName;
  for (const StoredFact & f : store.loadNodes({node})) byName[f.predicate] = f;
  CHECK(byName.size() == 6);
  CHECK(std::get<std::string>(byName["name"].value) == "Euro again" && byName["name"].updated == 200);
  CHECK(std::get<int64_t>(byName["decimals"].value) == 2);
  CHECK(std::get<double>(byName["rate"].value) == 1.0 / 3.0);  // exactly
  CHECK(std::get<bool>(byName["tradable"].value) == true);
  CHECK(std::get<NodeKey>(byName["issuer"].value).value == currencyKey("USD")->value);
}

StoredQueue queue(Resolution r, std::vector<Candle> bars) {
  StoredQueue q;
  q.edge = edge();
  q.resolution = r;
  q.kind = QueueKind::Candle;
  q.fetched = 5;
  q.bars = std::move(bars);
  return q;
}

void summaryCases(SummaryStores & stores) {
  auto store = stores.open("TESTDS");
  CHECK(store != nullptr);
  if (!store) return;
  StoredEdge e;
  e.listing.key = edge();
  e.listing.symbol = "EURUSD";
  e.listing.name = "EUR/USD";
  e.quote.last = 1.25;
  e.quote.time = 1000;
  SummaryBatch batch;
  batch.edges.push_back(e);
  batch.queues.push_back(queue(Resolution::Day, {{100, 1, 2, 0.5, 1.5, kUndefined}, {101, 2, 3, 1.5, 2.5, kUndefined}}));
  store->apply(batch);
  const auto edges = store->loadEdges();
  CHECK(edges.size() == 1);
  if (edges.size() == 1) {
    CHECK(edges[0].listing.symbol == "EURUSD" && edges[0].listing.name == "EUR/USD" && edges[0].quote.last == 1.25 && edges[0].quote.time == 1000);
    CHECK(!isDefined(edges[0].quote.bid) && edges[0].queues.size() == 1);
    CHECK(edges[0].queues.size() == 1 && edges[0].queues[0].first == Resolution::Day && edges[0].queues[0].second.count == 2);
  }
  auto read = store->readQueue(edge(), Resolution::Day);
  CHECK(read.has_value() && read->bars.size() == 2 && read->bars[1].close == 2.5);
  CHECK(!store->readQueue(edge(), Resolution::Hour1).has_value());
  // merged into what is there, then trimmed to the window
  StoredQueue merge = queue(Resolution::Day, {{101, 20, 30, 10, 25, kUndefined}, {102, 3, 4, 2, 3, kUndefined}});
  merge.merge = true;
  SummaryBatch again;
  again.queues.push_back(merge);
  store->apply(again);
  read = store->readQueue(edge(), Resolution::Day);
  CHECK(read.has_value() && read->bars.size() == 3 && read->bars[1].open == 20 && read->bars[2].time == 102);
  CHECK(stores.datasets() == std::vector<std::string>{"TESTDS"} || stores.datasets().empty());
}

void barCases(BarStore & store) {
  BarBatch batch;
  batch.series.push_back({edge(), Resolution::Min5, {{10, 1, 2, 0.5, 1.5, 100}, {11, 2, 3, 1.5, 2.5, kUndefined}}});
  store.apply(batch);
  BarBatch again;
  again.series.push_back({edge(), Resolution::Min5, {{11, 20, 30, 15, 25, 7}, {12, 4, 5, 3, 4, 1}}});
  store.apply(again);
  std::vector<Candle> out;
  store.read(edge(), Resolution::Min5, 0, 1000, [&](const Candle & c) { out.push_back(c); });
  CHECK(out.size() == 3);
  CHECK(out.size() == 3 && out[1].open == 20 && out[1].volume == 7 && out[0].volume == 100);
  out.clear();
  store.read(edge(), Resolution::Min5, 11, 11, [&](const Candle & c) { out.push_back(c); });
  CHECK(out.size() == 1 && out[0].time == 11);
}

}  // namespace

TEST(a_mariadb_fact_store_keeps_what_the_sqlite_one_does) {
  TempDir dir;
  factCases(*openSqliteFactStore(dir.path / "facts.sqlite"));
  ScratchSchema schema;
  if (!schema.available) return skipped();
  auto maria = openMariaDbFactStore(schema.config);
  factCases(*maria);
  // The MariaDB store also takes facts and nodes back (the SQLite cache's only does for the user's own file).
  FactBatch gone;
  gone.removed.push_back({currencyKey("EUR")->value, "note"});
  maria->apply(gone);
  CHECK(maria->loadAll().size() == 6);
  FactBatch nodes;
  nodes.removed_nodes.push_back(currencyKey("EUR")->value);
  maria->apply(nodes);
  CHECK(maria->loadAll().size() == 1 && maria->loadNodes({currencyKey("EUR")->value}).empty());
}

TEST(mariadb_summaries_keep_what_the_sqlite_ones_do_queues_as_blobs) {
  TempDir dir;
  summaryCases(*openSqliteSummaries(dir.path));
  ScratchSchema schema;
  if (!schema.available) return skipped();
  summaryCases(*openMariaDbSummaries(schema.config));
}

TEST(a_mariadb_bar_store_keeps_what_the_sqlite_one_does) {
  TempDir dir;
  barCases(*openSqliteBarStore(dir.path / "bars.sqlite"));
  ScratchSchema schema;
  if (!schema.available) return skipped();
  barCases(*openMariaDbBarStore(schema.config));
}

TEST(a_mariadb_store_of_another_schema_version_starts_over) {
  ScratchSchema schema;
  if (!schema.available) return skipped();
  {
    auto bars = openMariaDbBarStore(schema.config);
    BarBatch batch;
    batch.series.push_back({edge(), Resolution::Day, {{1, 1, 1, 1, 1, kUndefined}}});
    bars->apply(batch);
  }
  {
    maria::Connection db(schema.config);
    db.exec("UPDATE store_versions SET version = 99 WHERE store = 'bars'");
  }
  std::vector<std::string> log;
  auto bars = openMariaDbBarStore(schema.config, [&](const std::string & l) { log.push_back(l); });
  std::vector<Candle> out;
  bars->read(edge(), Resolution::Day, 0, 100, [&](const Candle & c) { out.push_back(c); });
  CHECK(out.empty());
  CHECK(log.size() == 1 && log[0].find("starting over") != std::string::npos);
}
