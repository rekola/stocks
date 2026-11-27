#include "TestFramework.h"
#include "TestIsin.h"

#include "FakeHttp.h"
#include "Quotes.h"
#include "history/HistoryLoader.h"
#include "model/Dates.h"
#include "source/EcbSource.h"
#include "source/InstrumentLists.h"
#include "source/MarketServerSource.h"
#include "source/SuomenPankkiSource.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <fstream>
#include <sstream>

// The client of the market-data server: its rows, its bars, its batches and the rule that it runs alone. The answers are the server's own
// (tests/fixtures/marketserver: /instruments and /summary of an ingest of HistData's fake site, and a second dataset with the same pair and a
// fund's scalar queue, trimmed to the last bars of each queue); `Server` answers a URL from them as the real one does.
using nlohmann::json;

namespace {

const char * const kUrl = "http://market.test:8080";

std::string fixtureText(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/marketserver/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}
json fixtureJson(const std::string & name) { return json::parse(fixtureText(name)); }

std::string decoded(const std::string & s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::vector<std::string> split(const std::string & s, char sep) {
  std::vector<std::string> out;
  std::stringstream in(s);
  std::string item;
  while (std::getline(in, item, sep))
    if (!item.empty()) out.push_back(item);
  return out;
}

// The server, from the fixtures: /instruments, and /summary for the edges and resolutions asked (an id it has not got is "missing", a
// resolution an instrument has not got is left out of it), as Api.h says. `forget` ids are listed without their queues (not in memory yet).
struct Server : FakeHttp {
  json instruments = fixtureJson("instruments.json");
  json everything;
  std::set<std::string> unloaded;  // listed, with no queues
  std::vector<std::string> summaryUrls;
  bool down = false;
  Server() {
    // The full answer is what the fixtures were trimmed from: the trimmed ones, with every resolution, are put together here.
    everything = {{"instruments", json::array()}, {"missing", json::array()}};
    for (const char * name : {"summary_1d.json", "summary_1w.json", "summary_1h.json", "summary_5m_1d.json"}) {
      const json part = fixtureJson(name);
      for (const json & i : part["instruments"]) {
        auto at = std::find_if(everything["instruments"].begin(), everything["instruments"].end(), [&](const json & e) { return e["id"] == i["id"]; });
        if (at == everything["instruments"].end()) everything["instruments"].push_back(i);
        else
          for (const auto & [r, q] : i["queues"].items()) (*at)["queues"][r] = q;
      }
    }
    answer = [this](const std::string & url) { return reply(url); };
  }
  HttpResult reply(const std::string & url) {
    if (down) return failed(0);
    if (url == std::string(kUrl) + "/instruments") return ok(instruments.dump());
    const std::string prefix = std::string(kUrl) + "/summary?";
    if (url.rfind(prefix, 0) != 0) return failed(404);
    summaryUrls.push_back(url);
    std::vector<std::string> ids, resolutions;
    for (const std::string & part : split(url.substr(prefix.size()), '&')) {
      if (part.rfind("edge=", 0) == 0) ids = split(decoded(part.substr(5)), ',');
      if (part.rfind("resolution=", 0) == 0) resolutions = split(part.substr(11), ',');
    }
    json out = {{"instruments", json::array()}, {"missing", json::array()}};
    for (const std::string & id : ids) {
      auto at = std::find_if(everything["instruments"].begin(), everything["instruments"].end(), [&](const json & e) { return e["id"] == id; });
      if (at == everything["instruments"].end()) {
        out["missing"].push_back(id);
        continue;
      }
      json i = *at;
      json queues = json::object();
      if (!unloaded.count(id))
        for (const std::string & r : resolutions)
          if (i["queues"].contains(r)) queues[r] = i["queues"][r];
      i["queues"] = queues;
      out["instruments"].push_back(i);
    }
    return ok(out.dump());
  }
};

std::unique_ptr<Store> plainStore() {
  StoreOptions options;
  options.invent_isins = false;  // as the programs have it
  return makeMemoryStore(std::move(options));
}

// The edge of the server's texts (an ISIN or a code for the instruments, a MIC, a dataset code), as the keys make it.
EdgeKey edgeOf(const std::string & base, const std::string & quote, const std::string & market, const std::string & dataset) {
  const auto instrument = [](const std::string & t) { return t.size() == 12 ? instrumentKey(t) : currencyKey(t); };
  return {InstrumentId{instrument(base)->value}, InstrumentId{instrument(quote)->value}, MarketId{marketKey(market)->value}, DatasetId{datasetKey(dataset)->value}};
}
const EdgeKey kEurusd = edgeOf("EUR", "USD", "XXXX", "HISTDATA");
const EdgeKey kGold = edgeOf("XC0009655157", "USD", "XXXX", "HISTDATA");
const EdgeKey kSpx = edgeOf("US78378X1072", "PTS", "XXXX", "HISTDATA");
const EdgeKey kEurusdOther = edgeOf("EUR", "USD", "XXXX", "OTHERDS");
const EdgeKey kFund = edgeOf("US0378331005", "EUR", "XOFF", "OTHERDS");

struct Loaded {
  Server server;
  std::unique_ptr<Store> store = plainStore();
  Listener listener;
  MarketServerSource source{[] {
    MarketServerSource::Options o;
    o.url = kUrl;
    return o;
  }()};
  Loaded() { source.load(*store, server, listener); }
};

// Loads `edges` for a period through the provider the source hands over, as the history loader asks it.
std::vector<HistoryProvider::Outcome> loadBatch(Loaded & w, const std::vector<EdgeKey> & edges, int request) {
  return w.source.historyProvider()->loadBatch(*w.store, w.server, edges, request, [] { return true; });
}

using Outcome = HistoryProvider::Outcome;

}  // namespace

// ---- The rows

TEST(the_instruments_of_the_server_are_rows_with_the_servers_edges_and_no_bars) {
  Loaded w;
  const auto s = w.store->snapshot();
  CHECK(w.listener.messages.empty() && w.listener.changes == 1);
  CHECK(s->quoteRowCount() == 5);
  // Only /instruments was read: no bars, and no other request.
  CHECK(w.server.urls.size() == 1 && w.server.urls[0] == std::string(kUrl) + "/instruments");
  for (const EdgeKey & e : {kEurusd, kGold, kSpx}) {
    CHECK(s->listed(e));
    CHECK(s->queues(e).empty() && !s->quote(e) && s->candles(e, Resolution::Day).candles.empty());
    CHECK(datasetCode(e.dataset) == "HISTDATA" && s->name(e.dataset) == "HistData.com");
  }
  // The edges are the server's: the same ids, from the same nodes.
  CHECK(marketServerEdgeId(kEurusd) == "EUR/USD/XXXX/HISTDATA");
  CHECK(marketServerEdgeId(kGold) == "XC0009655157/USD/XXXX/HISTDATA");
  CHECK(marketServerEdgeId(kSpx) == "US78378X1072/PTS/XXXX/HISTDATA");
  CHECK(s->kind(kEurusd.base) == InstrumentKind::Currency && s->kind(kEurusd.quote) == InstrumentKind::Currency);
  CHECK(s->kind(kGold.base) == InstrumentKind::Commodity && s->isin(kGold.base) == "XC0009655157" && s->name(kGold.base) == "Gold");
  CHECK(s->kind(kSpx.base) == InstrumentKind::Index && s->name(kSpx.base) == "S&P 500");
  CHECK(s->kind(kSpx.quote) == InstrumentKind::Unit && s->symbol(kSpx.quote) == kPointsUnit);  // an index is quoted in points
  CHECK(s->listing(kEurusd) == Listing({kEurusd, "EURUSD", "", "", "CURRENCY", "EUR/USD"}));
  CHECK(s->listing(kGold) == Listing({kGold, "XAUUSD", "", "", "METAL", "XAU/USD"}));
  // A row with no bars shows no price, change or trace: not a zero.
  Quotes q(s, {kEurusd});
  CHECK(q.text(0, Quotes::PRICE).empty() && q.text(0, Quotes::CHANGE).empty() && q.trace(0, Quotes::HISTORY).empty() && !q.number(0, Quotes::PRICE));
  CHECK(q.text(0, Quotes::DATASET) == "HistData.com" && q.text(0, Quotes::NAME) == "EUR/USD");
}

TEST(the_same_symbol_in_two_datasets_is_two_edges_each_under_its_own_dataset) {
  Loaded w;
  const auto s = w.store->snapshot();
  CHECK(!(kEurusd == kEurusdOther) && kEurusd.base == kEurusdOther.base && kEurusd.quote == kEurusdOther.quote);
  CHECK(s->listed(kEurusd) && s->listed(kEurusdOther));
  CHECK(datasetCode(kEurusdOther.dataset) == "OTHERDS" && s->name(kEurusdOther.dataset) == "Other set");
  CHECK(s->name(kEurusd.dataset) == "HistData.com");
  // Each keeps what its own dataset said of it, whole.
  CHECK(s->listing(kEurusdOther) == Listing({kEurusdOther, "EURUSD", "eu-1", "book-1", "FX", "Euro / Dollar"}));
  CHECK(s->listing(kEurusd)->identifier.empty());
  // A fund by its ISIN, in a market of its own.
  CHECK(s->listed(kFund) && s->kind(kFund.base) == InstrumentKind::Fund && s->isin(kFund.base) == "US0378331005");
}

TEST(loading_the_same_list_again_makes_no_new_version_and_tells_nobody) {
  Loaded w;
  const uint64_t version = w.store->snapshot()->version();
  const int changes = w.listener.changes;
  w.source.load(*w.store, w.server, w.listener);
  CHECK(w.store->snapshot()->version() == version);
  CHECK(w.listener.changes == changes && w.listener.messages.empty());
  CHECK(w.store->snapshot()->quoteRowCount() == 5);
}

TEST(an_instrument_that_cannot_be_made_is_skipped_with_one_message_and_the_rest_go_on) {
  Server server;
  json bad = server.instruments["instruments"][0];  // EUR/USD
  bad["id"] = "EUR/USD/XXXX/BADKIND";
  bad["edge"]["dataset"] = "BADKIND";
  bad["base"]["kind"] = "bond";  // a kind the model has not got
  json refused = server.instruments["instruments"][3];  // the S&P 500
  refused["id"] = "US78378X1073/PTS/XXXX/HISTDATA";  // a check digit that is wrong
  refused["edge"]["base"] = "US78378X1073";
  refused["base"]["isin"] = "US78378X1073";
  server.instruments["instruments"].push_back(bad);
  server.instruments["instruments"].push_back(refused);
  auto store = plainStore();
  Listener listener;
  MarketServerSource source{[] {
    MarketServerSource::Options o;
    o.url = kUrl;
    return o;
  }()};
  source.load(*store, server, listener);
  CHECK(store->snapshot()->quoteRowCount() == 5);  // the five good ones
  CHECK(listener.messages.size() == 2);
  CHECK(listener.messages[0].find("BADKIND") != std::string::npos && listener.messages[0].find("bond") != std::string::npos);
  CHECK(listener.messages[1].find("US78378X1073") != std::string::npos);
  // Said once each, not at every look.
  source.load(*store, server, listener);
  CHECK(listener.messages.size() == 2);
}

// ---- The bars

TEST(each_resolution_is_written_with_the_servers_keys_and_all_five_fields_and_undefined_where_null) {
  Loaded w;
  struct Case {
    int request;
    std::vector<Resolution> resolutions;
  };
  for (const Case & c : {Case{MarketServerHistory::Intraday, {Resolution::Min5, Resolution::Day}}, Case{MarketServerHistory::Hourly, {Resolution::Hour1}},
                         Case{MarketServerHistory::Daily, {Resolution::Day}}, Case{MarketServerHistory::Weekly, {Resolution::Week}}}) {
    const auto outcomes = loadBatch(w, {kEurusd, kGold, kSpx}, c.request);
    CHECK(outcomes == std::vector<Outcome>(3, Outcome::Ok));
    const auto s = w.store->snapshot();
    const json answer = fixtureJson(c.request == MarketServerHistory::Hourly ? "summary_1h.json" : c.request == MarketServerHistory::Weekly ? "summary_1w.json" : c.request == MarketServerHistory::Daily ? "summary_1d.json" : "summary_5m_1d.json");
    for (const Resolution r : c.resolutions)
      for (const json & i : answer["instruments"]) {
        if (!i["queues"].contains(name(r)) || i["id"] != marketServerEdgeId(kEurusd)) continue;
        const json & q = i["queues"][name(r)];
        const auto bars = s->candles(kEurusd, r).candles;
        CHECK(bars.size() == q["keys"].size() && bars.size() > 1);
        for (size_t k = 0; k < bars.size(); k++) {
          CHECK(bars[k].time == q["keys"][k].get<int64_t>());
          CHECK(bars[k].open == q["open"][k].get<double>() && bars[k].high == q["high"][k].get<double>() && bars[k].low == q["low"][k].get<double>() &&
                bars[k].close == q["close"][k].get<double>());
          CHECK(q["volume"][k].is_null() && !isDefined(bars[k].volume));  // HistData's have none: undefined, not 0
        }
        CHECK(s->coverage(kEurusd, r) && s->coverage(kEurusd, r)->kind == QueueKind::Candle);
      }
  }
  // Only what was asked for: the request for the weekly bars wrote no hourly ones, and the three datasets' edges are untouched.
  CHECK(w.server.summaryUrls.size() == 4);
  CHECK(w.server.summaryUrls[3].find("resolution=1w") != std::string::npos);
  CHECK(w.store->snapshot()->queues(kEurusdOther).empty());
}

TEST(a_scalar_queue_is_written_as_scalar_and_a_null_open_stays_undefined) {
  Loaded w;
  CHECK(loadBatch(w, {kFund, kEurusdOther}, MarketServerHistory::Daily) == std::vector<Outcome>(2, Outcome::Ok));
  const auto s = w.store->snapshot();
  CHECK(s->coverage(kFund, Resolution::Day)->kind == QueueKind::Scalar);
  const auto bars = s->candles(kFund, Resolution::Day).candles;
  CHECK(bars.size() == 3 && bars[0].close == 100.5 && bars[2].close == 101.0);
  CHECK(!isDefined(bars[0].open) && !isDefined(bars[0].high) && !isDefined(bars[0].low) && !isDefined(bars[0].volume));  // never made up
  CHECK(s->coverage(kEurusdOther, Resolution::Day)->kind == QueueKind::Candle);
  const auto other = s->candles(kEurusdOther, Resolution::Day).candles;
  CHECK(other.size() == 2 && other[0].volume == 1000 && !isDefined(other[1].volume) && other[1].open == 1.15);
}

TEST(the_price_the_changes_and_the_ranges_come_from_the_bars_and_the_price_is_as_old_as_its_bar) {
  Loaded w;
  // A quarter of daily bars as a server sends them: the close rises, the high rises and the low falls, so that the range is the last bar's.
  const int64_t last = dayNumber(2026, 11, 13);
  json queue = {{"kind", "candle"}, {"keys", json::array()}, {"open", json::array()}, {"high", json::array()}, {"low", json::array()}, {"close", json::array()}, {"volume", json::array()}};
  for (int i = 0; i < 100; i++) {
    queue["keys"].push_back(last - 99 + i);
    queue["open"].push_back(i == 50 ? json(nullptr) : json(75.0 + i));
    queue["high"].push_back(100.0 + i);
    queue["low"].push_back(150.0 - i);
    queue["close"].push_back(75.0 + i);
    queue["volume"].push_back(nullptr);
  }
  queue["count"] = 100;
  for (json & i : w.server.everything["instruments"])
    if (i["id"] == marketServerEdgeId(kEurusd)) i["queues"]["1d"] = queue;
  CHECK(loadBatch(w, {kEurusd}, MarketServerHistory::Daily) == std::vector<Outcome>(1, Outcome::Ok));
  const auto s = w.store->snapshot();
  const size_t quarter = static_cast<size_t>(Period::Quarter);
  CHECK(s->quote(kEurusd) && s->quote(kEurusd)->last == 174.0);  // the newest bar's close
  CHECK(s->ranges(kEurusd).high[quarter] == 199.0 && s->ranges(kEurusd).low[quarter] == 51.0);
  const PeriodView view = periodView(*s, kEurusd, Period::Quarter, false);
  CHECK(isDefined(view.change()) && view.change() > 0 && s->changes(kEurusd).pct[quarter] == view.change());
  // The age of the price is the age of its bar, not of the fetch: never a "now".
  CHECK(s->quote(kEurusd)->time == (last - kUnixEpochDayNumber) * 86400);
  CHECK(!isDefined(s->candles(kEurusd, Resolution::Day).candles[50].open));
}

// ---- The answer's three cases

TEST(an_answer_with_a_queue_a_missing_id_and_an_edge_without_the_queue_gives_each_edge_its_own_outcome) {
  Loaded w;
  w.server.everything["instruments"].erase(
      std::find_if(w.server.everything["instruments"].begin(), w.server.everything["instruments"].end(), [](const json & e) { return e["id"] == "US78378X1072/PTS/XXXX/HISTDATA"; }));
  w.server.unloaded.insert(marketServerEdgeId(kGold));
  const auto outcomes = loadBatch(w, {kEurusd, kSpx, kGold}, MarketServerHistory::Daily);
  CHECK(outcomes == (std::vector<Outcome>{Outcome::Ok, Outcome::Missing, Outcome::Offline}));
  CHECK(w.store->snapshot()->coverage(kEurusd, Resolution::Day));
  CHECK(!w.store->snapshot()->coverage(kGold, Resolution::Day) && !w.store->snapshot()->coverage(kSpx, Resolution::Day));
  CHECK(w.server.summaryUrls.size() == 1);  // all three in one request
  // The server's request is the ids and the resolution, the ids' "/" encoded.
  CHECK(w.server.summaryUrls[0].find("edge=EUR%2FUSD%2FXXXX%2FHISTDATA,US78378X1072%2FPTS%2FXXXX%2FHISTDATA,XC0009655157%2FUSD%2FXXXX%2FHISTDATA&resolution=1d") != std::string::npos);
}

TEST(an_edge_listed_without_its_queue_is_asked_again_a_few_times_and_then_given_up) {
  Loaded w;
  w.server.unloaded.insert(marketServerEdgeId(kGold));
  for (int tries = 1; tries < MarketServerHistory::kTries; tries++) CHECK(loadBatch(w, {kGold}, MarketServerHistory::Daily)[0] == Outcome::Offline);
  CHECK(loadBatch(w, {kGold}, MarketServerHistory::Daily)[0] == Outcome::Missing);  // not for ever
  // Loaded in the meantime, it is Ok and the count starts over.
  w.server.unloaded.clear();
  CHECK(loadBatch(w, {kGold}, MarketServerHistory::Daily)[0] == Outcome::Ok);
  w.server.unloaded.insert(marketServerEdgeId(kGold));
  CHECK(loadBatch(w, {kGold}, MarketServerHistory::Daily)[0] == Outcome::Offline);
}

TEST(the_loader_asks_again_for_an_edge_not_loaded_yet_after_its_pause_and_stops_after_the_tries) {
  Loaded w;
  w.server.unloaded.insert(marketServerEdgeId(kGold));
  HistoryLoader::Options options;
  options.pause = std::chrono::milliseconds(0);
  options.offline_pause = std::chrono::milliseconds(10);
  struct Borrowed : Http {
    FakeHttp * inner = nullptr;
    HttpResult get(const std::string & url, const HttpHeaders & h) override { return inner->get(url, h); }
  };
  auto borrowed = std::make_unique<Borrowed>();
  borrowed->inner = &w.server;
  HistoryLoader loader(std::shared_ptr<Store>(w.store.get(), [](Store *) {}), std::move(borrowed), options, [] {});
  loader.addProvider(w.source.historyProvider());
  loader.want({kGold, kEurusd});  // the default period: a quarter, on the daily bars
  loader.waitIdle();
  // The edge that is loaded is done at once; the other is asked for, again and again, kTries in all, and no more.
  int goldRequests = 0;
  for (const std::string & u : w.server.summaryUrls)
    if (u.find("XC0009655157") != std::string::npos) goldRequests++;
  CHECK(goldRequests == MarketServerHistory::kTries);
  CHECK(w.store->snapshot()->coverage(kEurusd, Resolution::Day));
  loader.want({kGold, kEurusd});
  loader.waitIdle();
  CHECK(w.server.summaryUrls.size() == static_cast<size_t>(MarketServerHistory::kTries));  // given up: not asked again this session
}

// ---- The requests

TEST(a_period_asks_for_the_resolution_its_sparkline_and_change_are_drawn_from_and_periods_share_a_request) {
  using R = MarketServerHistory;
  CHECK(R::requestOf(Period::Month) == R::requestOf(Period::Quarter) && R::requestOf(Period::Month) == R::Daily);
  CHECK(R::requestOf(Period::Year) == R::requestOf(Period::Year3) && R::requestOf(Period::Year) == R::requestOf(Period::Year10) && R::requestOf(Period::Year) == R::Weekly);
  CHECK(R::requestOf(Period::Week) == R::Hourly && R::requestOf(Period::Day) == R::Intraday);
  CHECK(R::resolutionsOf(R::Daily) == std::vector<Resolution>{Resolution::Day});
  CHECK(R::resolutionsOf(R::Weekly) == std::vector<Resolution>{Resolution::Week});
  CHECK(R::resolutionsOf(R::Hourly) == std::vector<Resolution>{Resolution::Hour1});
  CHECK((R::resolutionsOf(R::Intraday) == std::vector<Resolution>{Resolution::Min5, Resolution::Day}));  // the day's bars and the daily bar its change is measured from
  Loaded w;
  const auto provider = w.source.historyProvider();
  const auto snapshot = w.store->snapshot();
  const auto need = provider->need(*snapshot, kEurusd, Period::Quarter);
  CHECK(need && need->request == R::Daily && !need->stored);
  CHECK(provider->need(*snapshot, kEurusd, Period::Month)->request == need->request);
  CHECK(provider->batchSize() == 50 && provider->lifetime(R::Daily) == std::chrono::minutes(5));
  // The windows of the server's queues: the model's.
  for (Resolution r : {Resolution::Min5, Resolution::Hour1, Resolution::Day, Resolution::Week}) CHECK(provider->keep(r) == defaultKeep(r));
  CHECK(!provider->keep(Resolution::Min1) && !provider->keep(Resolution::Month));
  // It serves every dataset the server has, once /instruments is read.
  CHECK((provider->datasets() == std::vector<std::string>{"HISTDATA", "OTHERDS"}));
}

TEST(the_loader_gives_the_edges_on_the_screen_first_in_batches_of_fifty_of_the_servers_provider) {
  Loaded w;
  // 120 edges of the dataset, as a server of 120 instruments has (made as the source does, so that they are rows).
  std::vector<EdgeKey> edges;
  w.store->batch([&](Batch & b) {
    const DatasetId ds = b.addDataset("HISTDATA");
    const MarketId market = b.addMarket("XXXX");
    const InstrumentId usd = currencyNode(b, "USD");
    for (int i = 0; i < 120; i++) {
      const InstrumentId base = b.addInstrument("", "Fund " + std::to_string(i), InstrumentKind::Fund, testIsin(100000 + i));
      edges.push_back({base, usd, market, ds});
      b.setListing({edges.back(), "F" + std::to_string(i), "", "", "FUND", "Fund"});
      b.addRow(edges.back());
    }
  });
  HistoryLoader::Options options;
  options.pause = std::chrono::milliseconds(0);
  struct Borrowed : Http {
    FakeHttp * inner = nullptr;
    HttpResult get(const std::string & url, const HttpHeaders & h) override { return inner->get(url, h); }
  };
  auto borrowed = std::make_unique<Borrowed>();
  borrowed->inner = &w.server;
  HistoryLoader loader(std::shared_ptr<Store>(w.store.get(), [](Store *) {}), std::move(borrowed), options, [] {});
  loader.addProvider(w.source.historyProvider());
  // The edges on the screen (the last twenty) are first in what is wanted: they are in the first request.
  std::vector<EdgeKey> wanted(edges.end() - 20, edges.end());
  wanted.insert(wanted.end(), edges.begin(), edges.end() - 20);
  loader.want(wanted);
  loader.waitIdle();
  CHECK(w.server.summaryUrls.size() == 3);  // 50, 50 and 20 of the 120
  const std::string first = w.server.summaryUrls[0];
  for (size_t i = 100; i < 120; i++) CHECK(first.find(text(NodeKey{edges[i].base.value})) != std::string::npos);
  CHECK(first.find(text(NodeKey{edges[0].base.value})) != std::string::npos);  // and the next of the rest, in their order
  CHECK(first.find(text(NodeKey{edges[29].base.value})) != std::string::npos && first.find(text(NodeKey{edges[30].base.value})) == std::string::npos);
}

// ---- The rule

namespace {
struct Other : Source {
  std::string n;
  explicit Other(std::string name) : n(std::move(name)) {}
  std::string name() const override { return n; }
  std::string description() const override { return ""; }
  void load(Store &, Http &, SourceListener &) override {}
};
}  // namespace

TEST(the_server_supplies_all_data_so_it_runs_alone_and_any_other_source_is_refused_with_the_reason) {
  std::vector<std::unique_ptr<Source>> alone;
  alone.push_back(std::make_unique<MarketServerSource>());
  CHECK(suppliesAllDataClash(alone).empty() && datasetCodeClash(alone).empty());
  CHECK(alone[0]->suppliesAllData() && alone[0]->datasetCodes().empty() && alone[0]->name() == "market-server");

  std::vector<std::unique_ptr<Source>> withOthers;
  withOthers.push_back(std::make_unique<EcbSource>());
  withOthers.push_back(std::make_unique<MarketServerSource>());
  withOthers.push_back(std::make_unique<SuomenPankkiSource>());
  const std::string why = suppliesAllDataClash(withOthers);
  CHECK(why.find("market-server supplies every dataset; it runs alone") != std::string::npos);
  CHECK(why.find("ecb") != std::string::npos && why.find("spfb") != std::string::npos);
  CHECK(datasetCodeClash(withOthers).empty());  // no dataset is written twice: the rule above is the one that refuses

  // The sources that supply nothing of the server's kind (any that has its own dataset) are the other way round: nothing is refused.
  std::vector<std::unique_ptr<Source>> sources;
  sources.push_back(std::make_unique<EcbSource>());
  sources.push_back(std::make_unique<SuomenPankkiSource>());
  CHECK(suppliesAllDataClash(sources).empty());
  // Even one that writes no dataset is not let in beside it, until a later mode says so.
  std::vector<std::unique_ptr<Source>> quiet;
  quiet.push_back(std::make_unique<MarketServerSource>());
  quiet.push_back(std::make_unique<Other>("quiet"));
  CHECK(!suppliesAllDataClash(quiet).empty());
}

// ---- The server down

TEST(a_server_that_is_down_is_said_once_the_rows_stay_and_nothing_goes_to_another_host) {
  Loaded w;
  const size_t rows = w.store->snapshot()->quoteRowCount();
  const uint64_t version = w.store->snapshot()->version();
  w.server.down = true;
  w.source.load(*w.store, w.server, w.listener);
  w.source.load(*w.store, w.server, w.listener);
  w.source.load(*w.store, w.server, w.listener);
  CHECK(w.listener.messages.size() == 1 && w.listener.messages[0].find("no answer") != std::string::npos);
  CHECK(w.store->snapshot()->quoteRowCount() == rows && w.store->snapshot()->version() == version);
  CHECK(w.source.refreshInterval() == std::chrono::seconds(30));  // soon again
  // The history: no answer for every edge of the batch, in one request, and nothing is said again or written.
  const auto outcomes = loadBatch(w, {kEurusd, kGold, kSpx}, MarketServerHistory::Daily);
  CHECK(outcomes == std::vector<Outcome>(3, Outcome::Offline));
  CHECK(w.listener.messages.size() == 1);
  // Every request went to the server, none to a provider.
  for (const std::string & u : w.server.urls) CHECK(u.rfind(kUrl, 0) == 0);
  // It is back: the rows are read again, and the next look is in the long interval.
  w.server.down = false;
  w.source.load(*w.store, w.server, w.listener);
  CHECK(w.source.refreshInterval() == std::chrono::minutes(10) && w.store->snapshot()->version() == version);
  w.server.down = true;
  w.source.load(*w.store, w.server, w.listener);
  CHECK(w.listener.messages.size() == 2);  // down again after it was up: said again
}

TEST(a_refusal_is_refused_and_a_wrong_request_is_a_bug_that_is_said_with_the_servers_words) {
  Loaded w;
  w.server.answer = [](const std::string &) { return failed(429); };
  CHECK(loadBatch(w, {kEurusd, kGold}, MarketServerHistory::Daily) == std::vector<Outcome>(2, Outcome::Refused));
  w.server.answer = [](const std::string &) {
    HttpResult r;
    r.status = 400;
    r.body = R"({"error": "no more than 100 edges in a request"})";
    return r;
  };
  try {
    loadBatch(w, {kEurusd}, MarketServerHistory::Daily);
    CHECK(false);
  } catch (const std::exception & e) {
    CHECK(std::string(e.what()).find("no more than 100 edges in a request") != std::string::npos);
  }
  Listener listener;
  MarketServerSource source;
  auto store = plainStore();
  FakeHttp http;
  http.answer = [](const std::string &) {
    HttpResult r;
    r.status = 400;
    r.body = R"({"error": "a malformed id"})";
    return r;
  };
  source.load(*store, http, listener);
  CHECK(listener.messages.size() == 1 && listener.messages[0].find("a malformed id") != std::string::npos);
}

TEST(an_answer_that_is_not_a_summary_writes_nothing_and_is_said) {
  Loaded w;
  w.server.answer = [](const std::string &) { return ok(R"({"instruments": [{"id": "x"}], "missing": 3})"); };
  try {
    loadBatch(w, {kEurusd}, MarketServerHistory::Daily);
    CHECK(false);
  } catch (const std::exception & e) {
    CHECK(std::string(e.what()).find("market-server") != std::string::npos);
  }
  CHECK(w.store->snapshot()->queues(kEurusd).empty());
}
