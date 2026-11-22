#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "model/Isin.h"
#include "source/AktiaSource.h"
#include "source/OpSource.h"

#include <fstream>
#include <sstream>

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

constexpr auto kNoPause = std::chrono::milliseconds(0);

std::optional<EdgeKey> edgeOf(const Snapshot & s, const char * isin, const char * market, const char * dataset) {
  auto b = s.findInstrumentByIsin(isin);
  auto m = s.findMarket(market);
  auto d = s.findDataset(dataset);
  auto q = s.findInstrument("EUR");
  if (!b || !m || !d || !q) return std::nullopt;
  return EdgeKey{b->id, q->id, m->id, d->id};
}

}  // namespace

TEST(aktia_isins_are_valid_and_luxembourg_ones_come_from_the_id) {
  bool known = false;
  auto isin = AktiaSource::isinOf("LU0823428346EUR", known);
  CHECK(known && isin && *isin == "LU0823428346");
  isin = AktiaSource::isinOf("AKTNORDICB", known);
  CHECK(known && isin && *isin == "FI0008810569");
  isin = AktiaSource::isinOf("AKTASUNNOTAO", known);
  CHECK(known && !isin);  // listed, and not wanted
  isin = AktiaSource::isinOf("NOSUCH", known);
  CHECK(!known && !isin);
  for (const char * id : {"ROBURSVERIGEA", "AKTNORDICHYJB", "AKTEUKASSAKONEJB", "AKTARVOREINJB", "AKTIALIKVI", "AKTEURHYB", "AKTEMFRONTB",
                          "AKTEMLCB", "AKTEMERB", "AKTCORBONB", "ROBURROVAR", "ROBURCONTU", "ROBURJAPAN", "ROBURPACIF", "ODINSVERIGE",
                          "ODINNORGE", "HQINDIEN", "PARINDIA", "PAREQWRLDEME", "PARCHINA", "PAREQASIA", "AKTNORSCAB", "AKTNORDMCB",
                          "AKTNORDICB", "AKTEURSCAB", "AKTIAEUROB", "AKTVARH50B", "AKTVARH25B", "AKTIOSAKEB", "AKTABAB", "AKTEURSMAB",
                          "AKTPROSPB"}) {
    isin = AktiaSource::isinOf(id, known);
    CHECK(known && isin && isValidIsin(*isin));
  }
}

TEST(an_aktia_list_gives_the_wanted_funds_and_reports_the_unknown) {
  std::vector<AktiaSource::Product> products;
  std::vector<std::string> unknown;
  std::string error;
  CHECK(AktiaSource::parseList(fixture("aktia/product-list.json"), products, unknown, error));
  CHECK(products.size() == 2);
  CHECK(products[0].fund.isin == "FI0008810569" && products[0].productId == "AKTNORDICB");
  CHECK_NEAR(*products[0].fund.managementFee, 1.53, 1e-9);
  CHECK(*products[0].fund.risk == 6 && *products[0].fund.rating == 4);
  CHECK(products[1].fund.isin == "LU0823428346");
  CHECK(unknown.size() == 2);  // the quarterly one and the new one have no ISIN in the table
  CHECK(!AktiaSource::parseList("{}", products, unknown, error));
}

TEST(aktia_fees_prefer_the_online_fee) {
  FundRow f;
  std::string error;
  CHECK(AktiaSource::parseFees(fixture("aktia/fees-nordic.json"), f, error));
  CHECK(f.subscriptionFee && *f.subscriptionFee == 0.0);  // the online fee, not 1.0
  CHECK(f.redemptionFee && *f.redemptionFee == 0.5);
  CHECK(!AktiaSource::parseFees("[]", f, error));
}

