#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "AttributeView.h"
#include "history/HistoryLoader.h"
#include "model/Isin.h"
#include "source/FundSource.h"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <sstream>

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// The schemas of the tests (tests/fixtures/funds/schema-NAME.json): one whose answers are lists with a details request, one with a token and rows in an object.
FundSchema schema(const std::string & name) {
  FundSchema out;
  std::string error;
  CHECK(FundSchema::load(std::string(STOCKS_FIXTURES_DIR) + "/funds/schema-" + name + ".json", out, error));
  out.pause = std::chrono::milliseconds(0);
  return out;
}

// The day the tests say it is: the day after the latest NAVs of the fixtures.
FundSource source(const std::string & name) {
  FundSource s(schema(name));
  s.setToday([] { return dayNumber(2026, 11, 20); });
  return s;
}

// A loader owns its Http; a test keeps its FakeHttp to look at what it was asked.
struct Borrowed : Http {
  FakeHttp * inner = nullptr;
  HttpResult get(const std::string & url, const HttpHeaders & h) override { return inner->get(url, h); }
};
std::unique_ptr<Http> borrow(FakeHttp & http) {
  auto b = std::make_unique<Borrowed>();
  b->inner = &http;
  return b;
}

HistoryLoader::Options quickly() {
  HistoryLoader::Options o;
  o.pause = std::chrono::milliseconds(0);
  o.offline_pause = std::chrono::milliseconds(20);
  return o;
}

bool said(const Listener & l, const std::string & part) {
  for (const std::string & m : l.messages)
    if (m.find(part) != std::string::npos) return true;
  return false;
}

bool asked(const FakeHttp & http, const std::string & part) {
  for (const std::string & u : http.urls)
    if (u.find(part) != std::string::npos) return true;
  return false;
}

std::optional<EdgeKey> edgeOf(const Snapshot & s, const char * isin, const char * market, const char * dataset) {
  auto b = s.findInstrumentByIsin(isin);
  auto m = s.findMarket(market);
  auto d = s.findDataset(dataset);
  auto q = s.findInstrument("EUR");
  if (!b || !m || !d || !q) return std::nullopt;
  return EdgeKey{b->id, q->id, m->id, d->id};
}

}  // namespace

TEST(the_isin_table_is_valid_and_luxembourg_ones_come_from_the_id) {
  FundSchema f = schema("alpha");
  int wanted = 0;
  for (const auto & [id, isin] : f.isins)
    if (!isin.empty()) {
      CHECK(isValidIsin(isin));
      wanted++;
    }
  CHECK(wanted == 2);
  FundSource s = source("alpha");
  std::vector<FundSource::Entry> entries;
  std::string error;
  CHECK(s.parseList(R"([{"productId":"LU0823428346EUR"},{"productId":"ALPHANORDIC"},{"productId":"ALPHAREALTY"},{"productId":"NOSUCH"}])", entries, error));
  CHECK(entries.size() == 4);
  CHECK(entries[0].fund.isin == "LU0823428346" && entries[0].wanted);
  CHECK(entries[1].fund.isin == "FI0008810569");
  CHECK(!entries[2].wanted);  // known, not wanted
  CHECK(entries[3].fund.isin.empty() && entries[3].wanted);
}

TEST(a_list_of_rows_gives_the_funds_with_their_ratings_and_fees_in_per_cent) {
  FundSource s = source("alpha");
  std::vector<FundSource::Entry> entries;
  std::string error;
  CHECK(s.parseList(fixture("funds/alpha-list.json"), entries, error));
  CHECK(entries.size() == 6);
  CHECK(entries[0].fund.isin == "FI0008810569" && entries[0].id == "ALPHANORDIC" && entries[0].fund.symbol == "ALPHANORDIC");
  CHECK_NEAR(*entries[0].fund.managementFee, 1.53, 1e-9);
  CHECK(*entries[0].fund.risk == 6 && *entries[0].fund.rating == 4 && *entries[0].fund.esg == 3);
  // The latest NAV, its day and its change from the one before.
  CHECK(*entries[0].fund.price == 3.3099 && *entries[0].fund.day == dayNumber(2026, 11, 19) && *entries[0].fund.change == -0.005887);
  CHECK(!entries[2].wanted);
  CHECK(!entries[5].fund.price && !entries[5].fund.day);
  CHECK(!s.parseList("{}", entries, error));
}

