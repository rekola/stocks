#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "model/Isin.h"
#include "portfolio/Portfolio.h"
#include "source/GoldApiSource.h"
#include "source/PhysicalSource.h"

#include <fstream>
#include <map>
#include <sstream>

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/goldapi/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

struct Said : SourceListener {
  std::vector<std::string> messages;
  int changes = 0;
  void changed() override { changes++; }
  void message(const std::string & m) override { messages.push_back(m); }
  bool pause(std::chrono::milliseconds) override { return true; }  // no waiting in a test
};

std::string priceBody(const std::string & symbol, const std::string & name, const std::string & currency, double price, const std::string & updated = "2026-11-24T14:19:44Z") {
  return "{\"currency\":\"" + currency + "\",\"exchangeRate\":1.0,\"name\":\"" + name + "\",\"price\":" + std::to_string(price) + ",\"symbol\":\"" + symbol +
         "\",\"updatedAt\":\"" + updated + "\"}";
}

// Prices for every symbol of the real list, in euros 0.9 of dollars.
HttpResult answerPrices(const std::string & url) {
  if (url == "https://api.gold-api.com/symbols") return ok(fixture("symbols.json"));
  const std::string tail = url.substr(std::string("https://api.gold-api.com/price/").size());
  const std::string symbol = tail.substr(0, tail.find('/')), currency = tail.substr(tail.find('/') + 1);
  const std::map<std::string, std::pair<std::string, double>> table = {{"XAU", {"Gold", 3600}}, {"XAG", {"Silver", 45}}, {"XPT", {"Platinum", 1300}},
                                                                      {"XPD", {"Palladium", 1100}}, {"HG", {"Copper", 5}}, {"BTC", {"Bitcoin", 60000}}, {"ETH", {"Ethereum", 3000}}};
  const auto it = table.find(symbol);
  if (it == table.end()) return failed(404);
  return ok(priceBody(symbol, it->second.first, currency, it->second.second * (currency == "EUR" ? 0.9 : 1.0)));
}

}  // namespace

TEST(the_symbols_answer_is_read_and_a_row_without_a_symbol_is_left_out) {
  std::vector<GoldApiSource::Symbol> symbols;
  std::string error;
  CHECK(GoldApiSource::parseSymbols(fixture("symbols.json"), symbols, error));
  CHECK(symbols.size() == 7 && symbols[1].symbol == "XAU" && symbols[1].name == "Gold" && symbols[6].symbol == "XPT" && symbols[6].name == "Platinum");
  CHECK(GoldApiSource::parseSymbols(R"([{"name":"Gold","symbol":"XAU"},{"name":"No symbol"},{"symbol":"X"},5])", symbols, error));
  CHECK(symbols.size() == 2 && symbols[1].symbol == "X" && symbols[1].name == "X");  // a name defaults to the symbol
  CHECK(!GoldApiSource::parseSymbols("", symbols, error) && !error.empty());
  CHECK(!GoldApiSource::parseSymbols("{}", symbols, error) && !GoldApiSource::parseSymbols("[]", symbols, error));
}

TEST(a_price_answer_is_read_with_the_date_of_its_update) {
  GoldApiSource::Price p;
  std::string error;
  CHECK(GoldApiSource::parsePrice(fixture("price-XAU-EUR.json"), p, error));
  CHECK(p.symbol == "XAU" && p.name == "Gold" && p.currency == "EUR" && std::abs(p.price - 3312.7278451306997) < 1e-9 && p.day == dayNumber(2026, 11, 24));
  CHECK(GoldApiSource::parsePrice(fixture("price-XAU-USD.json"), p, error) && p.currency == "USD" && std::abs(p.price - 3617.699951) < 1e-9);
  CHECK(GoldApiSource::parsePrice(fixture("price-BTC-USD.json"), p, error) && p.symbol == "BTC");
  for (const char * bad : {"", "[]", "{}", R"({"symbol":"XAU","currency":"EUR","price":"3000","updatedAt":"2026-11-24T14:19:44Z"})",
                           R"({"symbol":"XAU","currency":"EUR","price":0,"updatedAt":"2026-11-24T14:19:44Z"})",
                           R"({"symbol":"XAU","currency":"EUR","price":-3,"updatedAt":"2026-11-24T14:19:44Z"})",
                           R"({"symbol":"XAU","price":3000,"updatedAt":"2026-11-24T14:19:44Z"})",
                           R"({"symbol":"XAU","currency":"EUR","price":3000})",
                           R"({"symbol":"XAU","currency":"EUR","price":3000,"updatedAt":"yesterday"})"})
    CHECK(!GoldApiSource::parsePrice(bad, p, error) && !error.empty());
}

