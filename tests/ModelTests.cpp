#include "TestFramework.h"

#include "model/Store.h"

using namespace stocks;

namespace {

struct World {
  std::unique_ptr<Store> store = makeMemoryStore();
  Instrument nokia = store->addInstrument({{}, "NOKIA", "Nokia", InstrumentKind::Company});
  Instrument eur = store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Market hel = store->addMarket({{}, "Nasdaq Helsinki"});
  Market sto = store->addMarket({{}, "Nasdaq Stockholm"});
  Dataset a = store->addDataset({{}, "provider-a"});
  Dataset b = store->addDataset({{}, "provider-b"});
  EdgeKey key(MarketId m, DatasetId d) const { return {nokia.id, eur.id, m, d}; }
};

Candle bar(Timestamp t, double close) { return {t, close, close, close, close, 1}; }

}  // namespace

TEST(adding_the_same_symbol_or_name_returns_the_record_already_there) {
  World w;
  Instrument again = w.store->addInstrument({{}, "NOKIA", "Other name", InstrumentKind::Index});
  CHECK(again.id == w.nokia.id && again.name == "Nokia" && again.kind == InstrumentKind::Company);
  CHECK(w.store->addMarket({{}, "Nasdaq Helsinki"}).id == w.hel.id);
  CHECK(w.store->addDataset({{}, "provider-a"}).id == w.a.id);
  CHECK(w.store->instruments().size() == 2 && w.store->markets().size() == 2 && w.store->datasets().size() == 2);
}

TEST(records_are_found_by_id_and_by_name) {
  World w;
  CHECK(w.store->instrument(w.eur.id)->symbol == "EUR");
  CHECK(w.store->findInstrument("NOKIA")->id == w.nokia.id);
  CHECK(!w.store->findInstrument("NOPE"));
  CHECK(!w.store->instrument({}) && !w.store->instrument({99}));
  CHECK(w.store->findMarket("Nasdaq Stockholm")->id == w.sto.id && w.store->market(w.hel.id)->name == "Nasdaq Helsinki");
  CHECK(w.store->findDataset("provider-b")->id == w.b.id && !w.store->dataset({}));
}

TEST(candles_stay_time_ordered_and_a_bar_at_a_known_time_replaces_it) {
  World w;
  EdgeKey k = w.key(w.hel.id, w.a.id);
  CHECK(w.store->appendCandles(k, 60000, {bar(120000, 3), bar(0, 1)}));
  CHECK(w.store->appendCandles(k, 60000, {bar(60000, 2), bar(120000, 30)}));
  CandleSeries s = w.store->candles(k);
  CHECK(s.intervalMs == 60000 && s.candles.size() == 3);
  CHECK(s.candles[0].time == 0 && s.candles[1].time == 60000 && s.candles[2].time == 120000);
  CHECK_NEAR(s.candles[2].close, 30, 1e-9);
}

TEST(candle_ranges_are_half_open_and_last_candles_takes_the_tail) {
  World w;
  EdgeKey k = w.key(w.hel.id, w.a.id);
  w.store->appendCandles(k, 1000, {bar(0, 1), bar(1000, 2), bar(2000, 3), bar(3000, 4)});
  CandleSeries r = w.store->candles(k, {1000, 3000});
  CHECK(r.candles.size() == 2 && r.candles[0].time == 1000 && r.candles[1].time == 2000);
  CandleSeries t = w.store->lastCandles(k, 2);
  CHECK(t.candles.size() == 2 && t.candles[0].time == 2000 && t.candles[1].time == 3000);
  CHECK(w.store->lastCandles(k, 99).candles.size() == 4);
  CHECK(w.store->candles(w.key(w.hel.id, w.b.id)).candles.empty());
  CHECK(w.store->lastCandles(w.key(w.hel.id, w.b.id), 3).candles.empty());
}

TEST(a_series_keeps_its_interval_and_refuses_another) {
  World w;
  EdgeKey k = w.key(w.hel.id, w.a.id);
  CHECK(!w.store->appendCandles(k, 0, {bar(0, 1)}));
  CHECK(w.store->appendCandles(k, 1000, {bar(0, 1)}));
  CHECK(!w.store->appendCandles(k, 2000, {bar(5000, 9)}));
  CHECK(w.store->candles(k).candles.size() == 1 && w.store->candles(k).intervalMs == 1000);
}

TEST(sources_that_disagree_about_one_pair_on_one_market_both_stay) {
  World w;
  w.store->appendCandles(w.key(w.hel.id, w.a.id), 1000, {bar(0, 5.0)});
  w.store->appendCandles(w.key(w.hel.id, w.b.id), 1000, {bar(0, 5.1)});
  CHECK_NEAR(w.store->candles(w.key(w.hel.id, w.a.id)).candles[0].close, 5.0, 1e-9);
  CHECK_NEAR(w.store->candles(w.key(w.hel.id, w.b.id)).candles[0].close, 5.1, 1e-9);
  CHECK(w.store->edges(w.hel.id).size() == 2);
}

