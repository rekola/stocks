#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "source/Countries.h"
#include "source/NordnetDetails.h"
#include "source/SourceRunner.h"

#include <fstream>
#include <sstream>

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/nordnet/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

const Attribute * find(const std::vector<Attribute> & said, const std::string & predicate) {
  for (const Attribute & a : said)
    if (a.predicate == predicate) return &a;
  return nullptr;
}

// A stock and a fund on one market, each with the order book id its details are asked for by.
void addInstruments(Store & store) {
  Instrument eur = store.addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument stock = store.addInstrument({{}, "", "AbbVie", InstrumentKind::Company, "US00287Y1091"});
  Instrument fund = store.addInstrument({{}, "", "A fund", InstrumentKind::Fund, "LU0231483743"});
  Market market = store.addMarket({{}, "Somewhere"});
  Dataset data = store.addDataset({{}, "nordnet"});
  for (auto [instrument, book] : {std::pair{stock, "stock-book"}, std::pair{fund, "fund-book"}}) {
    EdgeKey key{instrument.id, eur.id, market.id, data.id};
    store.appendCandles(key, Resolution::Day, {{dayNumber(2026, 1, 2), 1, 1, 1, 4.2, 1}});
    store.setListing({key, instrument.name, "id", book, "X", instrument.name});
  }
}

}  // namespace

TEST(a_stock_page_gives_its_description_shares_country_and_classification) {
  std::vector<Attribute> said;
  std::string error;
  CHECK(NordnetDetails::parseStock(fixture("stock-details.json"), said, error));
  CHECK(find(said, "description") && asText(find(said, "description")->value)->rfind("AbbVie", 0) == 0);
  CHECK(*asInteger(find(said, "shares-outstanding")->value) == 1767616306);
  CHECK(*asInteger(find(said, "shares-diluted")->value) == 1772000000);
  CHECK(*asText(find(said, "country")->value) == "US");
  CHECK(*asText(find(said, "sector")->value) == "HEALTHCARE");
  CHECK(*asText(find(said, "company-website")->value) == "http://www.abbvie.com");
  CHECK_NEAR(*asReal(find(said, "revenue")->value), 61160000000.0, 1);
  CHECK(!find(said, "pe"));  // the list says that
}

TEST(a_fund_page_gives_the_cut_off_the_ratings_the_fees_and_the_risk_statistics) {
  std::vector<Attribute> said;
  std::string error;
  CHECK(NordnetDetails::parseFund(fixture("fund-details.json"), said, error));
  CHECK(*asText(find(said, "fund-cut-off")->value) == "10:00:00");
  CHECK(*asInteger(find(said, "morningstar-rating")->value) == 2);
  CHECK(*asInteger(find(said, "risk-class")->value) == 5);
  CHECK_NEAR(*asReal(find(said, "total-fee")->value), 1.93, 1e-9);
  CHECK_NEAR(*asReal(find(said, "beta-3y")->value), 1.0, 1e-9);
  CHECK_NEAR(*asReal(find(said, "sharpe-1y")->value), -1.11, 1e-9);
  CHECK(*asInteger(find(said, "start-day")->value) == dayNumber(1992, 4, 27));
  CHECK(*asBoolean(find(said, "ucits")->value));
  CHECK(*asText(find(said, "admin-company")->value) == "abrdn Investments Luxembourg S.A.");
}

TEST(a_page_that_is_not_of_the_kind_is_refused) {
  std::vector<Attribute> said;
  std::string error;
  CHECK(!NordnetDetails::parseStock("{}", said, error));
  CHECK(!NordnetDetails::parseFund("not json", said, error));
  CHECK(!error.empty() && said.empty());
}