TEST(the_online_fee_is_preferred_to_the_ordinary_one) {
  FundSource s = source("alpha");
  FundSource::Entry e;
  std::string error;
  CHECK(s.parseDetail(fixture("funds/alpha-details.json"), e, error));
  CHECK(e.fund.subscriptionFee && *e.fund.subscriptionFee == 0.0);  // the online fee, not the ordinary 1 %
  CHECK(e.fund.redemptionFee && *e.fund.redemptionFee == 1.0);
  CHECK(!s.parseDetail("[]", e, error));
}

TEST(an_isin_the_list_does_not_give_is_found_in_the_details_and_said) {
  FundSource s = source("alpha");
  FundSource::Entry e;
  e.id = "ALPHANEWFUND";
  std::string error;
  CHECK(s.parseDetail(R"({"values":{"redemptionFee":{"value":1}},"static":{"deep":["x",{"code":"FI4000297817"}]}})", e, error));
  CHECK(e.fund.isin == "FI4000297817" && e.note.find("/static/deep/1/code") != std::string::npos);
  FundSource::Entry other;
  other.fund.isin = "FI0008810569";  // one it has is not replaced
  CHECK(s.parseDetail(R"({"x":"FI4000297817"})", other, error));
  CHECK(other.fund.isin == "FI0008810569" && other.note.empty());
}

TEST(an_isin_in_the_list_row_is_found_where_the_schema_names_no_field) {
  FundSchema f = schema("alpha");
  f.isins.clear();
  FundSource s(f);
  std::vector<FundSource::Entry> entries;
  std::string error;
  CHECK(s.parseList(R"([{"productId":"X1","info":{"code":"FI0008810569"}}])", entries, error));
  CHECK(entries.size() == 1 && entries[0].fund.isin == "FI0008810569" && !entries[0].note.empty());
}

TEST(the_funds_are_shown_at_once_from_the_list_alone) {
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("alpha.example/list") != std::string::npos) return ok(fixture("funds/alpha-list.json"));
    if (url.find("alpha.example/details/ALPHANEWFUND") != std::string::npos) return ok(R"({"values":{},"identifiers":{"isin":"FI4000297817"}})");
    return failed(404);
  };
  auto store = makeMemoryStore();
  Listener listener;
  FundSource funds = source("alpha");
  funds.load(*store, http, listener);
  auto s = store->snapshot();
  auto edge = edgeOf(*s, "FI0008810569", "Alpha Funds", "alpha");
  CHECK(edge.has_value());
  if (!edge) return;
  auto bars = s->candles(*edge, Resolution::Day).candles;
  CHECK(bars.size() == 1);  // the latest NAV: a close and nothing else
  CHECK(bars[0].time == dayNumber(2026, 11, 19) && bars[0].close == 3.3099 && !isDefined(bars[0].open) && !isDefined(bars[0].volume));
  auto previous = s->previousClose(*edge, dayNumber(2026, 11, 19));
  CHECK(previous && std::fabs(*previous - 3.3099 / (1 - 0.005887)) < 1e-9);
  CHECK(s->listing(*edge)->identifier == "ALPHANORDIC");
  CHECK(s->findInstrumentByIsin("FI0008810569")->kind == InstrumentKind::Fund);
  CHECK(edgeOf(*s, "LU0823428346", "Alpha Funds", "alpha").has_value());  // the ISIN in the id
  CHECK(edgeOf(*s, "FI4000297817", "Alpha Funds", "alpha").has_value());  // the ISIN in the details
  CHECK(said(listener, "found in the details at /identifiers/isin"));
  CHECK(s->instruments().size() == 4);  // two funds, the one found in the details, the euro: not the unwanted, the old or the unpriced
  CHECK(said(listener, "1 left out for an old NAV") && said(listener, "1 for no price"));
  CHECK(!asked(http, "rates") && !asked(http, "details/ALPHANORDIC"));  // no history, no details, until a fund is wanted
  CHECK(listener.changes >= 1);
}