TEST(the_aktia_source_writes_funds_as_nav_bars_and_drops_sparse_series) {
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("product-list") != std::string::npos) return ok(fixture("aktia/product-list.json"));
    if (url.find("product-base-details/AKTNORDICB") != std::string::npos) return ok(fixture("aktia/fees-nordic.json"));
    if (url.find("product-base-details/") != std::string::npos) return failed(404);
    if (url.find("base/rates/AKTNORDICB") != std::string::npos) return ok(fixture("aktia/rates-daily.json"));
    if (url.find("base/rates/LU0823428346EUR") != std::string::npos) return ok(fixture("aktia/rates-quarterly.json"));
    return failed(404);
  };
  auto store = makeMemoryStore();
  Listener listener;
  AktiaSource source(kNoPause);
  source.load(*store, http, listener);
  auto s = store->snapshot();
  auto edge = edgeOf(*s, "FI0008810569", "Aktia Funds", "aktia");
  CHECK(edge.has_value());
  if (!edge) return;
  auto bars = s->candles(*edge, Resolution::Day).candles;
  CHECK(bars.size() == 4);  // the later of two on one day stands
  CHECK(bars[2].close == 10.25 && !isDefined(bars[2].open) && !isDefined(bars[2].volume));
  CHECK(bars[0].time == dayNumber(2025, 12, 30));
  CHECK(s->listing(*edge)->identifier == "AKTNORDICB");
  CHECK(s->findInstrumentByIsin("FI0008810569")->kind == InstrumentKind::Fund);
  CHECK(!edgeOf(*s, "LU0823428346", "Aktia Funds", "aktia"));  // priced quarterly: dropped
  auto nordic = *s->findInstrumentByIsin("FI0008810569");
  auto fee = s->attribute(nordic.id, "redemption-fee");
  CHECK(fee && asReal(*fee) == 0.5);
  CHECK(listener.changes == 1);
}

TEST(an_op_page_gives_its_token_without_a_leading_ampersand) {
  std::string token, error;
  CHECK(OpSource::parseToken(fixture("op/funds-page.html"), token, error));
  CHECK(token == "a=1&b=xyz");
  CHECK(!OpSource::parseToken("<html></html>", token, error));
  CHECK(OpSource::chartUrl("111", token) == "https://marketdata.op.fi/portal2/charts//fundchartdata.php?ID_NOTATION=111&type=line&a=1&b=xyz");
}

TEST(an_op_list_leaves_out_the_less_often_priced_and_the_ones_without_an_isin) {
  std::vector<OpSource::Product> products;
  std::string error;
  CHECK(OpSource::parseList(fixture("op/fundlist.json"), products, error));
  CHECK(products.size() == 2);  // the monthly one and the one with no ISIN are out
  CHECK(products[0].fund.isin == "FI0008800511" && products[0].notation == "111");
  CHECK_NEAR(*products[0].fund.managementFee, 1.5, 1e-9);  // a string of a number
  CHECK(*products[0].fund.risk == 5 && *products[0].fund.rating == 4 && *products[0].fund.esg == 3);
  CHECK(products[1].notation == "333");  // a number
  CHECK(!OpSource::parseList("[]", products, error));
}

TEST(the_op_source_writes_funds_under_its_own_dataset_and_market) {
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("kaikki-rahastot") != std::string::npos) return ok(fixture("op/funds-page.html"));
    if (url.find("fundlist-json.php?a=1&b=xyz&") != std::string::npos) return ok(fixture("op/fundlist.json"));
    if (url.find("ID_NOTATION=111&") != std::string::npos) return ok(fixture("op/chart-daily.json"));
    if (url.find("ID_NOTATION=333&") != std::string::npos) return ok(fixture("op/chart-wrapped.txt"));
    return failed(404);
  };
  auto store = makeMemoryStore();
  Listener listener;
  OpSource source(kNoPause);
  source.load(*store, http, listener);
  auto s = store->snapshot();
  auto edge = edgeOf(*s, "FI0008800511", "OP Funds", "op");
  CHECK(edge.has_value());
  if (edge) CHECK(s->candles(*edge, Resolution::Day).candles.size() == 3);
  CHECK(edgeOf(*s, "FI0008801234", "OP Funds", "op").has_value());  // the wrapped series
  CHECK(!edgeOf(*s, "FI4000439971", "OP Funds", "op"));
  CHECK(!http.headers.empty() && http.headers[0].size() == 2);  // the page is asked for with a Referer and an Origin
}

TEST(a_failed_op_page_says_so_and_writes_nothing) {
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(403); };
  auto store = makeMemoryStore();
  Listener listener;
  OpSource source(kNoPause);
  source.load(*store, http, listener);
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