TEST(every_symbol_is_priced_in_euros_and_in_dollars_as_edges_of_the_dataset) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = answerPrices;
  Said said;
  GoldApiSource source(std::chrono::minutes(2), std::chrono::milliseconds(0));
  source.load(*store, http, said);
  CHECK(said.messages.empty() && said.changes == 7);  // one publication for each symbol
  // The list, then each symbol in each currency: 1 + 7 x 2 requests, gold first.
  CHECK(http.urls.size() == 15 && http.urls[0] == "https://api.gold-api.com/symbols");
  CHECK(http.urls[1] == "https://api.gold-api.com/price/XAU/EUR" && http.urls[2] == "https://api.gold-api.com/price/XAU/USD");
  const auto s = store->snapshot();
  const MarketId none = *s->findMarket(kNoMarket);
  const InstrumentId eur = *s->findInstrument("EUR"), usd = *s->findInstrument("USD");
  CHECK(s->kind(eur) == InstrumentKind::Currency && s->kind(usd) == InstrumentKind::Currency);
  const DatasetId data = *s->findDataset(kGoldApiDataset);
  CHECK(s->name(NodeRef(data)) == "goldapi" && s->edges(none).size() == 14);
  const InstrumentId gold = *s->findInstrument("XAU");
  // A live price is a quote and no bar: the bars are history's, and this API has none.
  const auto inEuros = s->quote({gold, eur, none, data}), inDollars = s->quote({gold, usd, none, data});
  CHECK(inEuros && inDollars);
  CHECK(std::abs(inEuros->last - 3240) < 1e-6 && std::abs(inDollars->last - 3600) < 1e-9);
  CHECK(dayNumberOfInstant(inEuros->time) == dayNumber(2026, 11, 24));
  CHECK(s->candles({gold, eur, none, data}, Resolution::Day).candles.empty() && s->candles({gold, usd, none, data}, Resolution::Day).candles.empty());
  CHECK(s->symbol(EdgeKey{gold, eur, none, data}) == "XAU" && s->name(NodeRef(gold)) == "Gold");
  // What a price is for is in the name the row shows: the metals by the troy ounce, copper by the pound, a coin by the coin.
  for (auto [symbol, shown] : {std::pair{"XAU", "Gold, per troy oz"}, {"XAG", "Silver, per troy oz"}, {"XPT", "Platinum, per troy oz"},
                               {"XPD", "Palladium, per troy oz"}, {"HG", "Copper, per tonne"}, {"BTC", "Bitcoin, per coin"}, {"ETH", "Ethereum, per coin"}})
    for (const InstrumentId currency : {eur, usd}) CHECK(s->listing({*s->findInstrument(symbol), currency, none, data})->name == shown);
  // Copper is quoted per pound by the API and is kept per metric tonne, the unit of the instrument (another source's copper has the same ISIN).
  CHECK(std::abs(s->quote({*s->findInstrument("HG"), usd, none, data})->last - 5 * 1000 / 0.45359237) < 1e-6);
  CHECK(std::abs(s->quote({*s->findInstrument("HG"), eur, none, data})->last - 4.5 * 1000 / 0.45359237) < 1e-6);
  CHECK(std::abs(s->quote({*s->findInstrument("XAG"), usd, none, data})->last - 45) < 1e-9);  // the others as they come
  // The metals are commodities (so in the Commodities preset), the coins are of no kind.
  for (const char * metal : {"XAU", "XAG", "XPT", "XPD", "HG"}) CHECK(s->kind(*s->findInstrument(metal)) == InstrumentKind::Commodity);
  for (const char * coin : {"BTC", "ETH"}) CHECK(s->kind(*s->findInstrument(coin)) == InstrumentKind::Other);
  CHECK(s->name(NodeRef(*s->findInstrument("XPT"))) == "Platinum" && std::string(s->said(NodeRef(gold), predicate::type)) == "METAL");
  // Not tradable: a spot price is priced and not bought.
  CHECK(!portfolio::tradable(*s, EdgeKey{gold, eur, none, data}));
}

