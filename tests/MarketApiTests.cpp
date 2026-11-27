// httplib first: the model declares a global seconds(Resolution) that its own `seconds` would otherwise be ambiguous with.
#include <httplib.h>

#include "TestFramework.h"
#include "MarketTestSupport.h"
#include "market/Api.h"
#include "market/Bars.h"
#include "model/Period.h"
#include "model/Store.h"

#include <nlohmann/json.hpp>

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

// A store with two instruments: eurusd with every resolution (the close of a bar its key and a half), gbpusd with two daily bars.
std::shared_ptr<Store> seededStore() {
  std::shared_ptr<Store> store = makeMemoryStore();
  store->batch([&](Batch & b) {
    const DatasetId dataset = b.addDataset("TESTDS", "Test");
    const MarketId market = b.addMarket("XXXX", "none");
    const InstrumentId usd = b.addInstrument("USD", "US dollar", InstrumentKind::Currency);
    const InstrumentId eur = b.addInstrument("EUR", "Euro", InstrumentKind::Currency);
    const InstrumentId gbp = b.addInstrument("GBP", "Pound", InstrumentKind::Currency);
    const EdgeKey eurusd{eur, usd, market, dataset}, gbpusd{gbp, usd, market, dataset};
    b.setListing({eurusd, "EURUSD", "", "", "CURRENCY", "EUR/USD"});
    b.setListing({gbpusd, "GBPUSD", "", "", "CURRENCY", "GBP/USD"});
    for (Resolution r : kResolutions) {
      std::vector<Candle> bars;
      for (int64_t k : windowKeys(r)) bars.push_back({k, static_cast<double>(k), k + 1.0, k - 1.0, static_cast<double>(k) + 0.5, kUndefined});
      b.appendCandles(eurusd, r, bars);
    }
    const int64_t day = barKey(Resolution::Day, kLast);
    b.appendCandles(gbpusd, Resolution::Day, {{day - 1, 1, 2, 0.5, 1.25, kUndefined}, {day, 1.25, 2, 1, 1.5, kUndefined}});
  });
  return store;
}

json get(Store & store, const std::string & path, const std::map<std::string, std::string> & params = {}, int * status = nullptr) {
  const ApiResponse r = handleApi(store, path, params);
  if (status) *status = r.status;
  if (r.status >= 500) std::fprintf(stderr, "  %s answered %d: %s\n", path.c_str(), r.status, r.body.c_str());
  return json::parse(r.body, nullptr, false);
}

// How many of the keys the period of the application takes: those within its days of the last bar's start.
size_t within(Resolution r, int days) {
  size_t n = 0;
  for (int64_t k : windowKeys(r))
    if (barStart(r, k) >= kLast - kLast % 300 - days * 86400LL) n++;
  return n;
}

}  // namespace

TEST(api_lists_the_instruments_with_their_first_and_last_bar) {
  const auto store = seededStore();
  int status = 0;
  const json health = get(*store, "/health", {}, &status);
  CHECK(status == 200 && health["status"] == "ok" && health["instruments"] == 2);
  const json instruments = get(*store, "/instruments");
  CHECK(instruments["instruments"].size() == 2);
  if (instruments["instruments"].size() != 2) return;
  const json & eur = instruments["instruments"][0];
  CHECK(eur["code"] == "eurusd" && eur["name"] == "EUR/USD" && eur["asset_class"] == "currency");
  CHECK(eur["base"] == "EUR" && eur["quote"] == "USD" && eur["market"] == "XXXX" && eur["dataset"] == "TESTDS");
  CHECK(eur["last_bar_time"] == barStart(Resolution::Min5, barKey(Resolution::Min5, kLast)));
  CHECK(eur["first_bar_time"] == barStart(Resolution::Week, windowKeys(Resolution::Week).front()));
  const json & gbp = instruments["instruments"][1];
  CHECK(gbp["code"] == "gbpusd" && gbp["first_bar_time"] == barStart(Resolution::Day, barKey(Resolution::Day, kLast) - 1));
}

