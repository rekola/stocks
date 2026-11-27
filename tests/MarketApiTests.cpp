// httplib first: the model declares a global seconds(Resolution) that its own `seconds` would otherwise be ambiguous with.
#include <httplib.h>

#include "TestFramework.h"
#include "MarketTestSupport.h"
#include "market/Api.h"
#include "market/Bars.h"
#include "model/Store.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <thread>

using namespace market;
using nlohmann::json;

namespace {

const Timestamp kLast = 1794607080;  // 2026-11-13 21:58:00 UTC (a Friday)

// The keys of a series that the window of the resolution holds, ending at kLast: a bar of every slot (of every week for the weeks).
std::vector<int64_t> windowKeys(Resolution r) {
  std::vector<int64_t> keys;
  const int64_t last = barKey(r, kLast), step = r == Resolution::Week ? 7 : 1;
  for (int64_t k = last; barStart(r, k) >= barStart(r, last) - keepSeconds(r); k -= step) keys.push_back(k);
  std::reverse(keys.begin(), keys.end());
  return keys;
}

const int64_t kDay = barKey(Resolution::Day, kLast);
const char * const kFundIsin = "US0378331005";

// The edges of the seeded store.
struct Edges {
  EdgeKey eurTest, eurOther;  // the same symbol, EURUSD, in two datasets
  EdgeKey gbpTest;            // two daily bars
  EdgeKey fund;               // a scalar queue, in the other dataset
  EdgeKey chfTest;            // a quote and no bars
};

// A store of two datasets (TESTDS, "Test", and OTHERDS, "Other set"):
//   eurTest   EUR/USD in TESTDS, every resolution, the volume undefined (as HistData's), the close of a bar its key and a half
//   eurOther  EUR/USD in OTHERDS (the same symbol), 1h and 1d with volumes, one bar of the day without a volume (and, with `noOpen`, one without an open)
//   gbpTest   GBP/USD in TESTDS, two daily bars
//   fund      a fund by its ISIN in OTHERDS, three daily values (a scalar queue)
//   chfTest   CHF/USD in TESTDS, a quote only
// `noOpen`: the last day of eurOther has no open (the bar store of a database does not take such a bar, so only a store without one is given it).
Edges seed(Store & store, bool noOpen = false) {
  Edges e;
  store.batch([&](Batch & b) {
    const DatasetId test = b.addDataset("TESTDS", "Test"), other = b.addDataset("OTHERDS", "Other set");
    const MarketId none = b.addMarket("XXXX", "none"), off = b.addMarket("XOFF", "off exchange");
    const InstrumentId usd = b.addInstrument("USD", "US dollar", InstrumentKind::Currency);
    const InstrumentId eur = b.addInstrument("EUR", "Euro", InstrumentKind::Currency);
    const InstrumentId gbp = b.addInstrument("GBP", "Pound", InstrumentKind::Currency);
    const InstrumentId chf = b.addInstrument("CHF", "Franc", InstrumentKind::Currency);
    const InstrumentId fund = b.addInstrument("FUNDX", "Fund X", InstrumentKind::Fund, kFundIsin, "FUND");
    e.eurTest = {eur, usd, none, test};
    e.eurOther = {eur, usd, none, other};
    e.gbpTest = {gbp, usd, none, test};
    e.chfTest = {chf, usd, none, test};
    e.fund = {fund, eur, off, other};
    b.setListing({e.eurTest, "EURUSD", "", "", "CURRENCY", "EUR/USD"});
    b.setListing({e.eurOther, "EURUSD", "eu-1", "book-1", "FX", "Euro / Dollar"});
    b.setListing({e.gbpTest, "GBPUSD", "", "", "CURRENCY", "GBP/USD"});
    b.setListing({e.chfTest, "CHFUSD", "", "", "CURRENCY", "CHF/USD"});
    b.setListing({e.fund, "FUNDX", "", "", "", "Fund X (A)", "", "fx-9"});
    for (Resolution r : kResolutions) {
      std::vector<Candle> bars;
      for (int64_t k : windowKeys(r)) bars.push_back({k, static_cast<double>(k), k + 1.0, k - 1.0, static_cast<double>(k) + 0.5, kUndefined});
      b.appendCandles(e.eurTest, r, bars);
    }
    b.appendCandles(e.gbpTest, Resolution::Day, {{kDay - 1, 1, 2, 0.5, 1.25, kUndefined}, {kDay, 1.25, 2, 1, 1.5, kUndefined}});
    b.appendCandles(e.eurOther, Resolution::Hour1, {{barKey(Resolution::Hour1, kLast) - 1, 1.1, 1.2, 1.0, 1.15, 500}, {barKey(Resolution::Hour1, kLast), 1.15, 1.3, 1.1, 1.25, 750}});
    b.appendCandles(e.eurOther, Resolution::Day,
                    {{kDay - 2, 1.1, 1.2, 1.0, 1.15, 1000},
                     {kDay - 1, 1.15, 1.3, 1.1, 1.2, kUndefined},   // no volume
                     {kDay, noOpen ? kUndefined : 1.2, 1.3, 1.1, 1.25, 2000}});
    b.appendCandles(e.fund, Resolution::Day, {{kDay - 2, 100.25, 100.25, 100.25, 100.25, kUndefined}, {kDay - 1, 101.5, 101.5, 101.5, 101.5, kUndefined}, {kDay, 99.75, 99.75, 99.75, 99.75, kUndefined}}, QueueKind::Scalar);
    Quote q;
    q.last = 9.99;
    q.bid = 9.98;
    q.ask = 10.01;
    q.time = kLast;
    b.setQuote(e.chfTest, q);
    b.setPreviousClose(e.eurTest, kDay, 4242.5);
  });
  return e;
}

// The same seed, written to a database, the store let go and another made from what the database has: what the server reads. `load`: whether
// the queues are brought into memory (what the server does at start) or left on the disk.
struct Served {
  std::shared_ptr<Store> store;
  Edges edges;
  Served(const DbConfig & db, bool load = true) {
    {
      const std::shared_ptr<Store> writer = makeMemoryStore(storeOptionsOf(db, {}));
      edges = seed(*writer);
    }
    store = makeMemoryStore(storeOptionsOf(db, {}));
    if (load) loadQueues(*store);
    else store->waitRestored();
  }
};

json get(Store & store, const std::string & path, const std::map<std::string, std::string> & params = {}, int * status = nullptr, std::string * body = nullptr) {
  const ApiResponse r = handleApi(store, path, params);
  if (status) *status = r.status;
  if (body) *body = r.body;
  if (r.status >= 500) std::fprintf(stderr, "  %s answered %d: %s\n", path.c_str(), r.status, r.body.c_str());
  return json::parse(r.body, nullptr, false);
}

std::string idOf(const EdgeKey & e) {
  return text(NodeKey{e.base.value}) + "/" + text(NodeKey{e.quote.value}) + "/" + text(NodeKey{e.market.value}) + "/" + text(NodeKey{e.dataset.value});
}

// The instrument of the answer with this id (null where there is none).
json instrumentWith(const json & answer, const std::string & id) {
  for (const json & i : answer["instruments"])
    if (i["id"] == id) return i;
  return nullptr;
}

// A value of the answer is the store's number: null where it is undefined, the same double where it is not.
bool sameNumber(const json & v, double expected) {
  if (!isDefined(expected)) return v.is_null();
  return v.is_number() && v.get<double>() == expected;
}

// A column of the answer holds what the store's queue holds, bar by bar.
void checkQueueAgainstStore(const Snapshot & s, const EdgeKey & edge, Resolution r, const json & queue) {
  const Seq<Candle> bars = s.queue(edge, r);
  const bool scalar = s.coverage(edge, r)->kind == QueueKind::Scalar;
  CHECK(queue["kind"] == (scalar ? "scalar" : "candle"));
  CHECK(queue["count"] == bars.size() && queue["first_key"] == bars.front().time && queue["last_key"] == bars.back().time);
  CHECK(queue["fetched"] == s.coverage(edge, r)->fetched);
  char hash[17];
  std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(s.coverage(edge, r)->last_hash));
  CHECK(queue["last_hash"] == hash);
  for (const char * column : {"keys", "open", "high", "low", "close", "volume"}) CHECK(queue[column].is_array() && queue[column].size() == bars.size());
  if (queue["keys"].size() != bars.size()) return;
  size_t i = 0;
  for (const Candle & bar : bars) {
    CHECK(queue["keys"][i] == bar.time);
    CHECK(sameNumber(queue["close"][i], bar.close));
    if (scalar) {
      CHECK(queue["open"][i].is_null() && queue["high"][i].is_null() && queue["low"][i].is_null() && queue["volume"][i].is_null());
    } else {
      CHECK(sameNumber(queue["open"][i], bar.open) && sameNumber(queue["high"][i], bar.high) && sameNumber(queue["low"][i], bar.low) &&
            sameNumber(queue["volume"][i], bar.volume));
    }
    i++;
  }
}

}  // namespace