TEST(a_second_load_replaces_the_quote_and_leaves_no_bar) {
  auto store = makeMemoryStore();
  FakeHttp http;
  double gold = 3600;
  std::string updated = "2026-11-24T14:19:44Z";
  http.answer = [&](const std::string & url) {
    if (url.find("/symbols") != std::string::npos) return ok(R"([{"name":"Gold","symbol":"XAU"}])");
    return ok(priceBody("XAU", "Gold", url.substr(url.rfind('/') + 1), gold, updated));
  };
  Said said;
  GoldApiSource source(std::chrono::minutes(2), std::chrono::milliseconds(0));
  source.load(*store, http, said);
  gold = 3650;
  source.load(*store, http, said);
  auto s = store->snapshot();
  const EdgeKey usd{*s->findInstrument("XAU"), *s->findInstrument("USD"), *s->findMarket(kNoMarket), *s->findDataset(kGoldApiDataset)};
  CHECK(s->quote(usd)->last == 3650 && s->candles(usd, Resolution::Day).candles.empty());
  updated = "2026-11-25T00:00:05Z";
  source.load(*store, http, said);
  const auto quote = store->snapshot()->quote(usd);
  CHECK(quote->last == 3650 && dayNumberOfInstant(quote->time) == dayNumber(2026, 11, 25));
  CHECK(store->snapshot()->candles(usd, Resolution::Day).candles.empty());
  const uint64_t version = store->snapshot()->version();
  source.load(*store, http, said);  // the same again
  CHECK(store->snapshot()->version() == version);
}

TEST(a_refusal_ends_the_load_and_another_failure_only_that_request) {
  {
    auto store = makeMemoryStore();
    FakeHttp http;
    http.answer = [](const std::string & url) { return url.find("XAG") != std::string::npos ? failed(429) : answerPrices(url); };
    Said said;
    GoldApiSource source(std::chrono::minutes(2), std::chrono::milliseconds(0));
    source.load(*store, http, said);
    const auto s = store->snapshot();
    CHECK(s->findInstrument("XAU") && !s->findInstrument("XPT"));  // gold was first; silver was refused, and the rest were not asked for
    CHECK(said.messages.size() == 1 && said.messages[0].find("XAG/EUR: HTTP 429, stopping") != std::string::npos);
    CHECK(http.urls.back().find("XAG/EUR") != std::string::npos);
  }
  {
    auto store = makeMemoryStore();
    FakeHttp http;
    http.answer = [](const std::string & url) { return url.find("XAG/EUR") != std::string::npos ? failed(500) : answerPrices(url); };
    Said said;
    GoldApiSource source(std::chrono::minutes(2), std::chrono::milliseconds(0));
    source.load(*store, http, said);
    const auto s = store->snapshot();
    const InstrumentId silver = *s->findInstrument("XAG");
    CHECK(s->edges(*s->findMarket(kNoMarket)).size() == 13 && s->findInstrument("ETH"));  // only that one price is missing
    CHECK(said.messages.size() == 1 && said.messages[0].find("XAG/EUR: HTTP 500") != std::string::npos && silver.valid());
  }
}

TEST(an_answer_for_another_symbol_or_currency_or_no_price_is_not_taken_for_the_one_asked) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("/symbols") != std::string::npos) return ok(R"([{"name":"Gold","symbol":"XAU"}])");
    if (url.find("XAU/EUR") != std::string::npos) return ok(priceBody("XAU", "Gold", "USD", 3600));      // the wrong currency
    return ok("<html>maintenance</html>");
  };
  Said said;
  GoldApiSource source(std::chrono::minutes(2), std::chrono::milliseconds(0));
  source.load(*store, http, said);
  CHECK(!store->snapshot()->findInstrument("XAU") && said.messages.size() == 2);
  CHECK(said.messages[0].find("the answer is for XAU/USD") != std::string::npos && said.messages[1].find("not a price") != std::string::npos);
}

TEST(a_failed_symbols_request_uses_the_list_from_before_and_with_none_says_so) {
  auto store = makeMemoryStore();
  FakeHttp http;
  bool down = true;
  http.answer = [&](const std::string & url) { return down && url.find("/symbols") != std::string::npos ? failed(503) : answerPrices(url); };
  Said said;
  GoldApiSource source(std::chrono::minutes(2), std::chrono::milliseconds(0));
  source.load(*store, http, said);
  CHECK(http.urls.size() == 1 && said.messages.size() == 1 && said.messages[0].find("symbols: HTTP 503") != std::string::npos);
  down = false;
  source.load(*store, http, said);
  CHECK(store->snapshot()->findInstrument("XPT"));
  down = true;
  said.messages.clear();
  source.load(*store, http, said);  // the list is not there, the prices of the one before are asked for again
  CHECK(said.messages.size() == 1 && said.messages[0].find("using the list from before") != std::string::npos);
  CHECK(source.refreshInterval() == std::chrono::minutes(2) && source.datasetCodes() == std::vector<std::string>{kGoldApiDataset} && source.name() == "goldapi");
}