TEST(the_history_of_a_fund_is_loaded_when_it_is_wanted_and_once) {
  FakeHttp http, history;
  http.answer = [](const std::string & url) {
    return url.find("alpha.example/list") != std::string::npos ? ok(fixture("funds/alpha-list.json")) : failed(404);
  };
  history.answer = [](const std::string & url) {
    if (url.find("alpha.example/details/ALPHANORDIC") != std::string::npos) return ok(fixture("funds/alpha-details.json"));
    if (url.find("alpha.example/rates/ALPHANORDIC") != std::string::npos) return ok(fixture("funds/alpha-rates-daily.json"));
    return failed(404);
  };
  std::shared_ptr<Store> store = makeMemoryStore();
  Listener listener;
  FundSource funds = source("alpha");
  funds.load(*store, http, listener);
  auto edge = edgeOf(*store->snapshot(), "FI0008810569", "Alpha Funds", "alpha");
  CHECK(edge.has_value());
  if (!edge) return;

  std::atomic<int> changes = 0;
  HistoryLoader loader(store, borrow(history), quickly(), [&] { changes++; });
  loader.addProvider(funds.historyProvider());
  loader.want({*edge});
  loader.waitIdle();
  auto s = store->snapshot();
  auto bars = s->candles(*edge, Resolution::Day).candles;
  CHECK(bars.size() == 5);  // the history's four (the later of two on one day stands) and the list's NAV of the day
  CHECK(bars[2].close == 10.25 && bars[0].time == dayNumber(2025, 12, 30) && bars[4].time == dayNumber(2026, 11, 19));
  CHECK(changes >= 1);
  // The details came with it: the fees, beside what the list said.
  auto base = *s->findInstrumentByIsin("FI0008810569");
  auto fee = s->attribute(base.id, "redemption-fee");
  CHECK(fee && asReal(*fee) == 1.0);  // a fraction in the answer, per cent here
  auto online = s->attribute(base.id, "subscription-fee");
  CHECK(online && asReal(*online) == 0.0);  // the online fee, not the ordinary 1 %
  auto page = s->attribute(base.id, predicate::webUrl);
  CHECK(page && asText(*page) && asText(*page)->rfind("https://", 0) == 0 && asText(*page)->find("/sijoituskohteet/") != std::string::npos);  // the product page of the answer
  auto assets = s->attribute(base.id, "net-assets");
  CHECK(assets && std::fabs(*asReal(*assets) - 43998919.16) < 1e-6);
  auto founded = s->attribute(base.id, "start-day");
  CHECK(founded && asInteger(*founded) == dayNumber(2023, 8, 15));  // a date
  CHECK(history.urls.size() == 2 && asked(history, "details/ALPHANORDIC") && asked(history, "rates/ALPHANORDIC"));
  // The info panel shows what is said of the fund.
  std::string shown;
  for (const InfoSection & section : buildInfo(*s, *edge))
    for (const InfoRow & row : section.rows) shown += section.title + "/" + row.predicate + "=" + row.value.text + "\n";
  CHECK(shown.find("Instrument/redemption-fee=1") != std::string::npos);
  CHECK(shown.find("Instrument/management-fee=1.53") != std::string::npos);
  CHECK(shown.find("Instrument/risk-class=6") != std::string::npos);
  CHECK(shown.find("Instrument/morningstar-rating=4") != std::string::npos);
  loader.want({*edge});  // not again
  loader.waitIdle();
  CHECK(history.urls.size() == 2);
}