TEST(api_names_each_instrument_by_its_edge_as_the_store_has_it) {
  forEachDatabase([](const DbConfig & db, const char * backend) {
    std::printf("  %s\n", backend);
    Served served(db);
    const Snapshot & s = *served.store->snapshot();
    int status = 0;
    const json health = get(*served.store, "/health", {}, &status);
    CHECK(status == 200 && health["status"] == "ok" && health["instruments"] == 5);
    for (const char * path : {"/instruments", "/summary"}) {
      const json answer = get(*served.store, path);
      CHECK(answer["instruments"].size() == 5);
      // every edge of the store is there once, with its own four keys as NodeKey text
      for (const EdgeKey & e : s.quoteRowsSince(0)) {
        const json i = instrumentWith(answer, idOf(e));
        CHECK(!i.is_null());
        if (i.is_null()) continue;
        CHECK(i["edge"]["base"] == text(NodeKey{e.base.value}) && i["edge"]["quote"] == text(NodeKey{e.quote.value}));
        CHECK(i["edge"]["market"] == text(NodeKey{e.market.value}) && i["edge"]["dataset"] == text(NodeKey{e.dataset.value}));
        CHECK(i["dataset"]["code"] == i["edge"]["dataset"] && i["dataset"]["name"] == s.name(e.dataset));
        const Listing listing = *s.listing(e);
        CHECK(i["listing"]["symbol"] == listing.symbol && i["listing"]["nordnet_identifier"] == listing.nordnetIdentifier && i["listing"]["nordnet_order_book_id"] == listing.nordnetOrderBookId);
        CHECK(i["listing"]["nordnet_type"] == listing.nordnetType && i["listing"]["name"] == listing.name);
        CHECK(i["base"]["kind"] == kindName(s.kind(e.base)));
      }
    }
    const json instruments = get(*served.store, "/instruments");
    const json eur = instrumentWith(instruments, "EUR/USD/XXXX/TESTDS");
    CHECK(!eur.is_null() && idOf(served.edges.eurTest) == "EUR/USD/XXXX/TESTDS");
    CHECK(eur["edge"] == json({{"base", "EUR"}, {"quote", "USD"}, {"market", "XXXX"}, {"dataset", "TESTDS"}}));
    CHECK(eur["dataset"] == json({{"code", "TESTDS"}, {"name", "Test"}}));
    CHECK(eur["listing"] == json({{"symbol", "EURUSD"}, {"nordnet_identifier", ""}, {"nordnet_order_book_id", ""}, {"nordnet_type", "CURRENCY"}, {"name", "EUR/USD"}, {"yahoo_type", ""}, {"fund_identifier", ""}}));
    // a currency is its code alone; a fund is its ISIN, and what a client needs to make it
    CHECK(eur["base"] == json({{"kind", "currency"}, {"name", nullptr}, {"isin", nullptr}}));
    const json fund = instrumentWith(instruments, std::string(kFundIsin) + "/EUR/XOFF/OTHERDS");
    CHECK(!fund.is_null() && fund["edge"]["base"] == kFundIsin);
    CHECK(fund["base"] == json({{"kind", "fund"}, {"name", "Fund X"}, {"isin", kFundIsin}}));
    CHECK(fund["listing"]["fund_identifier"] == "fx-9" && fund["dataset"]["name"] == "Other set");
    // the first and last bars (the listing only), and null where there are no bars
    CHECK(eur["last_bar_time"] == barStart(Resolution::Min5, barKey(Resolution::Min5, kLast)));
    CHECK(eur["first_bar_time"] == barStart(Resolution::Week, windowKeys(Resolution::Week).front()));
    const json chf = instrumentWith(instruments, "CHF/USD/XXXX/TESTDS");
    CHECK(!chf.is_null() && chf["last_bar_time"].is_null() && chf["first_bar_time"].is_null());
    CHECK(get(*served.store, "/summary")["instruments"][0].contains("first_bar_time") == false);
  });
}