TEST(the_gold_estimate_is_made_in_euros_and_in_dollars_from_gold_api_com_and_beats_the_other_spot) {
  auto store = makeMemoryStore();
  // Nordnet's gold, in dollars only, a little different; and gold-api.com's, in both.
  InstrumentId usd = store->addInstrument("USD", "US dollar", InstrumentKind::Currency), eur = store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId kulta = store->addInstrument("GOLD", "Kulta", InstrumentKind::Commodity, "XC0009655157");
  MarketId none = store->addMarket(kNoMarket, "No market");
  DatasetId nordnet = store->addDataset("nordnet");
  const EdgeKey spot{kulta, usd, none, nordnet};
  const int64_t day = dayNumber(2026, 11, 24);
  store->appendCandles(spot, Resolution::Day, {{day, kUndefined, kUndefined, kUndefined, 3500, kUndefined}});
  store->setListing({spot, "GOLD", "", "", "COMMODITY", "Kulta"});
  FakeHttp http;
  Said said;
  PhysicalTable table;
  std::string error;
  CHECK(parsePhysicalTable("spot,gold,XAU\nspot,gold,GOLD\nitem,gold,GOLDBAR,Gold bar (1 g fine),0.0321507466,0.97,1.04\nitem,gold,GOLDCOIN,Gold coin (1 oz fine),1,0.98,1.08\n", table, error));
  PhysicalSource estimate(table);
  // Before gold-api.com has answered: Nordnet's dollars only.
  estimate.load(*store, http, said);
  auto s = store->snapshot();
  const MarketId off = *s->findMarket(kOffExchange);
  CHECK(s->edges(off).size() == 2);
  GoldApiSource prices(std::chrono::minutes(2), std::chrono::milliseconds(0));
  http.answer = answerPrices;
  prices.load(*store, http, said);
  estimate.load(*store, http, said);
  s = store->snapshot();
  CHECK(s->edges(off).size() == 4);  // the bar and the coin, each in euros and in dollars
  const DatasetId estimateData = *s->findDataset(kPhysicalDataset);
  const InstrumentId coin = *s->findInstrument("GOLDCOIN"), bar = *s->findInstrument("GOLDBAR");
  const double inEuros = s->lastCandles({coin, eur, off, estimateData}, Resolution::Day, 1).candles.back().close;
  const double inDollars = s->lastCandles({coin, usd, off, estimateData}, Resolution::Day, 1).candles.back().close;
  CHECK(std::abs(inEuros - 3240 * 0.98) < 1e-6);     // gold-api.com's euros
  CHECK(std::abs(inDollars - 3600 * 0.98) < 1e-6);   // and its dollars, not Nordnet's 3500
  CHECK(std::abs(s->lastCandles({bar, eur, off, estimateData}, Resolution::Day, 1).candles.back().close - 3240 * 0.0321507466 * 0.97) < 1e-6);
  // A portfolio in euros holds the bar and values it in euros directly, with no pair needed.
  const PortfolioId p{portfolioKey(1)->value};
  const DatasetId user = store->addDataset(kUserDataset);
  store->setAttributes(NodeRef(p), {{predicate::name, "Gold"}, {predicate::currency, NodeKey{eur.value}}});
  portfolio::Transaction t;
  t.id = 1;
  t.day = day - 5;
  t.instrument = bar;
  t.market = off;
  t.quote = eur;
  t.quantity = 100;
  t.price = 3300 * 0.0321507466 * 1.04;
  store->appendTransactions({p, user}, {t});
  const auto after = store->snapshot();
  const portfolio::Valuation v = portfolio::value(portfolio::holdings(*after, p).positions, *after, eur);
  CHECK(v.rows.size() == 1 && v.rows[0].rate && *v.rows[0].rate == 1.0 && !v.incomplete);
  CHECK(v.rows[0].value && std::abs(*v.rows[0].value - 100 * 3240 * 0.0321507466 * 0.97) < 1e-4);
}

