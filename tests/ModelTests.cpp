#include "TestFramework.h"

#include "model/Store.h"

#include <atomic>
#include <thread>

namespace {

struct World {
  std::unique_ptr<Store> store = makeMemoryStore();
  Instrument nokia = store->addInstrument({{}, "NOKIA", "Nokia", InstrumentKind::Company});
  Instrument eur = store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Market hel = store->addMarket({{}, "Nasdaq Helsinki"});
  Market sto = store->addMarket({{}, "Nasdaq Stockholm"});
  Dataset a = store->addDataset({{}, "provider-a"});
  Dataset b = store->addDataset({{}, "provider-b"});
  std::shared_ptr<const Snapshot> snap() const { return store->snapshot(); }
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
  CHECK(w.snap()->instruments().size() == 2 && w.snap()->markets().size() == 2 && w.snap()->datasets().size() == 2);
}

TEST(records_are_found_by_id_and_by_name) {
  World w;
  CHECK(w.snap()->instrument(w.eur.id)->symbol == "EUR");
  CHECK(w.snap()->findInstrument("NOKIA")->id == w.nokia.id);
  CHECK(!w.snap()->findInstrument("NOPE"));
  CHECK(!w.snap()->instrument({}) && !w.snap()->instrument({99}));
  CHECK(w.snap()->findMarket("Nasdaq Stockholm")->id == w.sto.id && w.snap()->market(w.hel.id)->name == "Nasdaq Helsinki");
  CHECK(w.snap()->findDataset("provider-b")->id == w.b.id && !w.snap()->dataset({}));
}

TEST(candles_stay_time_ordered_and_a_bar_at_a_known_time_replaces_it) {
  World w;
  EdgeKey k = w.key(w.hel.id, w.a.id);
  CHECK(w.store->appendCandles(k, 60000, {bar(120000, 3), bar(0, 1)}));
  CHECK(w.store->appendCandles(k, 60000, {bar(60000, 2), bar(120000, 30)}));
  CandleSeries s = w.snap()->candles(k, 60000);
  CHECK(s.intervalMs == 60000 && s.candles.size() == 3);
  CHECK(s.candles[0].time == 0 && s.candles[1].time == 60000 && s.candles[2].time == 120000);
  CHECK_NEAR(s.candles[2].close, 30, 1e-9);
}

TEST(candle_ranges_are_half_open_and_last_candles_takes_the_tail) {
  World w;
  EdgeKey k = w.key(w.hel.id, w.a.id);
  w.store->appendCandles(k, 1000, {bar(0, 1), bar(1000, 2), bar(2000, 3), bar(3000, 4)});
  CandleSeries r = w.snap()->candles(k, 1000, {1000, 3000});
  CHECK(r.candles.size() == 2 && r.candles[0].time == 1000 && r.candles[1].time == 2000);
  CandleSeries t = w.snap()->lastCandles(k, 1000, 2);
  CHECK(t.candles.size() == 2 && t.candles[0].time == 2000 && t.candles[1].time == 3000);
  CHECK(w.snap()->lastCandles(k, 1000, 99).candles.size() == 4);
  CHECK(w.snap()->candles(w.key(w.hel.id, w.b.id), 1000).candles.empty());
  CHECK(w.snap()->lastCandles(w.key(w.hel.id, w.b.id), 1000, 3).candles.empty());
}

TEST(an_edge_has_a_series_for_each_interval_it_has_data_in) {
  World w;
  EdgeKey k = w.key(w.hel.id, w.a.id);
  CHECK(!w.store->appendCandles(k, 0, {bar(0, 1)}) && !w.store->appendCandles(k, -5, {bar(0, 1)}));
  CHECK(w.snap()->intervals(k).empty() && w.snap()->edges(w.hel.id).empty());  // nothing was made

  CHECK(w.store->appendCandles(k, interval::day, {bar(0, 10), bar(interval::day, 11)}));
  CHECK(w.store->appendCandles(k, interval::hour1, {bar(0, 1), bar(interval::hour1, 2), bar(2 * interval::hour1, 3)}));
  CHECK(w.store->appendCandles(k, interval::min5, {bar(0, 5)}));
  auto s = w.snap();
  CHECK((s->intervals(k) == std::vector<int64_t>{interval::min5, interval::hour1, interval::day}));  // shortest first
  CHECK(s->candles(k, interval::day).candles.size() == 2 && s->candles(k, interval::day).intervalMs == interval::day);
  CHECK(s->candles(k, interval::hour1).candles.size() == 3 && s->candles(k, interval::min5).candles.size() == 1);
  CHECK(s->candles(k, interval::week).candles.empty());  // an interval it has no data in
  CHECK(s->lastCandles(k, interval::hour1, 1).candles[0].close == 3);
  CHECK(s->edges(w.hel.id).size() == 1);  // one edge, whatever the number of series

  // A correction in one series leaves the others alone.
  w.store->appendCandles(k, interval::hour1, {bar(interval::hour1, 20)});
  s = w.snap();
  CHECK(s->candles(k, interval::hour1).candles.size() == 3 && s->candles(k, interval::hour1).candles[1].close == 20);
  CHECK(s->candles(k, interval::day).candles[1].close == 11 && s->candles(k, interval::min5).candles[0].close == 5);
}

TEST(the_intervals_are_the_lengths_the_providers_use) {
  CHECK(interval::min1 == 60000 && interval::min5 == 300000 && interval::min15 == 900000 && interval::min30 == 1800000);
  CHECK(interval::hour1 == 3600000 && interval::hour4 == 14400000 && interval::day == 86400000 && interval::week == 604800000);
}

TEST(sources_that_disagree_about_one_pair_on_one_market_both_stay) {
  World w;
  w.store->appendCandles(w.key(w.hel.id, w.a.id), 1000, {bar(0, 5.0)});
  w.store->appendCandles(w.key(w.hel.id, w.b.id), 1000, {bar(0, 5.1)});
  CHECK_NEAR(w.snap()->candles(w.key(w.hel.id, w.a.id), 1000).candles[0].close, 5.0, 1e-9);
  CHECK_NEAR(w.snap()->candles(w.key(w.hel.id, w.b.id), 1000).candles[0].close, 5.1, 1e-9);
  CHECK(w.snap()->edges(w.hel.id).size() == 2);
}

TEST(edges_lists_the_keys_with_data_in_a_market) {
  World w;
  CHECK(w.snap()->edges(w.hel.id).empty());
  w.store->appendCandles(w.key(w.hel.id, w.a.id), 1000, {bar(0, 1)});
  w.store->appendCandles(w.key(w.sto.id, w.a.id), 1000, {bar(0, 1)});
  w.store->appendCandles(w.key(w.hel.id, w.a.id), 1000, {});  // nothing to add creates nothing
  w.store->appendCandles(w.key(w.sto.id, w.b.id), 1000, {});
  w.store->appendBooks(w.key(w.hel.id, w.b.id), {{0, {{1, 1}}, {{2, 1}}}});  // a book alone makes an edge too
  std::vector<EdgeKey> hel = w.snap()->edges(w.hel.id);
  CHECK(hel.size() == 2);
  CHECK(w.snap()->edges(w.sto.id).size() == 1);
  CHECK(hel[0].market == w.hel.id && hel[1].market == w.hel.id);
}

TEST(a_book_at_a_time_is_the_latest_snapshot_at_or_before_it) {
  World w;
  EdgeKey k = w.key(w.hel.id, w.a.id);
  BookSnapshot s1{1000, {{9.9, 100}, {9.8, 200}}, {{10.1, 150}}};
  BookSnapshot s2{2000, {{10.0, 50}}, {{10.2, 60}, {10.3, 70}}};
  w.store->appendBooks(k, {s2, s1});
  CHECK(!w.snap()->bookAt(k, 999));
  CHECK(w.snap()->bookAt(k, 1000)->bids.size() == 2);
  CHECK(w.snap()->bookAt(k, 1999)->time == 1000);
  CHECK(w.snap()->bookAt(k, 5000)->asks.size() == 2);
  CHECK(!w.snap()->bookAt(w.key(w.hel.id, w.b.id), 5000));
  CHECK(w.snap()->books(k, {1500, 3000}).size() == 1);
  w.store->appendBooks(k, {{2000, {}, {}}});  // same time: replaced
  CHECK(w.snap()->books(k).size() == 2 && w.snap()->bookAt(k, 2000)->asks.empty());
}

TEST(announcements_are_kept_per_instrument_and_dataset_in_time_order) {
  World w;
  EventKey ka{w.nokia.id, w.a.id}, kb{w.nokia.id, w.b.id};
  w.store->appendAnnouncements(ka, {{2000, "results", "Q2 results", "Q2", ""}, {1000, "guidance", "Raises guidance", "", ""}});
  w.store->appendAnnouncements(ka, {{2000, "results", "Q2 results", "Q2 (corrected)", ""}});  // same time and headline
  w.store->appendAnnouncements(ka, {{2000, "mgmt", "New CFO", "", ""}});                      // same time, other headline
  w.store->appendAnnouncements(kb, {{1500, "results", "Other source", "", ""}});
  auto a = w.snap()->announcements(ka);
  CHECK(a.size() == 3 && a[0].headline == "Raises guidance" && a[1].body == "Q2 (corrected)" && a[2].headline == "New CFO");
  CHECK(w.snap()->announcements(ka, {1500, 2500}).size() == 2);
  CHECK(w.snap()->announcements(kb).size() == 1);
  CHECK(w.snap()->announcements({w.eur.id, w.a.id}).empty());
}

TEST(corporate_actions_carry_their_kind_and_value) {
  World w;
  EventKey k{w.nokia.id, w.a.id};
  CorporateAction div{5000, ActionKind::Dividend, 0.14, w.eur.id, ""};
  CorporateAction split{3000, ActionKind::Split, 2.0, {}, "2:1"};
  w.store->appendActions(k, {div, split});
  auto acts = w.snap()->actions(k);
  CHECK(acts.size() == 2 && acts[0].kind == ActionKind::Split && acts[1].kind == ActionKind::Dividend);
  CHECK_NEAR(acts[1].value, 0.14, 1e-12);
  CHECK(acts[1].currency == w.eur.id);
  w.store->appendActions(k, {{5000, ActionKind::Dividend, 0.15, w.eur.id, "revised"}});
  CHECK(w.snap()->actions(k).size() == 2 && w.snap()->actions(k)[1].note == "revised");
}

TEST(events_lists_the_datasets_that_say_something_about_an_instrument) {
  World w;
  CHECK(w.snap()->events(w.nokia.id).empty());
  w.store->appendAnnouncements({w.nokia.id, w.a.id}, {{1, "x", "x", "", ""}});
  w.store->appendActions({w.nokia.id, w.a.id}, {{1, ActionKind::Split, 2, {}, ""}});
  w.store->appendActions({w.nokia.id, w.b.id}, {{1, ActionKind::Split, 2, {}, ""}});
  CHECK(w.snap()->events(w.nokia.id).size() == 2);
  CHECK(w.snap()->events(w.eur.id).empty());
}

TEST(a_snapshot_never_changes_and_a_write_makes_a_new_one_that_shares_the_old) {
  World w;
  EdgeKey k = w.key(w.hel.id, w.a.id);
  w.store->appendCandles(k, 1000, {bar(0, 1), bar(1000, 2)});
  auto before = w.snap();
  uint64_t v = before->version();
  w.store->appendCandles(k, 1000, {bar(2000, 3), bar(0, 10)});
  w.store->appendBooks(k, {{0, {{1, 1}}, {{2, 1}}}});
  auto after = w.snap();
  CHECK(before->candles(k, 1000).candles.size() == 2 && before->candles(k, 1000).candles[0].close == 1);
  CHECK(before->books(k).empty() && before->version() == v);
  CHECK(after->candles(k, 1000).candles.size() == 3 && after->candles(k, 1000).candles[0].close == 10);
  CHECK(after->version() > v);
}

TEST(only_a_write_that_changes_something_makes_a_new_version) {
  World w;
  uint64_t v = w.snap()->version();
  w.store->addInstrument({{}, "NOKIA", "again", InstrumentKind::Company});
  w.store->appendCandles(w.key(w.hel.id, w.a.id), 1000, {});
  w.store->appendCandles(w.key(w.hel.id, w.a.id), 0, {bar(0, 1)});
  w.store->appendBooks(w.key(w.hel.id, w.a.id), {});
  CHECK(w.snap()->version() == v);
  w.store->appendCandles(w.key(w.hel.id, w.a.id), 1000, {bar(0, 1)});
  CHECK(w.snap()->version() == v + 1);
}

TEST(readers_see_whole_consistent_snapshots_while_a_writer_appends) {
  World w;
  EdgeKey k = w.key(w.hel.id, w.a.id);
  const int kBars = 3000;
  std::atomic<bool> done{false};
  std::atomic<int> bad{0};
  auto reader = [&] {
    uint64_t last = 0;
    while (!done) {
      auto s = w.snap();
      CandleSeries c = s->candles(k, 1000);
      // Each bar's close is its index, so a torn or reordered series shows up.
      for (size_t i = 0; i < c.candles.size(); i++)
        if (c.candles[i].time != static_cast<Timestamp>(i) * 1000 || c.candles[i].close != static_cast<double>(i)) bad++;
      if (s->version() < last) bad++;
      last = s->version();
    }
  };
  std::thread r1(reader), r2(reader);
  for (int i = 0; i < kBars; i++) w.store->appendCandles(k, 1000, {bar(static_cast<Timestamp>(i) * 1000, i)});
  done = true;
  r1.join();
  r2.join();
  CHECK(bad == 0);
  CHECK(w.snap()->candles(k, 1000).candles.size() == static_cast<size_t>(kBars));
}

TEST(two_writers_do_not_lose_each_others_bars) {
  World w;
  EdgeKey ka = w.key(w.hel.id, w.a.id), kb = w.key(w.hel.id, w.b.id);
  auto write = [&](EdgeKey k) {
    for (int i = 0; i < 500; i++) w.store->appendCandles(k, 1000, {bar(static_cast<Timestamp>(i) * 1000, i)});
  };
  std::thread t1(write, ka), t2(write, kb);
  t1.join();
  t2.join();
  CHECK(w.snap()->candles(ka, 1000).candles.size() == 500 && w.snap()->candles(kb, 1000).candles.size() == 500);
}

TEST(an_instrument_with_an_isin_is_that_isin_whatever_its_symbol_or_market) {
  World w;
  Instrument a = w.store->addInstrument({{}, "XAIX", "Xtrackers AI", InstrumentKind::Etf, "IE00BGV5VN51"});
  Instrument again = w.store->addInstrument({{}, "XAIX-OTHER", "Other name", InstrumentKind::Etf, "IE00BGV5VN51"});
  CHECK(a.id.valid() && again.id == a.id && again.symbol == "XAIX");  // the second listing finds the first
  CHECK(w.snap()->findInstrumentByIsin("IE00BGV5VN51")->id == a.id);
  CHECK(w.snap()->findInstrument("XAIX")->isin == "IE00BGV5VN51");
  CHECK(!w.snap()->findInstrumentByIsin("IE0000000000"));

  // The same symbol with another ISIN is another instrument; the symbol finds the first.
  Instrument b = w.store->addInstrument({{}, "XAIX", "A different fund", InstrumentKind::Etf, "LU0000000001"});
  CHECK(b.id.valid() && b.id != a.id);
  CHECK(w.snap()->findInstrument("XAIX")->id == a.id && w.snap()->findInstrumentByIsin("LU0000000001")->id == b.id);

  // Without an ISIN it is the symbol, as before: even a symbol that an instrument with an ISIN has.
  CHECK(w.store->addInstrument({{}, "XAIX", "no isin", InstrumentKind::Other}).id == a.id);
}

TEST(edges_can_be_selected_by_the_type_of_their_base_instrument) {
  World w;
  Instrument etf = w.store->addInstrument({{}, "FLXK", "A fund", InstrumentKind::Etf, "IE00BHZRR030", "UETF"});
  Instrument cert = w.store->addInstrument({{}, "4GLD", "A certificate", InstrumentKind::Other, "DE000A0S9GB0", "PTC"});
  EdgeKey a{etf.id, w.eur.id, w.hel.id, w.a.id}, b{cert.id, w.eur.id, w.hel.id, w.a.id}, c{w.nokia.id, w.eur.id, w.hel.id, w.a.id};
  for (const EdgeKey & k : {a, b, c}) w.store->appendCandles(k, 1000, {bar(0, 1)});
  CHECK(w.snap()->findInstrument("FLXK")->type == "UETF" && w.snap()->findInstrument("NOKIA")->type.empty());
  CHECK(w.snap()->edges(w.hel.id).size() == 3);
  auto isType = [](const char * t) { return [t](const Instrument & i) { return i.type == t; }; };
  CHECK(w.snap()->edges(w.hel.id, isType("PTC")).size() == 1 && w.snap()->edges(w.hel.id, isType("PTC"))[0] == b);
  CHECK(w.snap()->edges(w.hel.id, isType("UETF")).size() == 1);
  CHECK(w.snap()->edges(w.hel.id, isType("nothing")).empty());
  CHECK(w.snap()->edges(w.hel.id, [](const Instrument & i) { return i.kind == InstrumentKind::Etf; }).size() == 1);
  CHECK(w.snap()->edges(w.sto.id, isType("PTC")).empty());
}

TEST(an_instrument_with_no_edge_is_not_seen_by_a_market) {
  World w;
  w.store->addInstrument({{}, "LONELY", "No data", InstrumentKind::Etf, "XX0000000000", "UETF"});
  CHECK(w.snap()->findInstrument("LONELY"));  // it exists, as a node
  for (const Market & m : w.snap()->markets()) CHECK(w.snap()->edges(m.id).empty());
}