TEST(api_keeps_the_same_symbol_of_two_datasets_apart) {
  forEachDatabase([](const DbConfig & db, const char * backend) {
    std::printf("  %s\n", backend);
    Served served(db);
    const json all = get(*served.store, "/instruments");
    int eurusd = 0;
    for (const json & i : all["instruments"]) eurusd += i["listing"]["symbol"] == "EURUSD";
    CHECK(eurusd == 2);
    // each can be chosen, and what comes back is that dataset's own
    const json test = get(*served.store, "/summary", {{"edge", "EUR/USD/XXXX/TESTDS"}});
    const json other = get(*served.store, "/summary", {{"edge", "EUR/USD/XXXX/OTHERDS"}});
    CHECK(test["instruments"].size() == 1 && other["instruments"].size() == 1);
    if (test["instruments"].size() != 1 || other["instruments"].size() != 1) return;
    const json & t = test["instruments"][0], & o = other["instruments"][0];
    CHECK(t["edge"]["dataset"] == "TESTDS" && o["edge"]["dataset"] == "OTHERDS");
    CHECK(t["listing"]["symbol"] == o["listing"]["symbol"] && t["id"] != o["id"]);
    CHECK(t["queues"].size() == 4 && o["queues"].size() == 2);
    CHECK(t["queues"]["1h"]["volume"][0].is_null() && o["queues"]["1h"]["volume"][0] == 500);
    CHECK(t["queues"]["1d"]["count"] != o["queues"]["1d"]["count"]);
    // the dataset filter takes the datasets apart as well
    const json byDataset = get(*served.store, "/summary", {{"dataset", "OTHERDS"}});
    CHECK(byDataset["instruments"].size() == 2);
    for (const json & i : byDataset["instruments"]) CHECK(i["edge"]["dataset"] == "OTHERDS");
    CHECK(get(*served.store, "/instruments", {{"dataset", "TESTDS"}})["instruments"].size() == 3);
  });
}