TEST(the_details_are_asked_by_order_book_once_and_written_to_the_instruments) {
  auto store = makeMemoryStore();
  addInstruments(*store);
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url == NordnetDetails::stockUrl("stock-book")) return ok(fixture("stock-details.json"));
    if (url == NordnetDetails::fundUrl("fund-book")) return ok(fixture("fund-details.json"));
    return failed(404);
  };
  Listener listener;
  NordnetDetails source;
  CHECK(!source.loadBatch(*store, http, listener, std::chrono::milliseconds(0), 300));
  CHECK(http.urls.size() == 2);
  auto snapshot = store->snapshot();
  auto stock = snapshot->findInstrumentByIsin("US00287Y1091");
  auto fund = snapshot->findInstrumentByIsin("LU0231483743");
  CHECK(stock && fund);
  CHECK(snapshot->attribute(stock->id, "shares-outstanding").has_value());
  CHECK(snapshot->attribute(fund->id, "fund-cut-off").has_value());
  CHECK(!snapshot->attribute(stock->id, "fund-cut-off"));
  // Nothing is asked twice, by this source or after what it said was kept.
  source.loadBatch(*store, http, listener, std::chrono::milliseconds(0), 300);
  CHECK(http.urls.size() == 2);
  NordnetDetails again;
  again.loadBatch(*store, http, listener, std::chrono::milliseconds(0), 300);
  CHECK(http.urls.size() == 2);
}

TEST(a_load_fills_in_a_batch_and_a_missing_page_is_not_asked_again) {
  auto store = makeMemoryStore();
  addInstruments(*store);
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(404); };
  Listener listener;
  NordnetDetails source;
  const auto none = std::chrono::milliseconds(0);
  CHECK(source.loadBatch(*store, http, listener, none, 1));  // a batch of one, and one more remains
  CHECK(http.urls.size() == 1);
  CHECK(!source.loadBatch(*store, http, listener, none, 1));  // the other; the first one was not found, so it is not asked again
  CHECK(http.urls.size() == 2);
  CHECK(!source.loadBatch(*store, http, listener, none, 1));
  CHECK(http.urls.size() == 2);
}

TEST(a_connection_that_fails_is_tried_again_at_the_next_load) {
  auto store = makeMemoryStore();
  addInstruments(*store);
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(503); };
  Listener listener;
  NordnetDetails source;
  source.loadBatch(*store, http, listener, std::chrono::milliseconds(0), 300);
  source.loadBatch(*store, http, listener, std::chrono::milliseconds(0), 300);
  CHECK(http.urls.size() == 4);
}

TEST(a_country_is_known_by_its_finnish_name_in_any_case_and_never_guessed) {
  CHECK(*countryCodeOfFinnishName("Saksa") == "DE");
  CHECK(*countryCodeOfFinnishName("ITÄVALTA") == "AT");
  CHECK(*countryCodeOfFinnishName("Yhdistynyt kuningaskunta") == "GB");
  CHECK(*countryCodeOfFinnishName("Hongkong") == "HK");
  CHECK(*countryCodeOfFinnishName("FÄRSAARET") == "FO");
  CHECK(*countryCodeOfFinnishName("Tšekki") == "CZ");
  CHECK(!countryCodeOfFinnishName("Atlantis"));
  CHECK(!countryCodeOfFinnishName(""));
}

namespace {

// A source with details: three batches, as the runner asks for them.
class DetailsSource : public Source {
 public:
  // Counted where the test can read them after the runner has ended and taken the source with it.
  struct Counts {
    std::atomic<int> batches{0}, loads{0};
  };
  explicit DetailsSource(std::shared_ptr<Counts> counts) : c(std::move(counts)) {}
  std::shared_ptr<Counts> c;
  std::string name() const override { return "details"; }
  std::string description() const override { return "test"; }
  void load(Store &, Http &, SourceListener &) override { c->loads++; }
  bool loadDetails(Store &, Http &, SourceListener &) override { return ++c->batches < 3; }
  std::chrono::milliseconds detailsInterval() const override { return std::chrono::milliseconds(0); }
};

}  // namespace

TEST(the_runner_asks_a_source_for_its_details_in_batches_until_it_has_none_left) {
  auto counts = std::make_shared<DetailsSource::Counts>();
  std::vector<std::unique_ptr<Source>> sources;
  sources.push_back(std::make_unique<DetailsSource>(counts));
  {
    SourceRunner runner(makeMemoryStore(), std::move(sources), [] {}, [](std::string) {});
    runner.wait();  // nothing is refreshed, so it ends when the last batch has been asked for
  }
  CHECK(counts->loads == 1);
  CHECK(counts->batches == 3);
}
