#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "portfolio/Portfolio.h"
#include "source/GoldSource.h"
#include "source/DatasetCodes.h"

namespace {

constexpr int64_t kDay = dayNumber(2026, 12, 1);
constexpr double kOunce = 31.1035;

struct GoldListener : SourceListener {
  std::vector<std::string> messages;
  int changes = 0;
  void changed() override { changes++; }
  void message(const std::string & m) override { messages.push_back(m); }
};

GoldTable table() {
  GoldTable t;
  std::string error;
  const bool ok = parseGoldTable(
      "# a comment\n\nspot,GOLD\nitem,GOLDBAR,Gold bar (1 g fine)," + std::to_string(1 / kOunce) + ",0.97,1.04\nitem,GOLDCOIN,Gold coin (1 oz fine),1,0.98,1.08\n", t, error);
  CHECK(ok);
  return t;
}

// What Nordnet leaves: gold per troy ounce in dollars, on the market of what is priced and not traded, and a currency pair.
struct World {
  std::shared_ptr<Store> store = makeMemoryStore();
  InstrumentId usd = store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  InstrumentId eur = store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId gold = store->addInstrument("GOLD", "Kulta", InstrumentKind::Commodity, "XC0009655157");
  MarketId none = store->addMarket(kNoMarket, "No market");
  DatasetId data = store->addDataset("nordnet");
  EdgeKey spot{gold, usd, none, data};
  void setSpot(double close, int64_t day = kDay) {
    store->appendCandles(spot, Resolution::Day, {{day, kUndefined, close * 1.01, close * 0.99, close, kUndefined}});
    store->setListing({spot, "GOLD", "", "", "COMMODITY", "Kulta"});
  }
  void setPair() { store->appendCandles({eur, usd, none, data}, Resolution::Day, {{kDay, 1, 1, 1, 1.25, 1}}); }
  std::optional<EdgeKey> edgeOf(const std::string & symbol) {
    const auto s = store->snapshot();
    const auto market = s->findMarket(kOffExchange);
    return market ? s->findEdge(symbol, *market) : std::nullopt;
  }
};

}  // namespace

TEST(the_gold_table_is_read_with_its_comments_and_a_wrong_row_says_which) {
  GoldTable t = table();
  CHECK(t.spots == std::vector<std::string>{"GOLD"} && t.items.size() == 2);
  CHECK(t.items[0].symbol == "GOLDBAR" && std::abs(t.items[0].ounces - 1 / kOunce) < 1e-6 && t.items[0].bid == 0.97 && t.items[0].ask == 1.04);
  CHECK(t.items[1].ounces == 1 && t.items[1].name == "Gold coin (1 oz fine)");
  std::string error;
  GoldTable bad;
  CHECK(!parseGoldTable("spot,GOLD\nitem,A,Name,1,x,1\n", bad, error) && error.find("line 2") == 0);
  CHECK(!parseGoldTable("spot,GOLD\nitem,A,Name,0,1,1\n", bad, error) && error.find("line 2") == 0);  // no weight
  CHECK(!parseGoldTable("spot,GOLD\nitem,A,Name,1,1\n", bad, error));
  CHECK(!parseGoldTable("item,A,Name,1,1,1\n", bad, error));  // no spot
  CHECK(!parseGoldTable("spot,GOLD\n", bad, error));          // no item
  CHECK(!parseGoldTable("spot,GOLD\nthing,1\n", bad, error) && error.find("line 2") == 0);
  GoldTable shipped;
  CHECK(loadGoldTable(shipped, error) || !error.empty());  // the file the program ships with reads, where the data is found
  if (error.empty()) CHECK(shipped.items.size() == 2 && shipped.items[0].symbol == "GOLDBAR" && shipped.items[1].symbol == "GOLDCOIN");
}

TEST(the_two_golds_are_priced_from_the_spot_off_exchange_in_their_own_dataset) {
  World w;
  w.setSpot(3000);
  GoldSource source(table());
  FakeHttp http;
  GoldListener listener;
  source.load(*w.store, http, listener);
  CHECK(listener.changes == 1 && listener.messages.empty());
  const auto bar = w.edgeOf("GOLDBAR"), coin = w.edgeOf("GOLDCOIN");
  CHECK(bar && coin);
  if (!bar || !coin) return;
  const auto s = w.store->snapshot();
  CHECK(bar->quote == w.usd && coin->quote == w.usd && bar->dataset == coin->dataset && s->name(bar->dataset) == "Gold (estimate)");
  CHECK(s->findDataset(kGoldDataset) && bar->market == *s->findMarket(kOffExchange));
  // A gram of fine gold at the dealer's bid, and an ounce coin at its.
  const Candle b = s->lastCandles(*bar, Resolution::Day, 1).candles.back(), c = s->lastCandles(*coin, Resolution::Day, 1).candles.back();
  CHECK(b.time == kDay && std::abs(b.close - 3000 / kOunce * 0.97) < 1e-2);
  CHECK(c.time == kDay && std::abs(c.close - 3000 * 0.98) < 1e-9);
  CHECK(std::abs(c.high - 3030 * 0.98) < 1e-9 && std::abs(c.low - 2970 * 0.98) < 1e-9 && !isDefined(c.open));
  // The names, the symbols and the listings, and they are instruments of their own that can be held.
  CHECK(s->name(NodeRef(bar->base)) == "Gold bar (1 g fine)" && s->symbol(*coin) == "GOLDCOIN" && s->kind(bar->base) == InstrumentKind::Commodity);
  CHECK(bar->base != coin->base && bar->base != w.gold);
  CHECK(portfolio::tradable(*s, *bar) && portfolio::tradable(*s, *coin));
  CHECK(!portfolio::tradable(*s, w.spot));  // the spot itself is not
}

