#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "AttributeView.h"
#include "model/Isin.h"
#include "source/FundSource.h"

#include <filesystem>
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

FundSource source(const std::string & name) { return FundSource(schema(name)); }

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
  CHECK(entries.size() == 5);
  CHECK(entries[0].fund.isin == "FI0008810569" && entries[0].id == "ALPHANORDIC" && entries[0].fund.symbol == "ALPHANORDIC");
  CHECK_NEAR(*entries[0].fund.managementFee, 1.53, 1e-9);
  CHECK(*entries[0].fund.risk == 6 && *entries[0].fund.rating == 4);
  CHECK(!entries[2].wanted);
  CHECK(!s.parseList("{}", entries, error));
}

TEST(the_online_fee_is_preferred_to_the_ordinary_one) {
  FundSource s = source("alpha");
  FundSource::Entry e;
  std::string error;
  CHECK(s.parseDetail(fixture("funds/alpha-details.json"), e, error));
  CHECK(e.fund.subscriptionFee && *e.fund.subscriptionFee == 0.0);  // the online fee, not 1.0
  CHECK(e.fund.redemptionFee && *e.fund.redemptionFee == 0.5);
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

TEST(the_fund_source_writes_funds_as_nav_bars_and_drops_sparse_series) {
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("alpha.example/list") != std::string::npos) return ok(fixture("funds/alpha-list.json"));
    if (url.find("alpha.example/details/ALPHANORDIC") != std::string::npos) return ok(fixture("funds/alpha-details.json"));
    if (url.find("alpha.example/details/") != std::string::npos) return failed(404);
    if (url.find("alpha.example/rates/ALPHANORDIC") != std::string::npos) return ok(fixture("funds/alpha-rates-daily.json"));
    if (url.find("alpha.example/rates/LU0823428346EUR") != std::string::npos) return ok(fixture("funds/alpha-rates-quarterly.json"));
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
  CHECK(bars.size() == 4);  // the later of two on one day stands
  CHECK(bars[2].close == 10.25 && !isDefined(bars[2].open) && !isDefined(bars[2].volume));
  CHECK(bars[0].time == dayNumber(2025, 12, 30));
  CHECK(s->listing(*edge)->identifier == "ALPHANORDIC");
  CHECK(s->findInstrumentByIsin("FI0008810569")->kind == InstrumentKind::Fund);
  CHECK(!edgeOf(*s, "LU0823428346", "Alpha Funds", "alpha"));  // priced quarterly: dropped
  auto nordic = *s->findInstrumentByIsin("FI0008810569");
  auto fee = s->attribute(nordic.id, "redemption-fee");
  CHECK(fee && asReal(*fee) == 0.5);
  CHECK(listener.changes == 1);
  // The info panel shows what is said of the fund: its fees, risk class and rating beside its ISIN.
  std::string shown;
  for (const InfoSection & section : buildInfo(*s, *edge))
    for (const InfoRow & row : section.rows) shown += section.title + "/" + row.predicate + "=" + row.value.text + "\n";
  CHECK(shown.find("Instrument/redemption-fee=0.5") != std::string::npos);
  CHECK(shown.find("Instrument/management-fee=1.53") != std::string::npos);
  CHECK(shown.find("Instrument/risk-class=6") != std::string::npos);
  CHECK(shown.find("Instrument/morningstar-rating=4") != std::string::npos);
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

TEST(the_op_source_writes_funds_under_its_own_dataset_and_market) {
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("beta.example/funds") != std::string::npos) return ok(fixture("funds/beta-page.html"));
    if (url.find("beta.example/list?a=1&b=xyz&") != std::string::npos) return ok(fixture("funds/beta-list.json"));
    if (url.find("id=111&") != std::string::npos) return ok(fixture("funds/beta-chart-daily.json"));
    if (url.find("id=333&") != std::string::npos) return ok(fixture("funds/beta-chart-wrapped.txt"));
    return failed(404);
  };
  auto store = makeMemoryStore();
  Listener listener;
  FundSource funds = source("beta");
  funds.load(*store, http, listener);
  auto s = store->snapshot();
  auto edge = edgeOf(*s, "FI0008800511", "Beta Funds", "beta");
  CHECK(edge.has_value());
  if (edge) CHECK(s->candles(*edge, Resolution::Day).candles.size() == 3);
  CHECK(edgeOf(*s, "FI0008801238", "Beta Funds", "beta").has_value());  // the wrapped series
  if (edge) {
    auto url = s->attribute(NodeRef(edge->base), predicate::webUrl);
    CHECK(url && asText(*url) && asText(*url)->find("https://beta.example/") == 0);
  }
  CHECK(!edgeOf(*s, "FI4000439971", "Beta Funds", "beta"));
  CHECK(!http.headers.empty() && http.headers[0].size() == 2);  // the page is asked for with a Referer and an Origin
}

TEST(a_fund_with_a_wrong_isin_is_not_stored_and_is_reported) {
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("beta.example/funds") != std::string::npos) return ok(fixture("funds/beta-page.html"));
    if (url.find("beta.example/list") != std::string::npos) return ok(R"({"inst":[{"isin":"FI0008800512","name":"Typo","id_notation":"1"}]})");
    return ok(fixture("funds/beta-chart-daily.json"));
  };
  auto store = makeMemoryStore();
  Listener listener;
  FundSource funds = source("beta");
  funds.load(*store, http, listener);
  CHECK(store->snapshot()->nodeCount(NodeType::Instrument) == 0);
  bool said = false;
  for (const std::string & m : listener.messages) said = said || m.find("no ISIN for 1 (Typo)") != std::string::npos;
  CHECK(said);
  CHECK(!writeFund(*store, "beta", "Beta Funds", FundRow{.isin = "", .name = "x"}, {{1, 0, 0, 0, 1, 0}}));
  CHECK(store->snapshot()->nodeCount(NodeType::Instrument) == 0);
}

TEST(a_fund_whose_isin_is_only_in_its_details_is_stored_and_the_find_is_said) {
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("alpha.example/list") != std::string::npos) return ok(R"([{"productId":"ALPHANEWFUND","productNameEn":"Alpha New"}])");
    if (url.find("alpha.example/details/") != std::string::npos) return ok(R"({"values":{},"identifiers":{"isin":"FI4000297817"}})");
    return ok(fixture("funds/alpha-rates-daily.json"));
  };
  auto store = makeMemoryStore();
  Listener listener;
  FundSource funds = source("alpha");
  funds.load(*store, http, listener);
  CHECK(edgeOf(*store->snapshot(), "FI4000297817", "Alpha Funds", "alpha").has_value());
  bool said = false;
  for (const std::string & m : listener.messages) said = said || m.find("found in the details at /identifiers/isin") != std::string::npos;
  CHECK(said);
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
  CHECK(store->snapshot()->nodeCount(NodeType::Instrument) == 0);
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