TEST(a_refusal_stops_the_provider_and_not_the_other_funds_of_the_screen_loader) {
  FakeHttp http, history;
  http.answer = [](const std::string & url) {
    return url.find("alpha.example/list") != std::string::npos ? ok(fixture("funds/alpha-list.json")) : failed(404);
  };
  history.answer = [](const std::string &) { return failed(429); };
  std::shared_ptr<Store> store = makeMemoryStore();
  Listener listener;
  FundSource funds = source("alpha");
  funds.load(*store, http, listener);
  auto s = store->snapshot();
  auto a = edgeOf(*s, "FI0008810569", "Alpha Funds", "alpha"), b = edgeOf(*s, "LU0823428346", "Alpha Funds", "alpha");
  CHECK(a && b);
  if (!a || !b) return;
  std::vector<std::string> messages;
  std::mutex m;
  HistoryLoader loader(store, borrow(history), quickly(), [] {}, [&](std::string t) { std::lock_guard l(m); messages.push_back(t); });
  loader.addProvider(funds.historyProvider());
  loader.want({*a, *b});
  loader.waitIdle();
  CHECK(history.urls.size() == 1);  // the second fund is not asked for
  std::lock_guard l(m);
  CHECK(messages.size() == 1 && messages[0].find("alpha refused") != std::string::npos);
  CHECK(store->snapshot()->candles(*a, Resolution::Day).candles.size() == 1);  // what the list gave stays
}
TEST(an_op_page_gives_its_token_without_a_leading_ampersand) {
  FundSchema f = schema("beta");
  std::string token, error;
  CHECK(FundSource::parseToken(fixture("funds/beta-page.html"), f.tokenRegex, token, error));
  CHECK(token == "a=1&b=xyz");
  CHECK(!FundSource::parseToken("<html></html>", f.tokenRegex, token, error));
  CHECK(FundSource::expand(f.historyUrl, "111", token) == "https://beta.example/chart?id=111&type=line&a=1&b=xyz");
}

TEST(an_op_list_leaves_out_the_less_often_priced_and_the_ones_without_an_isin) {
  FundSource s = source("beta");
  std::vector<FundSource::Entry> entries;
  std::string error;
  CHECK(s.parseList(fixture("funds/beta-list.json"), entries, error));
  CHECK(entries.size() == 4);
  CHECK(entries[0].fund.isin == "FI0008800511" && entries[0].id == "111" && entries[0].wanted);
  CHECK_NEAR(*entries[0].fund.managementFee, 1.5, 1e-9);  // a string of a number, not scaled
  CHECK(*entries[0].fund.risk == 5 && *entries[0].fund.rating == 4 && *entries[0].fund.esg == 3);
  CHECK(entries[0].fund.url == "https://beta.example/funds/beta-world/");
  CHECK(!entries[1].wanted);  // the monthly one
  CHECK(entries[2].id == "333" && entries[2].fund.url.empty());  // a number
  CHECK(entries[3].fund.isin.empty());  // none
  CHECK(!s.parseList("[]", entries, error));
}

