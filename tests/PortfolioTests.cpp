#include "TestFramework.h"

#include "model/Dates.h"
#include "model/Store.h"
#include "portfolio/Portfolio.h"

using namespace portfolio;

namespace {

constexpr int64_t kDay = dayNumber(2026, 12, 1);

// Instruments on markets, with prices: Nokia in euro on Helsinki, Apple in dollars on Nasdaq, a fund off exchange, an index, a
// currency pair and a rate that no market lists.
struct World {
  std::unique_ptr<Store> store = makeMemoryStore();
  InstrumentId eur = store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  InstrumentId sek = store->addInstrument("SEK", "Swedish krona", InstrumentKind::Currency);
  InstrumentId nokia = store->addInstrument("", "Nokia Oyj", InstrumentKind::Company, "FI0009000681");
  InstrumentId apple = store->addInstrument("", "Apple", InstrumentKind::Company, "US0378331005");
  InstrumentId volvo = store->addInstrument("", "Volvo", InstrumentKind::Company, "SE0000115446");
  InstrumentId index = store->addInstrument("OMXH25", "OMX Helsinki 25", InstrumentKind::Index);
  InstrumentId fund = store->addInstrument("AFUND", "A fund", InstrumentKind::Fund);
  InstrumentId euribor = store->addInstrument("EURIBOR12", "Euribor 12m", InstrumentKind::Other);
  InstrumentId pct = store->addInstrument("pct", "Per cent", InstrumentKind::Unit);
  MarketId hel = store->addMarket("XHEL", "Nasdaq Helsinki");
  MarketId nasdaq = store->addMarket("XNAS", "Nasdaq");
  MarketId sto = store->addMarket("XSTO", "Nasdaq Stockholm");
  MarketId off = store->addMarket(kOffExchange, "Off exchange");
  MarketId none = store->addMarket(kNoMarket, "No market");
  DatasetId a = store->addDataset("providera");
  DatasetId b = store->addDataset("providerb");

  EdgeKey price(InstrumentId base, InstrumentId quote, MarketId market, double close, DatasetId d, int64_t day = kDay) {
    EdgeKey key{base, quote, market, d};
    store->appendCandles(key, Resolution::Day, {{day, close, close, close, close, 1}});
    return key;
  }
  World() {
    price(nokia, eur, hel, 5, a);
    price(apple, usd, nasdaq, 200, a);
    price(volvo, sek, sto, 300, a);
    price(index, pct, none, 1000, a);
    price(euribor, pct, none, 2.5, a);
    price(fund, eur, off, 12, a);
    price(eur, usd, none, 1.25, a);  // one euro costs 1.25 dollars
  }
  std::shared_ptr<const Snapshot> snap() const { return store->snapshot(); }
};

Transaction buy(const World & w, uint64_t id, int64_t day, double quantity, double price, double fee = 0) {
  Transaction t;
  t.id = id;
  t.day = day;
  t.side = Side::Buy;
  t.instrument = w.nokia;
  t.market = w.hel;
  t.quote = w.eur;
  t.quantity = quantity;
  t.price = price;
  t.fee = fee;
  return t;
}
Transaction sell(const World & w, uint64_t id, int64_t day, double quantity, double price, double fee = 0) {
  Transaction t = buy(w, id, day, quantity, price, fee);
  t.side = Side::Sell;
  return t;
}

}  // namespace

TEST(buys_make_a_position_at_the_average_cost_fees_included) {
  World w;
  const Derived d = derivePositions({buy(w, 1, kDay, 10, 4, 1), buy(w, 2, kDay + 1, 10, 6)});
  CHECK(d.refused.empty() && d.positions.size() == 1);
  const Position & p = d.positions[0];
  CHECK(p.instrument == w.nokia && p.quote == w.eur);
  CHECK(p.quantity == 20);
  CHECK_NEAR(p.average_cost, (10 * 4 + 1 + 10 * 6) / 20.0, 1e-12);
  CHECK_NEAR(p.cost(), 101, 1e-9);
  CHECK(p.history.size() == 2 && p.history[0].day == kDay && p.history[0].quantity == 10 && p.history[1].quantity == 20);
}

TEST(a_sell_keeps_the_average_cost_and_takes_from_the_quantity) {
  World w;
  const Derived d = derivePositions({buy(w, 1, kDay, 10, 4), buy(w, 2, kDay + 1, 10, 6), sell(w, 3, kDay + 2, 5, 100, 2)});
  CHECK(d.refused.empty() && d.positions.size() == 1);
  CHECK(d.positions[0].quantity == 15 && d.positions[0].average_cost == 5);  // what it sold at is no part of the cost
  CHECK(d.positions[0].history.back().day == kDay + 2 && d.positions[0].history.back().quantity == 15);
}