TEST(the_estimate_follows_the_spot_and_a_new_day_is_a_new_bar) {
  World w;
  w.setSpot(3000);
  GoldSource source(table());
  FakeHttp http;
  GoldListener listener;
  source.load(*w.store, http, listener);
  w.setSpot(3100);  // the spot moved on the same day
  source.load(*w.store, http, listener);
  const auto coin = *w.edgeOf("GOLDCOIN");
  auto bars = w.store->snapshot()->candles(coin, Resolution::Day).candles;
  CHECK(bars.size() == 1 && std::abs(bars[0].close - 3100 * 0.98) < 1e-9);
  w.setSpot(3200, kDay + 1);
  source.load(*w.store, http, listener);
  bars = w.store->snapshot()->candles(coin, Resolution::Day).candles;
  CHECK(bars.size() == 2 && bars[1].time == kDay + 1 && std::abs(bars[1].close - 3200 * 0.98) < 1e-9);
  // Nothing changed: nothing written again.
  const uint64_t version = w.store->snapshot()->version();
  source.load(*w.store, http, listener);
  CHECK(w.store->snapshot()->version() == version);
}

TEST(with_no_spot_nothing_is_written_and_it_says_so_once_after_a_few_tries) {
  World w;  // no spot price yet: the other source is still loading
  GoldSource source(table());
  FakeHttp http;
  GoldListener listener;
  const uint64_t version = w.store->snapshot()->version();
  source.load(*w.store, http, listener);
  source.load(*w.store, http, listener);
  CHECK(listener.messages.empty() && listener.changes == 0);
  source.load(*w.store, http, listener);
  CHECK(listener.messages.size() == 1 && listener.messages[0].find("no gold spot price") != std::string::npos);
  source.load(*w.store, http, listener);
  CHECK(listener.messages.size() == 1 && w.store->snapshot()->version() == version && !w.edgeOf("GOLDBAR"));
  // It arrives, and the estimate follows.
  w.setSpot(3000);
  source.load(*w.store, http, listener);
  CHECK(w.edgeOf("GOLDBAR") && w.edgeOf("GOLDCOIN") && listener.changes == 1);
  // A spot under another symbol is not this one.
  World other;
  other.store->appendCandles({other.gold, other.usd, other.none, other.data}, Resolution::Day, {{kDay, 1, 1, 1, 3000, 1}});
  other.store->setListing({{other.gold, other.usd, other.none, other.data}, "XAU", "", "", "COMMODITY", "Gold"});
  GoldSource third(table());
  GoldListener quiet;
  third.load(*other.store, http, quiet);
  CHECK(!other.edgeOf("GOLDBAR") && quiet.changes == 0);
}

TEST(a_portfolio_holds_the_two_golds_and_values_them_in_euros_at_the_bid) {
  World w;
  w.setSpot(3000);
  w.setPair();  // a euro costs 1.25 dollars
  GoldSource source(table());
  FakeHttp http;
  GoldListener listener;
  source.load(*w.store, http, listener);
  const EdgeKey bar = *w.edgeOf("GOLDBAR"), coin = *w.edgeOf("GOLDCOIN");
  const PortfolioId p{portfolioKey(1)->value};
  const DatasetId user = w.store->addDataset(kUserDataset);
  w.store->setAttributes(NodeRef(p), {{predicate::name, "Gold"}, {predicate::currency, NodeKey{w.eur.value}}});
  auto trade = [&](uint64_t id, const EdgeKey & e, double quantity, double price) {
    portfolio::Transaction t;
    t.id = id;
    t.day = kDay - 30;
    t.instrument = e.base;
    t.market = e.market;
    t.quote = e.quote;
    t.quantity = quantity;
    t.price = price;
    return t;
  };
  // 100 grams of bar and 2 coins, bought at the dealer's ask (in dollars, the currency the price is in).
  w.store->appendTransactions({p, user}, {trade(1, bar, 100, 3000 / kOunce * 1.04), trade(2, coin, 2, 3000 * 1.08)});
  const auto s = w.store->snapshot();
  const portfolio::Valuation v = portfolio::value(portfolio::holdings(*s, p).positions, *s, w.eur);
  CHECK(v.rows.size() == 2 && !v.incomplete);
  const double bars = 100 * 3000 / kOunce * 0.97, coins = 2 * 3000 * 0.98;  // dollars
  CHECK(v.rows[0].value && std::abs(*v.rows[0].value - bars / 1.25) < 1e-4 * bars);
  CHECK(v.rows[1].value && std::abs(*v.rows[1].value - coins / 1.25) < 1e-6);
  // What the premium cost shows as a loss at the start: bought at the ask, valued at the bid.
  CHECK(v.rows[0].profit < 0 && v.rows[1].profit < 0);
  CHECK_NEAR(v.rows[1].profit, 2 * 3000 * (0.98 - 1.08), 1e-6);
}

TEST(the_gold_source_names_a_dataset_no_other_source_writes) {
  std::vector<std::unique_ptr<Source>> sources;
  sources.push_back(std::make_unique<GoldSource>(table()));
  CHECK(datasetCodeClash(sources).empty());
  CHECK(sources[0]->datasetCodes() == std::vector<std::string>{kGoldDataset});
  sources.push_back(std::make_unique<GoldSource>(table()));
  CHECK(!datasetCodeClash(sources).empty());
}