TEST(api_gives_every_queue_as_the_store_holds_it_with_null_where_undefined) {
  forEachDatabase([](const DbConfig & db, const char * backend) {
    std::printf("  %s\n", backend);
    Served served(db);
    const auto snapshot = served.store->snapshot();
    const Snapshot & s = *snapshot;
    const json answer = get(*served.store, "/summary");
    size_t queues = 0;
    for (const EdgeKey & e : s.quoteRowsSince(0)) {
      const json i = instrumentWith(answer, idOf(e));
      CHECK(!i.is_null());
      if (i.is_null()) continue;
      CHECK(i["queues"].size() == s.queues(e).size());
      for (Resolution r : s.queues(e)) {
        CHECK(i["queues"].contains(name(r)));
        if (!i["queues"].contains(name(r))) continue;
        checkQueueAgainstStore(s, e, r, i["queues"][name(r)]);
        queues++;
      }
    }
    CHECK(queues == 4 + 1 + 2 + 1);  // eurTest's four, gbpTest's, eurOther's two and the fund's; chfTest has none
    // the queue is whole: the window of the store, not cut to a period, and a bar once (a week is a week, not also a year)
    const json eur = instrumentWith(answer, "EUR/USD/XXXX/TESTDS");
    for (Resolution r : kResolutions) {
      const json & q = eur["queues"][name(r)];
      CHECK(q["count"] == windowKeys(r).size() && q["keys"].front() == windowKeys(r).front() && q["keys"].back() == windowKeys(r).back());
    }
    CHECK(eur["queues"].size() == 4 && eur["queues"].contains("5m") && eur["queues"].contains("1h") && eur["queues"].contains("1d") && eur["queues"].contains("1w"));
    CHECK(eur["queues"]["1w"]["keys"][1].get<int64_t>() - eur["queues"]["1w"]["keys"][0].get<int64_t>() == 7);  // the model's key of a week
    // HistData-like: the volume is undefined in every bar, the others are numbers
    for (Resolution r : kResolutions) {
      const json & q = eur["queues"][name(r)];
      for (size_t k = 0; k < q["keys"].size(); k++) {
        CHECK(q["volume"][k].is_null());
        CHECK(q["open"][k].is_number() && q["high"][k].is_number() && q["low"][k].is_number() && q["close"][k].is_number());
      }
    }
    const json & day = eur["queues"]["1d"];
    CHECK(day["close"].back().get<double>() > static_cast<double>(kDay) && std::fabs(day["close"].back().get<double>() - (kDay + 0.5)) < 0.01);
    // volumes where there are some, and null in the bar that has none
    const json od = instrumentWith(answer, "EUR/USD/XXXX/OTHERDS")["queues"]["1d"];
    CHECK(od["volume"][0] == 1000 && od["volume"][1].is_null() && od["volume"][2] == 2000);
    CHECK(od["open"][0].is_number() && od["high"][1].is_number() && od["low"][1].is_number() && od["close"][2].is_number());
    CHECK(std::fabs(od["close"][2].get<double>() - 1.25) < 1e-6 && std::fabs(od["open"][2].get<double>() - 1.2) < 1e-6);
  });
}

TEST(api_sends_a_bar_with_no_open_as_a_null_open_and_keeps_its_high_and_low) {
  const std::shared_ptr<Store> store = makeMemoryStore();
  const Edges edges = seed(*store, true);
  std::string body;
  const json answer = get(*store, "/summary", {{"edge", "EUR/USD/XXXX/OTHERDS"}}, nullptr, &body);
  CHECK(body.find("NaN") == std::string::npos && body.find("nan") == std::string::npos);
  const json & od = answer["instruments"][0]["queues"]["1d"];
  checkQueueAgainstStore(*store->snapshot(), edges.eurOther, Resolution::Day, od);
  CHECK(od["open"][2].is_null() && od["open"][0].is_number() && od["open"][1].is_number());  // not the close, not a number made up
  CHECK(std::fabs(od["close"][2].get<double>() - 1.25) < 1e-6 && std::fabs(od["high"][2].get<double>() - 1.3) < 1e-6 && std::fabs(od["low"][2].get<double>() - 1.1) < 1e-6);
  CHECK(od["volume"][2] == 2000);
}

TEST(api_reports_a_scalar_queue_as_close_alone) {
  forEachDatabase([](const DbConfig & db, const char * backend) {
    std::printf("  %s\n", backend);
    Served served(db);
    const json fund = get(*served.store, "/summary", {{"edge", std::string(kFundIsin) + "/EUR/XOFF/OTHERDS"}})["instruments"][0];
    const json & q = fund["queues"]["1d"];
    CHECK(q["kind"] == "scalar" && q["count"] == 3 && q["keys"] == json::array({kDay - 2, kDay - 1, kDay}));
    CHECK(q["close"] == json::array({100.25, 101.5, 99.75}));
    for (const char * column : {"open", "high", "low", "volume"}) CHECK(q[column] == json::array({nullptr, nullptr, nullptr}));
    // and the others are marked as bars
    CHECK(get(*served.store, "/summary", {{"edge", "EUR/USD/XXXX/OTHERDS"}})["instruments"][0]["queues"]["1d"]["kind"] == "candle");
  });
}

TEST(api_leaves_out_a_resolution_that_is_not_in_memory) {
  forEachDatabase([](const DbConfig & db, const char * backend) {
    std::printf("  %s\n", backend);
    Served served(db, false);  // the queues are on the disk
    const json eur = get(*served.store, "/summary", {{"edge", "EUR/USD/XXXX/TESTDS"}})["instruments"][0];
    CHECK(eur["queues"].is_object() && eur["queues"].empty());
    CHECK(eur["last_bar_time"].is_null());  // no queue is sent
    // the coverage is known all the same: the listing has it, and the queue is not "gone" but not loaded yet
    const json listed = get(*served.store, "/instruments", {{"edge", "EUR/USD/XXXX/TESTDS"}});
    CHECK(listed["instruments"][0]["last_bar_time"] == barStart(Resolution::Min5, barKey(Resolution::Min5, kLast)) && listed["missing"].empty());
    loadQueues(*served.store);
    CHECK(get(*served.store, "/summary", {{"edge", "EUR/USD/XXXX/TESTDS"}})["instruments"][0]["queues"].size() == 4);
  });
}

