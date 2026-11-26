#pragma once

#include "source/DatasetCodes.h"
#include "TestFramework.h"
#include "TestIsin.h"

#include "FakeHttp.h"
#include "history/HistoryLoader.h"
#include "model/Dates.h"

#include <fstream>
#include <sstream>

// What the history tests share: a store with edges as discovery leaves them, and an Http a loader can own while the test keeps it.
namespace history_test {

inline std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/nordnet/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// A loader owns its Http; a test keeps its FakeHttp to look at what it was asked.
struct Borrowed : Http {
  FakeHttp * inner = nullptr;
  HttpResult get(const std::string & url, const HttpHeaders & h) override { return inner->get(url, h); }
};
inline std::unique_ptr<Http> borrow(FakeHttp & http) {
  auto b = std::make_unique<Borrowed>();
  b->inner = &http;
  return b;
}

inline HistoryLoader::Options quickly() {
  HistoryLoader::Options o;
  o.pause = std::chrono::milliseconds(0);
  o.offline_pause = std::chrono::milliseconds(20);
  return o;
}

// An edge as discovery leaves it: today's bar and a listing with the order book id. `mainData` false: the edge of
// another dataset ("other").
struct World {
  std::shared_ptr<Store> store = makeMemoryStore();
  InstrumentId usd = store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId market = store->addMarket("XSTO", "Nasdaq Stockholm");
  DatasetId nordnet = store->addDataset(kNordnetDataset);
  int count = 0;
  EdgeKey edge(InstrumentKind kind, const std::string & book, bool mainData = true) {
    InstrumentId i = store->addInstrument("", "Instrument", kind, testIsin(100000 + count++));
    EdgeKey k{i, usd, market, mainData ? nordnet : store->addDataset("other")};
    store->appendCandles(k, Resolution::Day, {{dayNumber(2026, 12, 1), kUndefined, 1, 1, 1, kUndefined}});
    store->setListing({k, "SYM", "", book, "ESH", "Instrument"});
    return k;
  }
  CandleSeries days(const EdgeKey & k) { return store->snapshot()->candles(k, Resolution::Day); }
};

}  // namespace history_test