TEST(a_source_with_a_token_asks_for_its_history_with_it) {
  FakeHttp http, history;
  http.answer = [](const std::string & url) {
    if (url.find("beta.example/funds") != std::string::npos) return ok(fixture("funds/beta-page.html"));
    if (url.find("beta.example/list?a=1&b=xyz&") != std::string::npos) return ok(fixture("funds/beta-list.json"));
    return failed(404);
  };
  history.answer = [](const std::string & url) {
    if (url.find("id=111&") != std::string::npos) return ok(fixture("funds/beta-chart-daily.json"));
    if (url.find("id=333&") != std::string::npos) return ok(fixture("funds/beta-chart-wrapped.txt"));
    return failed(404);
  };
  std::shared_ptr<Store> store = makeMemoryStore();
  Listener listener;
  FundSource funds = source("beta");
  funds.load(*store, http, listener);
  auto s = store->snapshot();
  auto edge = edgeOf(*s, "FI0008800511", "Beta Funds", "beta");
  CHECK(edge.has_value());
  auto other = edgeOf(*s, "FI0008801238", "Beta Funds", "beta");  // priced as a number, not a string
  CHECK(other.has_value());
  CHECK(!edgeOf(*s, "FI4000439971", "Beta Funds", "beta"));  // known, not wanted
  CHECK(said(listener, "no ISIN for 444 (No ISIN)"));
  if (!edge || !other) return;
  CHECK(s->candles(*edge, Resolution::Day).candles.size() == 1);
  CHECK(s->candles(*edge, Resolution::Day).candles[0].close == 20.25);
  CHECK(!s->previousClose(*edge, dayNumber(2026, 11, 19)));  // the list has no change
  auto base = *s->findInstrumentByIsin("FI0008800511");
  auto url = s->attribute(base.id, predicate::webUrl);
  CHECK(url && asText(*url) && *asText(*url) == "https://beta.example/funds/beta-world/");
  CHECK(!http.headers.empty() && http.headers[0].size() == 2);  // asked for with a Referer and an Origin

  HistoryLoader loader(store, borrow(history), quickly(), [] {});
  loader.addProvider(funds.historyProvider());
  loader.want({*edge, *other});  // in this order
  loader.waitIdle();
  CHECK(history.urls.size() == 2 && history.urls[0] == "https://beta.example/chart?id=111&type=line&a=1&b=xyz");
  CHECK(store->snapshot()->candles(*edge, Resolution::Day).candles.size() == 4);  // the history's three and the list's
  CHECK(store->snapshot()->candles(*other, Resolution::Day).candles.size() == 3);  // the wrapped series
}
TEST(a_fund_with_a_wrong_isin_is_not_stored_and_is_reported) {
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("beta.example/funds") != std::string::npos) return ok(fixture("funds/beta-page.html"));
    if (url.find("beta.example/list") != std::string::npos)
      return ok(R"({"inst":[{"isin":"FI0008800512","name":"Typo","id_notation":"1","price":"1","datetime_price":"1795046400"}]})");
    return failed(404);
  };
  auto store = makeMemoryStore();
  Listener listener;
  FundSource funds = source("beta");
  funds.load(*store, http, listener);
  CHECK(store->snapshot()->instruments().empty());
  CHECK(said(listener, "no ISIN for 1 (Typo)"));
  CHECK(!writeFund(*store, "beta", "Beta Funds", FundRow{.isin = "", .name = "x"}, {{1, 0, 0, 0, 1, 0}}));
  CHECK(store->snapshot()->instruments().empty());
}
TEST(the_dates_a_list_gives_are_day_numbers) {
  using nlohmann::json;
  CHECK(looseDay(json("2026-11-19T00:00:00")) == dayNumber(2026, 11, 19));
  CHECK(looseDay(json("2026-11-19")) == dayNumber(2026, 11, 19));
  CHECK(looseDay(json(1795046400)) == dayNumber(2026, 11, 19));            // seconds
  CHECK(looseDay(json("1795046400")) == dayNumber(2026, 11, 19));          // a string of them
  CHECK(looseDay(json(1795046400000.0)) == dayNumber(2026, 11, 19));       // milliseconds
  CHECK(looseDay(json(1795046400 - 3 * 3600)) == dayNumber(2026, 11, 19));  // midnight in Helsinki, in UTC
  CHECK(looseDay(json(1795046400 - 2 * 3600)) == dayNumber(2026, 11, 19));  // and in winter
  CHECK(looseDay(json(1795046400 + 12 * 3600)) == dayNumber(2026, 11, 19));  // noon UTC
  CHECK(!looseDay(json("soon")) && !looseDay(json(0)) && !looseDay(json("2026-13-40")));
}
TEST(schemas_are_read_from_json_and_a_wrong_one_says_what_is_wrong) {
  // Every schema that comes with the program: it reads, by its name and by its path, and the ISINs in its table are valid.
  int shipped = 0;
  for (const auto & file : std::filesystem::directory_iterator(std::string(STOCKS_DATA_DIR) + "/funds")) {
    if (file.path().extension() != ".json") continue;
    shipped++;
    FundSchema byPath, byName;
    std::string error;
    CHECK(FundSchema::load(file.path().string(), byPath, error));
    CHECK(FundSchema::load(file.path().stem().string(), byName, error));
    CHECK(byPath.name == file.path().stem().string() && byName.name == byPath.name);
    CHECK(!byPath.list.url.empty() && !byPath.historyUrl.empty());
    for (const auto & [id, isin] : byPath.isins) CHECK(isin.empty() || isValidIsin(isin));
  }
  CHECK(shipped >= 2);
  FundSchema f;
  std::string error;
  CHECK(!FundSchema::load("nosuchschema", f, error) && error.find("nosuchschema") != std::string::npos);
  CHECK(!FundSchema::load("/nonexistent/x.json", f, error));
  CHECK(!FundSchema::parse("[]", f, error));
  CHECK(!FundSchema::parse(R"({"name":"x","market":"X","dataset":"x","list":{"url":"u","fields":{"id":"id"}},"history":{"url":"h"}})", f, error));  // a pointer has a slash
  CHECK(error.find("field id") != std::string::npos);
  CHECK(!FundSchema::parse(R"({"name":"x","market":"X","dataset":"x","list":{"url":"u","fields":{"name":"/n"}},"history":{"url":"h"}})", f, error));  // no id
  CHECK(FundSchema::parse(R"({"name":"x","market":"X","dataset":"x","list":{"url":"u","fields":{"id":"/i"}},"history":{"url":"h"}})", f, error));
  CHECK(f.pause == std::chrono::milliseconds(2000) && f.refresh == std::chrono::hours(6));
}