TEST(api_says_nothing_of_a_quote_a_previous_close_or_a_change) {
  forEachDatabase([](const DbConfig & db, const char * backend) {
    std::printf("  %s\n", backend);
    Served served(db);
    std::string body;
    get(*served.store, "/summary", {}, nullptr, &body);
    const json answer = json::parse(body);
    // the store has them (a quote of 9.99 for CHF, a previous close of 4242.5 for EUR), and the answer has none of their names or values
    CHECK(served.store->snapshot()->quote(served.edges.chfTest)->last == 9.99);
    for (const char * word : {"bid", "ask", "previous", "change", "pct", "periods", "NaN", "nan", "Infinity", "9.99", "9.98", "10.01", "4242.5"}) {
      if (body.find(word) != std::string::npos) std::fprintf(stderr, "  the answer has '%s'\n", word);
      CHECK(body.find(word) == std::string::npos);
    }
    const std::set<std::string> instrumentKeys = {"id", "edge", "dataset", "listing", "base", "last_bar_time", "queues"};
    const std::set<std::string> queueKeys = {"kind", "first_key", "last_key", "count", "last_hash", "fetched", "keys", "open", "high", "low", "close", "volume"};
    for (const json & i : answer["instruments"]) {
      std::set<std::string> keys;
      for (auto it = i.begin(); it != i.end(); ++it) keys.insert(it.key());
      CHECK(keys == instrumentKeys);
      for (auto q = i["queues"].begin(); q != i["queues"].end(); ++q) {
        std::set<std::string> queueFields;
        for (auto it = q->begin(); it != q->end(); ++it) queueFields.insert(it.key());
        CHECK(queueFields == queueKeys);
      }
    }
  });
}

namespace {

std::vector<std::string> idsOf(const json & answer) {
  std::vector<std::string> out;
  for (const json & i : answer["instruments"]) out.push_back(i["id"]);
  return out;
}

std::vector<std::string> keysOf(const json & object) {
  std::vector<std::string> out;
  for (auto it = object.begin(); it != object.end(); ++it) out.push_back(it.key());
  return out;
}

}  // namespace

TEST(api_chooses_instruments_by_edge_and_dataset_and_refuses_what_it_cannot_read) {
  forEachDatabase([](const DbConfig & db, const char * backend) {
    std::printf("  %s\n", backend);
    Served served(db);
    Store & store = *served.store;
    int status = 0;
    // an edge, or several (in the listing's order, once each)
    CHECK(idsOf(get(store, "/summary", {{"edge", "EUR/USD/XXXX/TESTDS"}})) == std::vector<std::string>{"EUR/USD/XXXX/TESTDS"});
    CHECK(idsOf(get(store, "/instruments", {{"edge", "GBP/USD/XXXX/TESTDS,EUR/USD/XXXX/OTHERDS,GBP/USD/XXXX/TESTDS"}})) ==
          (std::vector<std::string>{"EUR/USD/XXXX/OTHERDS", "GBP/USD/XXXX/TESTDS"}));
    CHECK(idsOf(get(store, "/summary", {{"edge", "EUR/USD/XXXX/TESTDS"}, {"dataset", "TESTDS"}})) == std::vector<std::string>{"EUR/USD/XXXX/TESTDS"});
    CHECK(get(store, "/summary", {{"dataset", "TESTDS"}})["instruments"].size() == 3);
    // empty items are skipped, and an edge with no ids left is no filter
    const std::vector<std::string> both = {"EUR/USD/XXXX/OTHERDS", "EUR/USD/XXXX/TESTDS"};
    CHECK(idsOf(get(store, "/instruments", {{"edge", "EUR/USD/XXXX/TESTDS,"}})) == std::vector<std::string>{"EUR/USD/XXXX/TESTDS"});
    CHECK(idsOf(get(store, "/instruments", {{"edge", ",EUR/USD/XXXX/TESTDS,,EUR/USD/XXXX/OTHERDS"}})) == both);
    CHECK(get(store, "/instruments", {{"edge", ""}}, &status)["instruments"].size() == 5 && status == 200);
    CHECK(get(store, "/instruments", {{"edge", ",,"}}, &status)["instruments"].size() == 5 && status == 200);
    CHECK(get(store, "/instruments", {{"edge", ""}})["missing"].empty());
    // one rule for what matches nothing: 200 and no instruments
    CHECK(get(store, "/summary", {{"dataset", "NOSUCH"}}, &status)["instruments"] == json::array() && status == 200);
    CHECK(get(store, "/instruments", {{"dataset", "NOSUCH"}}, &status)["instruments"] == json::array() && status == 200);
    const json apart = get(store, "/summary", {{"edge", "EUR/USD/XXXX/TESTDS"}, {"dataset", "OTHERDS"}}, &status);
    CHECK(apart["instruments"] == json::array() && apart["missing"] == json::array() && status == 200);  // in the store, not in that dataset: left out, not gone
    // a well-formed id that is no instrument of the store is "missing", and the others are there
    const json gone = get(store, "/summary", {{"edge", "EUR/USD/XXXX/TESTDS,EUR/GBP/XXXX/TESTDS,EUR/USD/XXXX/NODATA,EUR/GBP/XXXX/TESTDS"}}, &status);
    CHECK(status == 200 && idsOf(gone) == std::vector<std::string>{"EUR/USD/XXXX/TESTDS"});
    CHECK(gone["missing"] == json::array({"EUR/GBP/XXXX/TESTDS", "EUR/USD/XXXX/NODATA"}));
    CHECK(get(store, "/instruments", {{"edge", "EUR/GBP/XXXX/TESTDS"}}, &status)["missing"] == json::array({"EUR/GBP/XXXX/TESTDS"}) && status == 200);
    CHECK(get(store, "/summary", {{"edge", "ZZZ/USD/XXXX/TESTDS"}})["instruments"].empty());
    // what it refuses names what was wrong
    const auto refused = [&](const std::map<std::string, std::string> & params, const std::string & mention, const std::string & path = "/summary") {
      std::string body;
      const json answer = get(store, path, params, &status, &body);
      const bool ok = status == 400 && answer["error"].is_string() && answer["error"].get<std::string>().find(mention) != std::string::npos && !answer.contains("instruments");
      if (!ok) std::fprintf(stderr, "  expected 400 naming '%s', got %d %s\n", mention.c_str(), status, body.c_str());
      CHECK(ok);
    };
    refused({{"edge", "EUR/USD"}}, "EUR/USD");
    refused({{"edge", "EUR/USD/XXXX"}}, "EUR/USD/XXXX");
    refused({{"edge", "EUR/USD/XXXXX/TESTDS"}}, "XXXXX");
    refused({{"edge", "EUR/USD/XXXX/TEST DS"}}, "TEST DS");
    refused({{"edge", "EUR/USD/XXXX/TESTDS,x/y/z/w"}}, "x/y/z/w");
    refused({{"edge", "ABCDEFGHIJKL/USD/XXXX/TESTDS"}}, "ABCDEFGHIJKL");  // twelve letters are no ISIN and too long for a code
    refused({{"edge", "eur/usd/xxxx/testds"}}, "eur/usd/xxxx/testds");      // only the id as it is returned
    refused({{"edge", "EUR/USD/XXXX/testds"}}, "EUR/USD/XXXX/TESTDS");
    refused({{"edge", "US0378331006/EUR/XOFF/OTHERDS"}}, "US0378331006");  // an ISIN with a wrong check digit
    refused({{"edge", "EUR/USD"}}, "EUR/USD", "/instruments");
    refused({{"dataset", "bad dataset!"}}, "bad dataset!");
    refused({{"dataset", ""}}, "dataset");
    refused({{"dataset", "testds"}}, "TESTDS");
    refused({{"dataset", "TESTDS,OTHERDS"}}, "dataset");  // one code, whether it is a repeated parameter or a comma
    refused({{"dataset", "TESTDS,OTHERDS"}}, "dataset", "/instruments");
    refused({{"resolution", "2x"}}, "2x");
    refused({{"resolution", "1d,1y,1w"}}, "1y");
    refused({{"resolution", "1D"}}, "1D");  // the model's names are cased (1m is a minute, 1M a month)
    refused({{"resolution", "1d"}}, "resolution", "/instruments");
    refused({{"resolution", ""}}, "resolution", "/instruments");
    // the old parameter is not read as no filter
    refused({{"codes", "eurusd"}}, "codes");
    refused({{"edge", "EUR/USD/XXXX/TESTDS"}, {"foo", "1"}}, "foo");
    refused({{"dataset", "TESTDS"}}, "dataset", "/health");
    CHECK(get(store, "/nowhere", {}, &status)["error"].is_string() && status == 404);
    CHECK(get(store, "/raw-bars", {}, &status)["error"].is_string() && status == 404);
  });
}