TEST(a_sell_of_more_than_is_held_is_refused_and_the_rest_stand) {
  World w;
  const Derived d = derivePositions({buy(w, 1, kDay, 10, 4), sell(w, 2, kDay + 1, 11, 5), sell(w, 3, kDay + 2, 4, 5)});
  CHECK(d.refused.size() == 1 && d.refused[0].id == 2 && !d.refused[0].why.empty());
  CHECK(d.positions.size() == 1 && d.positions[0].quantity == 6 && d.positions[0].average_cost == 4);
  // Selling what was never bought is the same.
  CHECK(derivePositions({sell(w, 1, kDay, 1, 1)}).refused.size() == 1);
  CHECK(derivePositions({sell(w, 1, kDay, 1, 1)}).positions.empty());
}

TEST(a_position_closed_and_opened_again_starts_again) {
  World w;
  const Derived d = derivePositions({buy(w, 1, kDay, 10, 4), sell(w, 2, kDay + 1, 10, 9), buy(w, 3, kDay + 2, 5, 8)});
  CHECK(d.refused.empty() && d.positions.size() == 1);
  CHECK(d.positions[0].quantity == 5 && d.positions[0].average_cost == 8);  // not mixed with the old 4
  CHECK(d.positions[0].history.size() == 3 && d.positions[0].history[1].quantity == 0);  // the series shows it was closed
  // Closed and not opened again: no position at all.
  CHECK(derivePositions({buy(w, 1, kDay, 10, 4), sell(w, 2, kDay + 1, 10, 9)}).positions.empty());
}

TEST(on_one_day_the_buys_come_first_whatever_the_ids) {
  World w;
  const Derived d = derivePositions({sell(w, 1, kDay, 5, 5), buy(w, 2, kDay, 10, 4)});  // the sell has the lower id
  CHECK(d.refused.empty() && d.positions.size() == 1 && d.positions[0].quantity == 5);
  // The order the list is given in does not matter either.
  const Derived e = derivePositions({buy(w, 2, kDay + 1, 10, 4), buy(w, 1, kDay, 10, 6)});
  CHECK(e.positions[0].average_cost == 5 && e.positions[0].history[0].day == kDay);
}

TEST(a_transaction_that_makes_no_sense_is_refused) {
  World w;
  Transaction nothing = buy(w, 1, kDay, 0, 4), negative = buy(w, 2, kDay, 1, -1), fee = buy(w, 3, kDay, 1, 1, -1), nan = buy(w, 4, kDay, 1, kUndefined);
  Transaction nowhere = buy(w, 5, kDay, 1, 1);
  nowhere.quote = InstrumentId{};
  const Derived d = derivePositions({nothing, negative, fee, nan, nowhere, buy(w, 6, kDay, 1, 1)});
  CHECK(d.refused.size() == 5 && d.positions.size() == 1 && d.positions[0].quantity == 1);
}

TEST(a_position_is_valued_in_the_currency_of_the_portfolio_through_the_stores_own_edges) {
  World w;
  Transaction apple = buy(w, 3, kDay, 2, 150);
  apple.instrument = w.apple;
  apple.market = w.nasdaq;
  apple.quote = w.usd;
  const Derived d = derivePositions({buy(w, 1, kDay, 10, 4), apple});
  const Valuation v = value(d.positions, *w.snap(), w.eur);
  CHECK(v.rows.size() == 2 && !v.incomplete);
  const PositionValue & nokia = v.rows[0];
  CHECK(nokia.price && nokia.price->close == 5 && nokia.market_value == 50 && nokia.rate && *nokia.rate == 1 && nokia.value && *nokia.value == 50);
  CHECK_NEAR(nokia.profit, 10, 1e-9);
  CHECK_NEAR(nokia.profit_percent, 25, 1e-9);
  const PositionValue & a = v.rows[1];  // 2 x 200 dollars, a euro costing 1.25 dollars
  CHECK(a.market_value == 400 && a.rate && std::abs(*a.rate - 0.8) < 1e-12 && a.value && std::abs(*a.value - 320) < 1e-9);
  CHECK_NEAR(a.profit, 100, 1e-9);  // in dollars, its own currency
  CHECK_NEAR(v.total, 370, 1e-9);
  CHECK(nokia.weight && a.weight && std::abs(*nokia.weight + *a.weight - 1) < 1e-12 && std::abs(*nokia.weight - 50.0 / 370) < 1e-12);
  // In dollars it is the other way round: euros cost 1.25 dollars.
  const Valuation dollars = value(d.positions, *w.snap(), w.usd);
  CHECK_NEAR(dollars.total, 62.5 + 400, 1e-9);
}

