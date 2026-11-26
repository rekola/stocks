// httplib first: the model declares a global seconds(Resolution) that its own `seconds` would otherwise be ambiguous with.
#include <httplib.h>

#include "TestFramework.h"
#include "MarketTestSupport.h"
#include "market/Api.h"
#include "market/Store.h"
#include "model/NodeKey.h"

#include <nlohmann/json.hpp>

#include <thread>

using namespace market;
using nlohmann::json;

namespace {

Edge edgeOf(const char * base, const char * quote) {
  return {currencyKey(base)->value, currencyKey(quote)->value, marketKey("XXXX")->value, datasetKey("histdata")->value};
}

const Timestamp kLast = 1794607080;  // 2026-11-13 21:58:00 UTC

// A small seeded database: eurusd with every level (a bar of every slot of the stored period, the close its key plus a half), gbpusd
// with two daily bars only, usdjpy with nothing loaded.
void seed(MarketStore & store) {
  const uint64_t dataset = store.putDataset("histdata", "HistData");
  const uint64_t v = store.beginBatch(dataset, "seed");
  store.putInstrument({edgeOf("EUR", "USD"), "eurusd", "EUR/USD", "currency", 200005});
  store.putInstrument({edgeOf("GBP", "USD"), "gbpusd", "GBP/USD", "currency", 200005});
  store.putInstrument({edgeOf("USD", "JPY"), "usdjpy", "USD/JPY", "currency", 200005});
  for (Resolution level : kLevels) {
    const int64_t last = barKey(level, kLast);
    std::vector<Bar> bars;
    for (int64_t k = windowFirstKey(level, last); k <= last; k++) bars.push_back({k, static_cast<double>(k), k + 1.0, k - 1.0, static_cast<double>(k) + 0.5});
    store.replaceBars(edgeOf("EUR", "USD"), level, bars, kLast, v);
  }
  const int64_t day = barKey(Resolution::Day, kLast);
  store.replaceBars(edgeOf("GBP", "USD"), Resolution::Day, {{day - 1, 1, 2, 0.5, 1.25}, {day, 1.25, 2, 1, 1.5}}, kLast, v);
}

json get(MarketStore & store, const std::string & path, const std::map<std::string, std::string> & params = {}, int * status = nullptr) {
  const ApiResponse r = handleApi(store, path, params);
  if (status) *status = r.status;
  return json::parse(r.body, nullptr, false);
}

void apiContract(MarketStore & store) {
  int status = 0;
  CHECK(get(store, "/health", {}, &status)["status"] == "ok" && status == 200);

  const json instruments = get(store, "/instruments");
  CHECK(instruments["instruments"].size() == 3);
  const json & eur = instruments["instruments"][0];
  CHECK(eur["code"] == "eurusd" && eur["name"] == "EUR/USD" && eur["asset_class"] == "currency");
  CHECK(eur["base"] == "EUR" && eur["quote"] == "USD" && eur["market"] == "XXXX" && eur["dataset"] == "histdata");
  CHECK(eur["last_bar_time"] == kLast);
  CHECK(eur["first_bar_time"] == barStart(Resolution::Day, windowFirstKey(Resolution::Day, barKey(Resolution::Day, kLast))));
  CHECK(instruments["instruments"][1]["first_bar_time"] == barStart(Resolution::Day, barKey(Resolution::Day, kLast) - 1));
  CHECK(instruments["instruments"][2]["first_bar_time"].is_null() && instruments["instruments"][2]["last_bar_time"].is_null());
}

void summaryContract(MarketStore & store) {
  int status = 0;
  CHECK(get(store, "/summary")["instruments"].size() == 3);
  const json one = get(store, "/summary", {{"codes", "eurusd"}});
  CHECK(one["instruments"].size() == 1);
  const json & eur = one["instruments"][0];
  CHECK(eur["code"] == "eurusd" && eur["last_bar_time"] == kLast);
  const auto & w = eur["windows"];
  CHECK(w.size() == 7);
  CHECK(w["day"]["level"] == "5m" && w["day"]["keys"].size() == 288 && w["day"]["close"].size() == 288);
  CHECK(w["week"]["level"] == "1h" && w["week"]["keys"].size() == 168);
  CHECK(w["month"]["level"] == "1h" && w["month"]["keys"].size() == 720);
  CHECK(w["3months"]["level"] == "1d" && w["3months"]["keys"].size() == 90);
  CHECK(w["1year"]["keys"].size() == 365 && w["3years"]["keys"].size() == 1095 && w["10years"]["keys"].size() == 3650);
  // each window ends at the last bar, oldest first, a key and its close
  const int64_t day = barKey(Resolution::Day, kLast), hour = barKey(Resolution::Hour1, kLast), five = barKey(Resolution::Min5, kLast);
  CHECK(w["day"]["keys"].back() == five && w["day"]["close"].back() == five + 0.5);
  CHECK(w["week"]["keys"].back() == hour && w["week"]["keys"][0] == hour - 167);
  CHECK(w["10years"]["keys"].back() == day && w["10years"]["keys"][0] == day - 3649);
  CHECK(w["3months"]["close"][0] == day - 89 + 0.5);
  // the day's change is measured from the close of the day before the last bar's
  CHECK(w["day"]["previous_close"] == (day - 1) + 0.5);

  // a level that has not been loaded is an empty window, not an error
  const json gbp = get(store, "/summary", {{"codes", "GBPUSD"}})["instruments"][0];
  CHECK(gbp["windows"]["day"]["keys"].empty() && gbp["windows"]["week"]["close"].empty() && gbp["windows"]["day"]["previous_close"].is_null());
  CHECK(gbp["windows"]["3months"]["keys"].size() == 2);
  const json jpy = get(store, "/summary", {{"codes", "usdjpy"}})["instruments"][0];
  CHECK(jpy["last_bar_time"].is_null() && jpy["windows"].size() == 7 && jpy["windows"]["10years"]["keys"].empty());

  const json two = get(store, "/summary", {{"codes", "gbpusd, eurusd"}});
  CHECK(two["instruments"].size() == 2 && two["instruments"][0]["code"] == "gbpusd");
  CHECK(get(store, "/summary", {{"codes", "nonesuch"}}, &status)["error"].is_string() && status == 404);
  CHECK(get(store, "/nowhere", {}, &status)["error"].is_string() && status == 404);
}

}  // namespace

TEST(api_over_the_memory_store) {
  auto store = makeMemoryStore();
  seed(*store);
  apiContract(*store);
  summaryContract(*store);
}

TEST(api_over_a_seeded_mariadb) {
  ScratchDb db;
  if (!db.available) return skipWithoutDatabase();
  auto store = openMariaDbStore(db.config, OpenMode::ReadWrite);
  seed(*store);
  apiContract(*store);
  summaryContract(*store);
}

TEST(api_over_http_is_read_only_json) {
  auto store = makeMemoryStore();
  seed(*store);
  ApiServer server(*store);
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