TEST(edges_lists_the_keys_with_data_in_a_market) {
  World w;
  CHECK(w.store->edges(w.hel.id).empty());
  w.store->appendCandles(w.key(w.hel.id, w.a.id), 1000, {bar(0, 1)});
  w.store->appendCandles(w.key(w.sto.id, w.a.id), 1000, {bar(0, 1)});
  w.store->appendCandles(w.key(w.hel.id, w.a.id), 1000, {});  // nothing to add creates nothing
  w.store->appendCandles(w.key(w.sto.id, w.b.id), 1000, {});
  w.store->appendBooks(w.key(w.hel.id, w.b.id), {{0, {{1, 1}}, {{2, 1}}}});  // a book alone makes an edge too
  std::vector<EdgeKey> hel = w.store->edges(w.hel.id);
  CHECK(hel.size() == 2);
  CHECK(w.store->edges(w.sto.id).size() == 1);
  CHECK(hel[0].market == w.hel.id && hel[1].market == w.hel.id);
}

TEST(a_book_at_a_time_is_the_latest_snapshot_at_or_before_it) {
  World w;
  EdgeKey k = w.key(w.hel.id, w.a.id);
  BookSnapshot s1{1000, {{9.9, 100}, {9.8, 200}}, {{10.1, 150}}};
  BookSnapshot s2{2000, {{10.0, 50}}, {{10.2, 60}, {10.3, 70}}};
  w.store->appendBooks(k, {s2, s1});
  CHECK(!w.store->bookAt(k, 999));
  CHECK(w.store->bookAt(k, 1000)->bids.size() == 2);
  CHECK(w.store->bookAt(k, 1999)->time == 1000);
  CHECK(w.store->bookAt(k, 5000)->asks.size() == 2);
  CHECK(!w.store->bookAt(w.key(w.hel.id, w.b.id), 5000));
  CHECK(w.store->books(k, {1500, 3000}).size() == 1);
  w.store->appendBooks(k, {{2000, {}, {}}});  // same time: replaced
  CHECK(w.store->books(k).size() == 2 && w.store->bookAt(k, 2000)->asks.empty());
}

TEST(announcements_are_kept_per_instrument_and_dataset_in_time_order) {
  World w;
  EventKey ka{w.nokia.id, w.a.id}, kb{w.nokia.id, w.b.id};
  w.store->appendAnnouncements(ka, {{2000, "results", "Q2 results", "Q2", ""}, {1000, "guidance", "Raises guidance", "", ""}});
  w.store->appendAnnouncements(ka, {{2000, "results", "Q2 results", "Q2 (corrected)", ""}});  // same time and headline
  w.store->appendAnnouncements(ka, {{2000, "mgmt", "New CFO", "", ""}});                      // same time, other headline
  w.store->appendAnnouncements(kb, {{1500, "results", "Other source", "", ""}});
  std::vector<Announcement> a = w.store->announcements(ka);
  CHECK(a.size() == 3 && a[0].headline == "Raises guidance" && a[1].body == "Q2 (corrected)" && a[2].headline == "New CFO");
  CHECK(w.store->announcements(ka, {1500, 2500}).size() == 2);
  CHECK(w.store->announcements(kb).size() == 1);
  CHECK(w.store->announcements({w.eur.id, w.a.id}).empty());
}

TEST(corporate_actions_carry_their_kind_and_value) {
  World w;
  EventKey k{w.nokia.id, w.a.id};
  CorporateAction div{5000, ActionKind::Dividend, 0.14, w.eur.id, ""};
  CorporateAction split{3000, ActionKind::Split, 2.0, {}, "2:1"};
  w.store->appendActions(k, {div, split});
  std::vector<CorporateAction> acts = w.store->actions(k);
  CHECK(acts.size() == 2 && acts[0].kind == ActionKind::Split && acts[1].kind == ActionKind::Dividend);
  CHECK_NEAR(acts[1].value, 0.14, 1e-12);
  CHECK(acts[1].currency == w.eur.id);
  w.store->appendActions(k, {{5000, ActionKind::Dividend, 0.15, w.eur.id, "revised"}});
  CHECK(w.store->actions(k).size() == 2 && w.store->actions(k)[1].note == "revised");
}

TEST(events_lists_the_datasets_that_say_something_about_an_instrument) {
  World w;
  CHECK(w.store->events(w.nokia.id).empty());
  w.store->appendAnnouncements({w.nokia.id, w.a.id}, {{1, "x", "x", "", ""}});
  w.store->appendActions({w.nokia.id, w.a.id}, {{1, ActionKind::Split, 2, {}, ""}});
  w.store->appendActions({w.nokia.id, w.b.id}, {{1, ActionKind::Split, 2, {}, ""}});
  CHECK(w.store->events(w.nokia.id).size() == 2);
  CHECK(w.store->events(w.eur.id).empty());
}