TEST(api_summary_gives_each_period_the_bars_of_its_resolution_ending_at_the_last_bar) {
  const auto store = seededStore();
  int status = 0;
  CHECK(get(*store, "/summary")["instruments"].size() == 2);
  const json one = get(*store, "/summary", {{"codes", "eurusd"}});
  CHECK(one["instruments"].size() == 1);
  if (one["instruments"].size() != 1) return;
  const json & eur = one["instruments"][0];
  const Timestamp anchor = barStart(Resolution::Min5, barKey(Resolution::Min5, kLast));
  CHECK(eur["code"] == "eurusd" && eur["last_bar_time"] == anchor);
  const auto & p = eur["periods"];
  CHECK(p.size() == 7);
  CHECK(p["day"]["resolution"] == "5m" && p["day"]["keys"].size() == within(Resolution::Min5, 1) && p["day"]["close"].size() == p["day"]["keys"].size());
  CHECK(p["week"]["resolution"] == "1h" && p["week"]["keys"].size() == within(Resolution::Hour1, 7));
  CHECK(p["month"]["resolution"] == "1d" && p["month"]["keys"].size() == within(Resolution::Day, 30));
  CHECK(p["quarter"]["resolution"] == "1d" && p["quarter"]["keys"].size() == within(Resolution::Day, 90));
  CHECK(p["year"]["resolution"] == "1w" && p["year"]["keys"].size() == within(Resolution::Week, 365));
  CHECK(p["year3"]["resolution"] == "1w" && p["year3"]["keys"].size() == within(Resolution::Week, 1095));
  CHECK(p["year10"]["resolution"] == "1w" && p["year10"]["keys"].size() == within(Resolution::Week, 3650));
  // oldest first, each a key and its close, the last the bar of the last instant
  const int64_t five = barKey(Resolution::Min5, kLast);
  CHECK(p["day"]["keys"].back() == five && p["day"]["close"].back() == five + 0.5);
  CHECK(p["year10"]["keys"].back() == barKey(Resolution::Week, kLast));
  CHECK(p["year10"]["keys"][1].get<int64_t>() - p["year10"]["keys"][0].get<int64_t>() == 7);  // a bar a week
  // the day's change is measured from the close of the last day before the day of the last bar
  const int64_t day = barKey(Resolution::Day, kLast);
  CHECK(p["day"]["previous_close"] == (day - 1) + 0.5);

  // a resolution that has not been loaded is an empty period, not an error
  const json gbp = get(*store, "/summary", {{"codes", "GBPUSD"}})["instruments"][0];
  CHECK(gbp["periods"]["day"]["keys"].empty() && gbp["periods"]["week"]["close"].empty() && gbp["periods"]["day"]["resolution"].is_null());
  CHECK(gbp["periods"]["day"]["previous_close"].is_null() || gbp["periods"]["day"]["previous_close"] == 1.25);
  CHECK(gbp["periods"]["month"]["keys"].size() == 2 && gbp["periods"]["year"]["keys"].size() == 2);  // the year is drawn from weeks or, as in the application, days

  const json two = get(*store, "/summary", {{"codes", "gbpusd, eurusd"}});
  CHECK(two["instruments"].size() == 2 && two["instruments"][0]["code"] == "gbpusd");
  CHECK(get(*store, "/summary", {{"codes", "nonesuch"}}, &status)["error"].is_string() && status == 404);
  CHECK(get(*store, "/nowhere", {}, &status)["error"].is_string() && status == 404);
}

TEST(api_over_http_is_read_only_json) {
  std::shared_ptr<Store> store = seededStore();
  ApiServer server([&] { return store; });
  std::string error;
  CHECK(server.bind("127.0.0.1", 0, error));
  std::thread thread([&] { server.serve(); });
  httplib::Client client("127.0.0.1", server.port());
  auto health = client.Get("/health");
  CHECK(health && health->status == 200 && health->get_header_value("Content-Type").find("application/json") == 0);
  auto summary = client.Get("/summary?codes=eurusd,gbpusd");
  CHECK(summary && summary->status == 200 && json::parse(summary->body)["instruments"].size() == 2);
  auto post = client.Post("/summary", "x", "text/plain");
  CHECK(post && post->status == 405);
  auto del = client.Delete("/instruments");
  CHECK(del && del->status == 405);
  auto missing = client.Get("/raw-bars");
  CHECK(missing && missing->status == 404);
  server.stop();
  thread.join();
}