TEST(api_sends_only_the_resolutions_asked_for_and_last_bar_time_follows_what_is_sent) {
  forEachDatabase([](const DbConfig & db, const char * backend) {
    std::printf("  %s\n", backend);
    Served served(db);
    Store & store = *served.store;
    const std::string eur = "EUR/USD/XXXX/TESTDS";
    const auto queuesOf = [&](const std::map<std::string, std::string> & params) {
      const json answer = get(store, "/summary", params);
      return answer["instruments"].empty() ? std::vector<std::string>{"<none>"} : keysOf(answer["instruments"][0]["queues"]);
    };
    CHECK(queuesOf({{"edge", eur}}) == (std::vector<std::string>{"1d", "1h", "1w", "5m"}));
    CHECK(queuesOf({{"edge", eur}, {"resolution", "1d"}}) == std::vector<std::string>{"1d"});
    CHECK(queuesOf({{"edge", eur}, {"resolution", "1d,1w"}}) == (std::vector<std::string>{"1d", "1w"}));
    CHECK(queuesOf({{"edge", eur}, {"resolution", ",1w,,1w,"}}) == std::vector<std::string>{"1w"});
    CHECK(queuesOf({{"edge", eur}, {"resolution", ""}}) == (std::vector<std::string>{"1d", "1h", "1w", "5m"}));  // no names: no filter
    // a resolution the edge has not is left out of it, not an error
    int status = 0;
    const json gbp = get(store, "/summary", {{"edge", "GBP/USD/XXXX/TESTDS"}, {"resolution", "1h,1w"}}, &status);
    CHECK(status == 200 && gbp["instruments"].size() == 1 && gbp["instruments"][0]["queues"] == json::object());
    CHECK(queuesOf({{"edge", "GBP/USD/XXXX/TESTDS"}, {"resolution", "1d,5m"}}) == std::vector<std::string>{"1d"});
    CHECK(queuesOf({{"edge", eur}, {"resolution", "1M"}}).empty());  // a name of the model that this edge has no queue for
    // the queue sent is the whole queue, whichever the others
    const json one = get(store, "/summary", {{"edge", eur}, {"resolution", "1d"}})["instruments"][0];
    CHECK(one["queues"]["1d"]["count"] == windowKeys(Resolution::Day).size());
    // last_bar_time is the start of the latest bar of the queues sent; the listing says the store's
    const Timestamp fiveMin = barStart(Resolution::Min5, barKey(Resolution::Min5, kLast));
    CHECK(get(store, "/summary", {{"edge", eur}})["instruments"][0]["last_bar_time"] == fiveMin);
    CHECK(one["last_bar_time"] == barStart(Resolution::Day, barKey(Resolution::Day, kLast)));
    CHECK(get(store, "/summary", {{"edge", eur}, {"resolution", "1w"}})["instruments"][0]["last_bar_time"] == barStart(Resolution::Week, barKey(Resolution::Week, kLast)));
    CHECK(get(store, "/summary", {{"edge", eur}, {"resolution", "1h,1w"}})["instruments"][0]["last_bar_time"] == barStart(Resolution::Hour1, barKey(Resolution::Hour1, kLast)));
    CHECK(gbp["instruments"][0]["last_bar_time"].is_null());  // nothing sent
    CHECK(get(store, "/instruments", {{"edge", eur}})["instruments"][0]["last_bar_time"] == fiveMin);
    CHECK(get(store, "/instruments", {{"edge", "GBP/USD/XXXX/TESTDS"}})["instruments"][0]["last_bar_time"] == barStart(Resolution::Day, kDay));
  });
  // the queues that are not in memory are not sent, and do not count: the listing still has their coverage
  forEachDatabase([](const DbConfig & db, const char * backend) {
    std::printf("  %s\n", backend);
    Served served(db, false);
    const json eur = get(*served.store, "/summary", {{"edge", "EUR/USD/XXXX/TESTDS"}})["instruments"][0];
    CHECK(eur["queues"].empty() && eur["last_bar_time"].is_null());
    CHECK(get(*served.store, "/instruments", {{"edge", "EUR/USD/XXXX/TESTDS"}})["instruments"][0]["last_bar_time"] ==
          barStart(Resolution::Min5, barKey(Resolution::Min5, kLast)));
  });
}