TEST(a_position_with_no_rate_stays_in_its_own_currency_and_the_total_says_it_is_incomplete) {
  World w;
  Transaction volvo = buy(w, 2, kDay, 3, 250);
  volvo.instrument = w.volvo;
  volvo.market = w.sto;
  volvo.quote = w.sek;
  const Derived d = derivePositions({buy(w, 1, kDay, 10, 4), volvo});
  const Valuation v = value(d.positions, *w.snap(), w.eur);
  CHECK(v.incomplete && v.rows.size() == 2);
  CHECK(!v.rows[1].rate && !v.rows[1].value && !v.rows[1].weight);  // no rate is invented
  CHECK(v.rows[1].market_value == 900 && v.rows[1].profit == 150);  // shown in kronor
  CHECK(v.total == 50 && v.rows[0].weight && *v.rows[0].weight == 1);  // the total is of what has a value
}

TEST(a_rate_is_found_through_one_other_currency_but_not_through_a_unit) {
  World w;
  CHECK(!rate(*w.snap(), w.sek, w.eur));
  w.price(w.usd, w.sek, w.none, 10, w.a);  // a dollar costs ten kronor
  const auto sek = rate(*w.snap(), w.sek, w.eur);  // krona -> dollar -> euro: 0.1 dollars, 0.08 euro
  CHECK(sek && std::abs(*sek - 0.08) < 1e-12);
  const auto back = rate(*w.snap(), w.eur, w.sek);
  CHECK(back && std::abs(*back - 12.5) < 1e-9);
  CHECK(rate(*w.snap(), w.eur, w.eur) == 1.0);
  CHECK(!rate(*w.snap(), w.eur, w.pct) && !rate(*w.snap(), w.eur, InstrumentId{}));  // a unit is no currency
}

TEST(the_price_is_the_members_together_and_a_member_that_is_not_live_is_left_out) {
  World w;
  w.price(w.nokia, w.eur, w.hel, 7, w.b, kDay + 1);  // a later bar, in another dataset: the first is no longer live
  const auto price = latestPrice(*w.snap(), w.nokia, w.eur);
  CHECK(price && price->close == 7 && price->edge.dataset == w.b && price->day == kDay + 1 && price->mix.used == 1);
  // An older bar elsewhere does not take over, and a price in another currency is not this one.
  w.price(w.nokia, w.eur, w.hel, 1, w.b, kDay - 5);
  CHECK(latestPrice(*w.snap(), w.nokia, w.eur)->close == 7);
  CHECK(!latestPrice(*w.snap(), w.nokia, w.usd));
  // The same day in two datasets: both count, and the first by key stands for them.
  World same;
  same.price(same.nokia, same.eur, same.hel, 6, same.b);
  const auto both = latestPrice(*same.snap(), same.nokia, same.eur);
  CHECK(both && both->close == 5.5 && both->edge.dataset == same.a && both->mix.used == 2);
}

TEST(a_price_on_another_market_of_the_same_unit_is_a_member_of_the_same_price) {
  World w;
  w.price(w.nokia, w.eur, w.sto, 6, w.a);  // the same day, in euro, on Stockholm
  const auto price = latestPrice(*w.snap(), w.nokia, w.eur);
  CHECK(price && price->close == 5.5 && price->mix.used == 2 && price->mix.members.size() == 2);
  CHECK(price->mix.members[0].edge.market != price->mix.members[1].edge.market);
}

TEST(only_what_a_venue_trades_or_a_company_prices_can_be_held) {
  World w;
  const auto s = w.snap();
  CHECK(tradable(*s, GroupKey{w.nokia, w.eur}));           // a real venue
  CHECK(tradable(*s, GroupKey{w.fund, w.eur}));            // XOFF: a fund priced by its company
  CHECK(!tradable(*s, GroupKey{w.index, w.pct}));          // an index
  CHECK(!tradable(*s, GroupKey{w.eur, w.usd}));            // a currency pair that only has a price
  CHECK(!tradable(*s, GroupKey{w.euribor, w.pct}));        // a rate
  CHECK(!tradable(*s, GroupKey{w.volvo, w.eur}));          // nothing says it
  CHECK(tradable(w.hel) && tradable(w.off) && !tradable(w.none) && !tradable(MarketId{}));
  // One member on a venue is enough, whatever else says its price: a spot price that is also fixed on a market is bought there, and a
  // currency pair once a forex market lists it.
  w.price(w.eur, w.usd, w.sto, 1.26, w.b);
  CHECK(tradable(*w.snap(), GroupKey{w.eur, w.usd}));
  // A share that a list priced on no market and another dataset on a venue is held too.
  w.price(w.apple, w.usd, w.none, 201, w.b);
  CHECK(tradable(*w.snap(), GroupKey{w.apple, w.usd}));
}

TEST(minted_ids_are_not_zero_and_fit_a_key) {
  for (int i = 0; i < 1000; i++) {
    const uint64_t id = mintId();
    CHECK(id != 0 && id < (uint64_t{1} << 62));
  }
  uint64_t before = mintId();
  for (int i = 0; i < 1000; i++) {
    const uint64_t id = mintId();
    CHECK(id > before);  // in the order they were made, though made in one millisecond
    before = id;
  }
}