TEST(a_failed_token_page_says_so_and_writes_nothing) {
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(403); };
  auto store = makeMemoryStore();
  Listener listener;
  FundSource funds = source("beta");
  funds.load(*store, http, listener);
  CHECK(listener.changes == 0 && listener.messages.size() == 1);
  CHECK(store->snapshot()->instruments().empty());
}

TEST(sparse_series_are_judged_by_their_typical_gap) {
  std::vector<Candle> daily, monthly, mixed;
  for (int i = 0; i < 40; i++) daily.push_back({int64_t(1000 + i + (i / 5) * 2), 0, 0, 0, 1, 0});  // weekends
  for (int i = 0; i < 12; i++) monthly.push_back({int64_t(1000 + i * 30), 0, 0, 0, 1, 0});
  for (int i = 0; i < 12; i++) mixed.push_back({int64_t(1000 + i * 90), 0, 0, 0, 1, 0});  // quarterly long ago...
  for (int i = 0; i < 60; i++) mixed.push_back({int64_t(2000 + i), 0, 0, 0, 1, 0});       // ...daily now
  CHECK(!tooSparse(daily));
  CHECK(tooSparse(monthly));
  CHECK(!tooSparse(mixed));
  CHECK(!tooSparse({}));
}

// A real answer of a company's list (tests/fixtures/funds/discovery/NAME.json, for the schema NAME in data/funds), with what it is expected to give
// (NAME.expected.json): the funds shown at once, from the list alone, the ones left out for an old NAV and the ones with no ISIN.
TEST(a_real_discovery_answer_shows_the_funds_with_an_isin_and_a_new_nav_and_says_which_it_leaves_out) {
  using nlohmann::json;
  int files = 0;
  for (const auto & file : std::filesystem::directory_iterator(std::string(STOCKS_FIXTURES_DIR) + "/funds/discovery")) {
    const std::string stem = file.path().stem().string();
    if (file.path().extension() != ".json" || stem.find(".expected") != std::string::npos) continue;
    files++;
    const std::string list = fixture("funds/discovery/" + stem + ".json");
    const json expected = json::parse(fixture("funds/discovery/" + stem + ".expected.json"));
    FundSchema schema;
    std::string error;
    CHECK(FundSchema::load(stem, schema, error));
    schema.pause = std::chrono::milliseconds(0);
    // A list that needs a token has the page that hands it out beside it (NAME.page.html).
    std::string token, pageUrl;
    size_t requests = 1;
    if (!schema.tokenPage.empty()) {
      pageUrl = schema.tokenPage;
      CHECK(FundSource::parseToken(fixture("funds/discovery/" + stem + ".page.html"), schema.tokenRegex, token, error));
      requests++;
    }
    const std::string listUrl = FundSource::expand(schema.list.url, "", token);
    FakeHttp http;
    http.answer = [&](const std::string & url) {
      if (url == listUrl) return ok(list);
      if (!pageUrl.empty() && url == pageUrl) return ok(fixture("funds/discovery/" + stem + ".page.html"));
      return failed(404);
    };
    auto store = makeMemoryStore();
    Listener listener;
    FundSource funds(schema);
    funds.setToday([] { return dayNumber(2026, 11, 20); });
    funds.load(*store, http, listener);
    auto snapshot = store->snapshot();

    CHECK(json::parse(list)[json::json_pointer(schema.list.rows)].size() == expected["rows"].get<size_t>());
    // Every edge is a fund with a valid ISIN, one bar with a close and a change to say the NAV before it; nothing is asked for but the list and the details of
    // the funds with no ISIN.
    size_t edges = 0;
    for (const Market & market : snapshot->markets())
      for (const EdgeKey & edge : snapshot->edges(market.id)) {
        edges++;
        auto base = snapshot->instrument(edge.base);
        CHECK(base && base->kind == InstrumentKind::Fund && isValidIsin(base->isin));
        auto bars = snapshot->candles(edge, Resolution::Day).candles;
        CHECK(bars.size() == 1 && bars[0].close > 0 && bars[0].time <= dayNumber(2026, 11, 20));
      }
    CHECK(edges == expected["shown"].get<size_t>());
    for (const std::string & url : http.urls) CHECK(url == listUrl || url == pageUrl || url.find("details") != std::string::npos);
    CHECK(said(listener, std::to_string(expected["stale"].get<int>()) + " left out for an old NAV"));
    for (const std::string & id : expected["noIsin"]) CHECK(said(listener, "no ISIN for " + id + " ("));
    CHECK(http.urls.size() == requests + expected["noIsin"].size());  // the page and the list, and the details of each of those
    // The change the list gives is the NAV before the latest, which the history of the day before says too.
    for (const auto & [id, close] : expected["previousClose"].items()) {
      std::optional<double> previous;
      for (const Market & market : snapshot->markets())
        for (const EdgeKey & edge : snapshot->edges(market.id))
          if (snapshot->listing(edge)->identifier == id) previous = snapshot->previousClose(edge, snapshot->candles(edge, Resolution::Day).candles.back().time);
      CHECK(previous && std::fabs(*previous - close.get<double>()) < 0.005);
    }
    http.urls.clear();  // the next load asks for the list only: what has no ISIN is not asked for again
    funds.load(*store, http, listener);
    CHECK(http.urls.size() == requests);
    for (const auto & [id, isin] : expected["isins"].items()) {
      auto instrument = snapshot->findInstrumentByIsin(isin.get<std::string>());
      CHECK(instrument.has_value());
      if (!instrument) continue;
      bool found = false;
      for (const Market & market : snapshot->markets())
        for (const EdgeKey & edge : snapshot->edges(market.id))
          if (edge.base == instrument->id) found = snapshot->listing(edge)->identifier == id;
      CHECK(found);
    }
  }
  CHECK(files >= 1);
}

// The histories as the companies give them, in part: a day stamped at midnight in Helsinki, with the points out of order, and a day stamped at noon UTC.
TEST(real_history_answers_are_days_in_order_whichever_way_the_company_stamps_them) {
  std::vector<Candle> bars;
  std::string error;
  CHECK(parseNavSeries(fixture("funds/real-history-a.json"), bars, error));
  CHECK(bars.size() == 17);
  CHECK(bars.front().time == dayNumber(2006, 5, 15) && bars.back().time == dayNumber(2026, 11, 19));
  for (size_t i = 1; i < bars.size(); i++) CHECK(bars[i - 1].time < bars[i].time);  // sorted, though the answer was not
  CHECK(bars.back().close == 3.3099 && !isDefined(bars.back().open));  // the list's NAV of that day
  CHECK(parseNavSeries(fixture("funds/real-history-b.json"), bars, error));
  CHECK(bars.size() == 6);
  CHECK(bars.front().time == dayNumber(2011, 9, 29) && bars.front().close == 100);
  CHECK(bars.back().time == dayNumber(2026, 11, 19) && bars.back().close == 347.11);
  for (size_t i = 1; i < bars.size(); i++) CHECK(bars[i].time - bars[i - 1].time < 10000);
}