TEST(a_sample_of_physical_platinum_is_priced_from_the_platinum_spot_in_both_currencies_and_gold_is_not_touched) {
  auto store = makeMemoryStore();
  FakeHttp http;
  Said said;
  PhysicalTable shipped;
  std::string error;
  CHECK(loadPhysicalTable(shipped, error));  // the file the program ships with
  PhysicalSource estimate(shipped);
  // Only gold has a price at first (the other spot has not answered): the platinum is waited for, the gold is not.
  GoldApiSource prices(std::chrono::minutes(2), std::chrono::milliseconds(0), {"EUR", "USD"});
  http.answer = [](const std::string & url) { return url.find("XPT") != std::string::npos ? failed(500) : answerPrices(url); };
  prices.load(*store, http, said);
  estimate.load(*store, http, said);
  auto s = store->snapshot();
  const MarketId off = *s->findMarket(kOffExchange);
  CHECK(s->edges(off).size() == 4 && !s->findInstrument("PLATBAR"));
  http.answer = answerPrices;
  prices.load(*store, http, said);
  estimate.load(*store, http, said);
  s = store->snapshot();
  CHECK(s->edges(off).size() == 6);  // the gold bar, the gold coin and the platinum bar, each in euros and in dollars
  const DatasetId data = *s->findDataset(kPhysicalDataset);
  const InstrumentId bar = *s->findInstrument("PLATBAR"), eur = *s->findInstrument("EUR"), usd = *s->findInstrument("USD");
  CHECK(s->name(NodeRef(bar)) == "Platinum bar (1 g fine)" && s->kind(bar) == InstrumentKind::Commodity && portfolio::tradable(*s, EdgeKey{bar, eur, off, data}));
  const double gram = 0.0321507466;
  CHECK(std::abs(s->lastCandles({bar, eur, off, data}, Resolution::Day, 1).candles.back().close - 1300 * 0.9 * gram * 0.93) < 1e-6);
  CHECK(std::abs(s->lastCandles({bar, usd, off, data}, Resolution::Day, 1).candles.back().close - 1300 * gram * 0.93) < 1e-6);
  // The gold bar is the gold's, not the platinum's.
  const InstrumentId goldBar = *s->findInstrument("GOLDBAR");
  CHECK(std::abs(s->lastCandles({goldBar, usd, off, data}, Resolution::Day, 1).candles.back().close - 3600 * gram * 0.97) < 1e-6);
}

TEST(a_metal_has_the_isin_of_its_commodity_and_is_the_instrument_another_source_made_by_it) {
  auto store = makeMemoryStore();
  // Nordnet's list was there first: gold by its ISIN, called Kulta.
  const InstrumentId nordnetGold = store->addInstrument("", "Kulta", InstrumentKind::Commodity, "XC0009655157", "COMMODITY");
  FakeHttp http;
  http.answer = answerPrices;
  Said said;
  GoldApiSource source(std::chrono::minutes(2), std::chrono::milliseconds(0));
  source.load(*store, http, said);
  const auto s = store->snapshot();
  // The same instrument: one gold, with its name and type as they were said, and the prices of both sources on it.
  CHECK(s->findInstrumentByIsin("XC0009655157") == std::optional<InstrumentId>(nordnetGold) && s->findInstrument("XAU") == std::optional<InstrumentId>(nordnetGold));
  CHECK(s->name(NodeRef(nordnetGold)) == "Kulta" && s->said(NodeRef(nordnetGold), predicate::type) == "COMMODITY");
  const MarketId none = *s->findMarket(kNoMarket);
  CHECK(s->edges(none).size() == 14 && s->hasCandles({nordnetGold, *s->findInstrument("EUR"), none, *s->findDataset(kGoldApiDataset)}, Resolution::Day));
  // Every symbol has a real (valid) ISIN, the ones source/KnownIsins.h names, and none is one of ours: there is no second gold, and the coins are not private.
  for (auto [symbol, isin] : {std::pair{"XAU", "XC0009655157"}, {"XAG", "XC0009653103"}, {"XPT", "XC0009665545"}, {"XPD", "XC0009665529"}, {"HG", "XC0005705501"},
                              {"BTC", "XTV15WLZJMF0"}, {"ETH", "XTD5RG2FHH04"}}) {
    CHECK(isValidIsin(isin) && s->isin(*s->findInstrument(symbol)) == isin && std::string(isin).rfind("ZZ", 0) != 0);
  }
}