TEST(api_gives_a_queue_the_marks_to_tell_whether_it_changed) {
  const std::shared_ptr<Store> store = makeMemoryStore();
  const Edges edges = seed(*store);
  const auto read = [&] { return get(*store, "/summary", {{"edge", "GBP/USD/XXXX/TESTDS"}, {"resolution", "1d"}})["instruments"][0]["queues"]["1d"]; };
  const json before = read();
  CHECK(before["last_key"] == kDay && before["count"] == 2 && before["last_hash"].is_string() && before["last_hash"].get<std::string>().size() == 16);
  // the last bar says another close: same key and count, another hash
  store->batch([&](Batch & b) { b.appendCandles(edges.gbpTest, Resolution::Day, {{kDay, 1.25, 2, 1, 1.75, kUndefined}}); });
  const json after = read();
  CHECK(after["last_key"] == before["last_key"] && after["count"] == before["count"] && after["last_hash"] != before["last_hash"]);
  // a new day: the key and the count move
  store->batch([&](Batch & b) { b.appendCandles(edges.gbpTest, Resolution::Day, {{kDay + 1, 1.75, 2, 1, 1.8, kUndefined}}); });
  CHECK(read()["last_key"] == kDay + 1 && read()["count"] == 3);
}

namespace {

// The server over real HTTP on a free port, until it goes out of scope.
struct Running {
  std::shared_ptr<Store> store;
  ApiServer server;
  std::thread thread;
  std::unique_ptr<httplib::Client> client;
  explicit Running(std::shared_ptr<Store> s) : store(std::move(s)), server([this] { return store; }) {
    std::string error;
    CHECK(server.bind("127.0.0.1", 0, error));
    thread = std::thread([this] { server.serve(); });
    client = std::make_unique<httplib::Client>("127.0.0.1", server.port());
  }
  ~Running() {
    server.stop();
    thread.join();
  }
};

// An edge id of the longest ids there are, which no store has (a currency's code of eleven characters, with the "#" the key allows): `n` is
// in the letters of its base, so that the ids are all different.
std::string longId(int n) {
  std::string base = "AAAA#######";
  base[0] = static_cast<char>('A' + n / 26 / 26 % 26);
  base[1] = static_cast<char>('A' + n / 26 % 26);
  base[2] = static_cast<char>('A' + n % 26);
  return base + "/###########/ABCD/###########";
}

// The percent-encoding of a query value as a client sends it: everything but the unreserved characters.
std::string encoded(const std::string & v) {
  std::string out;
  for (unsigned char c : v) {
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out.push_back(static_cast<char>(c));
    else {
      char hex[4];
      std::snprintf(hex, sizeof hex, "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

}  // namespace

TEST(api_over_http_is_read_only_json_and_joins_a_parameter_given_again) {
  Running run(makeMemoryStore());
  seed(*run.store);
  httplib::Client & client = *run.client;
  auto health = client.Get("/health");
  CHECK(health && health->status == 200 && health->get_header_value("Content-Type").find("application/json") == 0);
  auto summary = client.Get("/summary?edge=EUR/USD/XXXX/TESTDS,EUR/USD/XXXX/OTHERDS");
  CHECK(summary && summary->status == 200 && json::parse(summary->body)["instruments"].size() == 2);
  // `edge` and `resolution` given again are joined, not dropped
  auto again = client.Get("/summary?edge=EUR/USD/XXXX/TESTDS&edge=GBP/USD/XXXX/TESTDS&resolution=1d&resolution=1w");
  CHECK(again && again->status == 200);
  if (again && again->status == 200) {
    const json answer = json::parse(again->body);
    CHECK(idsOf(answer) == (std::vector<std::string>{"EUR/USD/XXXX/TESTDS", "GBP/USD/XXXX/TESTDS"}));
    CHECK(keysOf(answer["instruments"][0]["queues"]) == (std::vector<std::string>{"1d", "1w"}));
  }
  auto twice = client.Get("/summary?edge=EUR/USD/XXXX/TESTDS&edge=&edge=GBP/USD/XXXX/TESTDS,");
  CHECK(twice && twice->status == 200 && json::parse(twice->body)["instruments"].size() == 2);
  // `dataset` given again is refused, and says so
  auto datasets = client.Get("/summary?dataset=TESTDS&dataset=OTHERDS");
  CHECK(datasets && datasets->status == 400 && json::parse(datasets->body)["error"].get<std::string>().find("dataset") != std::string::npos);
  auto byDataset = client.Get("/instruments?dataset=OTHERDS");
  CHECK(byDataset && byDataset->status == 200 && json::parse(byDataset->body)["instruments"].size() == 2);
  auto empty = client.Get("/instruments?dataset=NOSUCH");
  CHECK(empty && empty->status == 200 && json::parse(empty->body)["instruments"].empty());
  auto bad = client.Get("/summary?edge=nonsense");
  CHECK(bad && bad->status == 400 && json::parse(bad->body)["error"].get<std::string>().find("nonsense") != std::string::npos);
  auto resolution = client.Get("/instruments?resolution=1d");
  CHECK(resolution && resolution->status == 400);
  auto old = client.Get("/summary?codes=eurusd");
  CHECK(old && old->status == 400);
  auto post = client.Post("/summary", "x", "text/plain");
  CHECK(post && post->status == 405);
  auto put = client.Put("/summary", "x", "text/plain");
  CHECK(put && put->status == 405);
  auto patch = client.Patch("/summary", "x", "text/plain");
  CHECK(patch && patch->status == 405);
  auto del = client.Delete("/instruments");
  CHECK(del && del->status == 405);
  auto missing = client.Get("/raw-bars");
  CHECK(missing && missing->status == 404);
}

TEST(api_over_http_takes_a_hundred_edge_ids_of_the_longest_kind_and_refuses_a_hundred_and_one) {
  Running run(makeMemoryStore());
  seed(*run.store);
  const auto request = [&](int n, bool asOneParameter) {
    std::string target = "/summary?resolution=5m%2C1h%2C1d%2C1w&dataset=TESTDS";
    std::string all;
    for (int i = 0; i < n; i++) {
      const std::string id = encoded(longId(i));
      if (asOneParameter) all += (i ? "%2C" : "") + id;
      else target += "&edge=" + id;
    }
    if (asOneParameter) target += "&edge=" + all;
    return std::pair<std::string, httplib::Result>(target, run.client->Get(target));
  };
  for (bool oneParameter : {true, false}) {
    const auto [target, hundred] = request(100, oneParameter);
    // the request line is longer than the library's own limit of 8 KB: the limit of the build holds it
    CHECK(target.size() > 8192 && target.size() < 16384);
    CHECK(hundred && hundred->status == 200);
    if (hundred && hundred->status == 200) {
      const json answer = json::parse(hundred->body);
      CHECK(answer["instruments"].empty() && answer["missing"].size() == 100);  // well formed, and the store has none of them
    }
    const auto [target101, over] = request(101, oneParameter);
    CHECK(over && over->status == 400);
    if (over && over->status == 400) {
      const std::string error = json::parse(over->body)["error"];
      CHECK(error.find("100") != std::string::npos && error.find("101") != std::string::npos);
    }
  }
}

#ifdef CPPHTTPLIB_ZLIB_SUPPORT
#include <zlib.h>

TEST(api_over_http_answers_gzip_to_a_client_that_asks_for_it_with_the_same_json) {
  Running run(makeMemoryStore());
  seed(*run.store);
  const std::string target = "/summary?edge=EUR/USD/XXXX/TESTDS";
  auto plain = run.client->Get(target, {{"Accept-Encoding", "identity"}});
  CHECK(plain && plain->status == 200 && plain->get_header_value("Content-Encoding").empty());
  httplib::Client raw("127.0.0.1", run.server.port());
  raw.set_decompress(false);  // see the bytes on the wire
  auto packed = raw.Get(target, {{"Accept-Encoding", "gzip"}});
  CHECK(packed && packed->status == 200 && packed->get_header_value("Content-Encoding") == "gzip");
  if (!packed || !plain) return;
  CHECK(packed->body.size() < plain->body.size());
  std::string inflated;
  z_stream z{};
  CHECK(inflateInit2(&z, 15 + 16) == Z_OK);  // gzip
  z.next_in = reinterpret_cast<Bytef *>(packed->body.data());
  z.avail_in = static_cast<uInt>(packed->body.size());
  char buffer[16384];
  int rc = Z_OK;
  while (rc == Z_OK) {
    z.next_out = reinterpret_cast<Bytef *>(buffer);
    z.avail_out = sizeof buffer;
    rc = inflate(&z, Z_NO_FLUSH);
    inflated.append(buffer, sizeof buffer - z.avail_out);
  }
  inflateEnd(&z);
  CHECK(rc == Z_STREAM_END && json::parse(inflated) == json::parse(plain->body));
  // the library's own client decodes it too
  auto decoded = run.client->Get(target, {{"Accept-Encoding", "gzip"}});
  CHECK(decoded && json::parse(decoded->body) == json::parse(plain->body));
}
#endif
