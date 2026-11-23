#include "TestFramework.h"

#include "model/Dates.h"
#include "model/Store.h"

#include <atomic>
#include <thread>

namespace {

struct World {
  std::unique_ptr<Store> store = makeMemoryStore();
  Instrument nokia = store->addInstrument({{}, "NOKIA", "Nokia", InstrumentKind::Company});
  Instrument eur = store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  MarketId hel = store->addMarket("Nasdaq Helsinki");
  MarketId sto = store->addMarket("Nasdaq Stockholm");
  DatasetId a = store->addDataset("providera");
  DatasetId b = store->addDataset("providerb");
  std::shared_ptr<const Snapshot> snap() const { return store->snapshot(); }
  EdgeKey key(MarketId m, DatasetId d) const { return {nokia.id, eur.id, m, d}; }
};

Candle bar(Timestamp t, double close) { return {t, close, close, close, close, 1}; }

}  // namespace

TEST(adding_the_same_symbol_or_name_returns_the_record_already_there) {
  World w;
  Instrument again = w.store->addInstrument({{}, "NOKIA", "Other name", InstrumentKind::Index});
  CHECK(again.id == w.nokia.id && again.name == "Other name" && again.kind == InstrumentKind::Index);  // the last said stands
  CHECK(w.store->addMarket("Nasdaq Helsinki") == w.hel);
  CHECK(w.store->addDataset("providera") == w.a);
  CHECK(w.snap()->nodeCount(NodeType::Instrument) == 2 && w.snap()->nodeCount(NodeType::Market) == 2 && w.snap()->nodeCount(NodeType::Dataset) == 2);
}

TEST(records_are_found_by_id_and_by_name) {
  World w;
  CHECK(w.snap()->instrument(w.eur.id)->symbol == "EUR");
  CHECK(w.snap()->findInstrument("NOKIA")->id == w.nokia.id);
  CHECK(!w.snap()->findInstrument("NOPE"));
  CHECK(!w.snap()->instrument({}) && !w.snap()->instrument({99}));
  CHECK(*w.snap()->findMarket("Nasdaq Stockholm") == w.sto && w.snap()->name(w.hel) == "Nasdaq Helsinki");
  CHECK(*w.snap()->findDataset("providerb") == w.b && !w.snap()->findDataset("provider-b") && w.snap()->name(NodeRef(DatasetId{})).empty());
}

TEST(candles_stay_time_ordered_and_a_bar_at_a_known_time_replaces_it) {
  World w;
  EdgeKey k = w.key(w.hel, w.a);
  w.store->appendCandles(k, Resolution::Min5, {bar(120000, 3), bar(0, 1)});
  w.store->appendCandles(k, Resolution::Min5, {bar(60000, 2), bar(120000, 30)});
  CandleSeries s = w.snap()->candles(k, Resolution::Min5);
  CHECK(s.resolution == Resolution::Min5 && s.candles.size() == 3);
  CHECK(s.candles[0].time == 0 && s.candles[1].time == 60000 && s.candles[2].time == 120000);
  CHECK_NEAR(s.candles[2].close, 30, 1e-9);
}

TEST(candle_ranges_are_half_open_and_last_candles_takes_the_tail) {
  World w;
  EdgeKey k = w.key(w.hel, w.a);
  w.store->appendCandles(k, Resolution::Min1, {bar(0, 1), bar(1000, 2), bar(2000, 3), bar(3000, 4)});
  CandleSeries r = w.snap()->candles(k, Resolution::Min1, {1000, 3000});
  CHECK(r.candles.size() == 2 && r.candles[0].time == 1000 && r.candles[1].time == 2000);
  CandleSeries t = w.snap()->lastCandles(k, Resolution::Min1, 2);
  CHECK(t.candles.size() == 2 && t.candles[0].time == 2000 && t.candles[1].time == 3000);
  CHECK(w.snap()->lastCandles(k, Resolution::Min1, 99).candles.size() == 4);
  CHECK(w.snap()->candles(w.key(w.hel, w.b), Resolution::Min1).candles.empty());
  CHECK(w.snap()->lastCandles(w.key(w.hel, w.b), Resolution::Min1, 3).candles.empty());
}

TEST(an_edge_has_a_series_for_each_resolution_it_has_data_in) {
  World w;
  EdgeKey k = w.key(w.hel, w.a);
  w.store->appendCandles(k, Resolution::Day, {});  // nothing to add makes nothing
  CHECK(w.snap()->resolutions(k).empty() && w.snap()->edges(w.hel).empty());

  const int64_t today = dayNumber(2026, 11, 20);
  w.store->appendCandles(k, Resolution::Day, {bar(today - 1, 10), bar(today, 11)});  // keyed by date
  const int64_t hour = slotOfInstant(Resolution::Hour1, 1795000000);  // hourly bars are keyed by their hour number
  w.store->appendCandles(k, Resolution::Hour1, {bar(hour, 1), bar(hour + 1, 2), bar(hour + 2, 3)});
  w.store->appendCandles(k, Resolution::Min5, {bar(slotOfInstant(Resolution::Min5, 1795000000), 5)});
  auto s = w.snap();
  CHECK((s->resolutions(k) == std::vector<Resolution>{Resolution::Min5, Resolution::Hour1, Resolution::Day}));  // shortest first
  CHECK(s->candles(k, Resolution::Day).candles.size() == 2 && s->candles(k, Resolution::Day).resolution == Resolution::Day);
  CHECK(s->candles(k, Resolution::Hour1).candles.size() == 3 && s->candles(k, Resolution::Min5).candles.size() == 1);
  CHECK(s->candles(k, Resolution::Week).candles.empty());  // a resolution it has no data in
  CHECK(s->lastCandles(k, Resolution::Hour1, 1).candles[0].close == 3);
  CHECK(s->edges(w.hel).size() == 1);  // one edge, whatever the number of series

  // A correction in one series leaves the others alone.
  w.store->appendCandles(k, Resolution::Hour1, {bar(hour + 1, 20)});
  s = w.snap();
  CHECK(s->candles(k, Resolution::Hour1).candles.size() == 3 && s->candles(k, Resolution::Hour1).candles[1].close == 20);
  CHECK(s->candles(k, Resolution::Day).candles[1].close == 11 && s->candles(k, Resolution::Min5).candles[0].close == 5);
}

TEST(resolutions_are_codes_with_a_name_and_the_intraday_ones_a_length) {
  CHECK(seconds(Resolution::Min1) == 60 && seconds(Resolution::Min5) == 300 && seconds(Resolution::Min15) == 900);
  CHECK(seconds(Resolution::Min30) == 1800 && seconds(Resolution::Hour1) == 3600 && seconds(Resolution::Hour4) == 14400);
  for (Resolution r : {Resolution::Day, Resolution::Week, Resolution::Month, Resolution::Quarter}) {
    CHECK(isDateResolution(r) && seconds(r) == 0);  // no fixed length: daylight saving, calendars
  }
  for (Resolution r : {Resolution::Min1, Resolution::Min2, Resolution::Min5, Resolution::Hour1, Resolution::Hour4}) CHECK(!isDateResolution(r));
  CHECK(std::string(name(Resolution::Min5)) == "5m" && std::string(name(Resolution::Hour1)) == "1h");
  CHECK(std::string(name(Resolution::Day)) == "1d" && std::string(name(Resolution::Week)) == "1w");
  CHECK(std::string(name(Resolution::Month)) == "1M" && std::string(name(Resolution::Quarter)) == "1Q");
  CHECK(Resolution::Min1 < Resolution::Hour1 && Resolution::Hour1 < Resolution::Day && Resolution::Day < Resolution::Quarter);
}

TEST(sources_that_disagree_about_one_pair_on_one_market_both_stay) {
  World w;
  w.store->appendCandles(w.key(w.hel, w.a), Resolution::Min1, {bar(0, 5.0)});
  w.store->appendCandles(w.key(w.hel, w.b), Resolution::Min1, {bar(0, 5.1)});
  CHECK_NEAR(w.snap()->candles(w.key(w.hel, w.a), Resolution::Min1).candles[0].close, 5.0, 1e-9);
  CHECK_NEAR(w.snap()->candles(w.key(w.hel, w.b), Resolution::Min1).candles[0].close, 5.1, 1e-9);
  CHECK(w.snap()->edges(w.hel).size() == 2);
}

TEST(edges_lists_the_keys_with_data_in_a_market) {
  World w;
  CHECK(w.snap()->edges(w.hel).empty());
  w.store->appendCandles(w.key(w.hel, w.a), Resolution::Min1, {bar(0, 1)});
  w.store->appendCandles(w.key(w.sto, w.a), Resolution::Min1, {bar(0, 1)});
  w.store->appendCandles(w.key(w.hel, w.a), Resolution::Min1, {});  // nothing to add creates nothing
  w.store->appendCandles(w.key(w.sto, w.b), Resolution::Min1, {});
  w.store->appendBooks(w.key(w.hel, w.b), {{0, {{1, 1}}, {{2, 1}}}});  // a book alone makes an edge too
  std::vector<EdgeKey> hel = w.snap()->edges(w.hel);
  CHECK(hel.size() == 2);
  CHECK(w.snap()->edges(w.sto).size() == 1);
  CHECK(hel[0].market == w.hel && hel[1].market == w.hel);
}

TEST(a_book_at_a_time_is_the_latest_snapshot_at_or_before_it) {
  World w;
  EdgeKey k = w.key(w.hel, w.a);
  BookSnapshot s1{1000, {{9.9, 100}, {9.8, 200}}, {{10.1, 150}}};
  BookSnapshot s2{2000, {{10.0, 50}}, {{10.2, 60}, {10.3, 70}}};
  w.store->appendBooks(k, {s2, s1});
  CHECK(!w.snap()->bookAt(k, 999));
  CHECK(w.snap()->bookAt(k, 1000)->bids.size() == 2);
  CHECK(w.snap()->bookAt(k, 1999)->time == 1000);
  CHECK(w.snap()->bookAt(k, 5000)->asks.size() == 2);
  CHECK(!w.snap()->bookAt(w.key(w.hel, w.b), 5000));
  CHECK(w.snap()->books(k, {1500, 3000}).size() == 1);
  w.store->appendBooks(k, {{2000, {}, {}}});  // same time: replaced
  CHECK(w.snap()->books(k).size() == 2 && w.snap()->bookAt(k, 2000)->asks.empty());
}

TEST(order_book_snapshots_are_keyed_by_floating_point_seconds) {
  World w;
  EdgeKey k = w.key(w.hel, w.a);
  w.store->appendBooks(k, {{1000.75, {{1, 1}}, {{2, 1}}}, {1000.25, {{1, 2}}, {{2, 2}}}, {1000.5, {{1, 3}}, {{2, 3}}}});
  auto s = w.snap();
  auto all = s->books(k);
  CHECK(all.size() == 3 && all[0].time == 1000.25 && all[1].time == 1000.5 && all[2].time == 1000.75);  // ordered by the fraction
  CHECK(s->bookAt(k, 1000.6)->time == 1000.5 && s->bookAt(k, 1000.75)->time == 1000.75);
  CHECK(!s->bookAt(k, 1000.2) && s->bookAt(k, 5000)->time == 1000.75);
  w.store->appendBooks(k, {{1000.5, {{9, 9}}, {{10, 9}}}});  // the same instant replaces
  CHECK(w.snap()->books(k).size() == 3 && w.snap()->bookAt(k, 1000.5)->bids[0].size == 9);
}

TEST(announcements_are_kept_per_instrument_and_dataset_in_time_order) {
  World w;
  EventKey ka{w.nokia.id, w.a}, kb{w.nokia.id, w.b};
  w.store->appendAnnouncements(ka, {{2000, "results", "Q2 results", "Q2", ""}, {1000, "guidance", "Raises guidance", "", ""}});
  w.store->appendAnnouncements(ka, {{2000, "results", "Q2 results", "Q2 (corrected)", ""}});  // same time and headline
  w.store->appendAnnouncements(ka, {{2000, "mgmt", "New CFO", "", ""}});                      // same time, other headline
  w.store->appendAnnouncements(kb, {{1500, "results", "Other source", "", ""}});
  auto a = w.snap()->announcements(ka);
  CHECK(a.size() == 3 && a[0].headline == "Raises guidance" && a[1].body == "Q2 (corrected)" && a[2].headline == "New CFO");
  CHECK(w.snap()->announcements(ka, {1500, 2500}).size() == 2);
  CHECK(w.snap()->announcements(kb).size() == 1);
  CHECK(w.snap()->announcements({w.eur.id, w.a}).empty());
}

TEST(corporate_actions_carry_their_kind_and_value) {
  World w;
  EventKey k{w.nokia.id, w.a};
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
  w.store->appendAnnouncements({w.nokia.id, w.a}, {{1, "x", "x", "", ""}});
  w.store->appendActions({w.nokia.id, w.a}, {{1, ActionKind::Split, 2, {}, ""}});
  w.store->appendActions({w.nokia.id, w.b}, {{1, ActionKind::Split, 2, {}, ""}});
  CHECK(w.snap()->events(w.nokia.id).size() == 2);
  CHECK(w.snap()->events(w.eur.id).empty());
}

TEST(a_snapshot_never_changes_and_a_write_makes_a_new_one_that_shares_the_old) {
  World w;
  EdgeKey k = w.key(w.hel, w.a);
  w.store->appendCandles(k, Resolution::Min1, {bar(0, 1), bar(1000, 2)});
  auto before = w.snap();
  uint64_t v = before->version();
  w.store->appendCandles(k, Resolution::Min1, {bar(2000, 3), bar(0, 10)});
  w.store->appendBooks(k, {{0, {{1, 1}}, {{2, 1}}}});
  auto after = w.snap();
  CHECK(before->candles(k, Resolution::Min1).candles.size() == 2 && before->candles(k, Resolution::Min1).candles[0].close == 1);
  CHECK(before->books(k).empty() && before->version() == v);
  CHECK(after->candles(k, Resolution::Min1).candles.size() == 3 && after->candles(k, Resolution::Min1).candles[0].close == 10);
  CHECK(after->version() > v);
}

TEST(only_a_write_that_changes_something_makes_a_new_version) {
  World w;
  uint64_t v = w.snap()->version();
  w.store->addInstrument({{}, "NOKIA", "Nokia", InstrumentKind::Company});
  w.store->appendCandles(w.key(w.hel, w.a), Resolution::Min1, {});
  w.store->appendBooks(w.key(w.hel, w.a), {});
  CHECK(w.snap()->version() == v);
  w.store->appendCandles(w.key(w.hel, w.a), Resolution::Min1, {bar(0, 1)});
  CHECK(w.snap()->version() == v + 1);
}

TEST(readers_see_whole_consistent_snapshots_while_a_writer_appends) {
  World w;
  EdgeKey k = w.key(w.hel, w.a);
  const int kBars = 3000;
  std::atomic<bool> done{false};
  std::atomic<int> bad{0};
  auto reader = [&] {
    uint64_t last = 0;
    while (!done) {
      auto s = w.snap();
      CandleSeries c = s->candles(k, Resolution::Min1);
      // Each bar's close is its index, so a torn or reordered series shows up.
      for (size_t i = 0; i < c.candles.size(); i++)
        if (c.candles[i].time != static_cast<Timestamp>(i) * 1000 || c.candles[i].close != static_cast<double>(i)) bad++;
      if (s->version() < last) bad++;
      last = s->version();
    }
  };
  std::thread r1(reader), r2(reader);
  for (int i = 0; i < kBars; i++) w.store->appendCandles(k, Resolution::Min1, {bar(static_cast<Timestamp>(i) * 1000, i)});
  done = true;
  r1.join();
  r2.join();
  CHECK(bad == 0);
  CHECK(w.snap()->candles(k, Resolution::Min1).candles.size() == static_cast<size_t>(kBars));
}

TEST(two_writers_do_not_lose_each_others_bars) {
  World w;
  EdgeKey ka = w.key(w.hel, w.a), kb = w.key(w.hel, w.b);
  auto write = [&](EdgeKey k) {
    for (int i = 0; i < 500; i++) w.store->appendCandles(k, Resolution::Min1, {bar(static_cast<Timestamp>(i) * 1000, i)});
  };
  std::thread t1(write, ka), t2(write, kb);
  t1.join();
  t2.join();
  CHECK(w.snap()->candles(ka, Resolution::Min1).candles.size() == 500 && w.snap()->candles(kb, Resolution::Min1).candles.size() == 500);
}

TEST(an_instrument_is_its_isin_whatever_its_symbol_or_market) {
  World w;
  Instrument a = w.store->addInstrument({{}, "", "Xtrackers AI", InstrumentKind::Etf, "IE00BGV5VN51"});
  Instrument again = w.store->addInstrument({{}, "", "Other name", InstrumentKind::Etf, "IE00BGV5VN51"});  // another listing
  CHECK(a.id.valid() && again.id == a.id);
  CHECK(w.snap()->findInstrumentByIsin("IE00BGV5VN51")->id == a.id);
  CHECK(!w.snap()->findInstrumentByIsin("IE0000000004"));
  Instrument b = w.store->addInstrument({{}, "", "A different fund", InstrumentKind::Etf, "LU0000000009"});
  CHECK(b.id.valid() && b.id != a.id);
  // A currency has no ISIN, and is its code.
  CHECK(w.store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency}).id == w.eur.id);
}

TEST(a_fat_edge_has_a_typed_listing_and_only_once_it_has_data) {
  World w;
  Instrument fund = w.store->addInstrument({{}, "", "Xtrackers AI", InstrumentKind::Etf, "IE00BGV5VN51"});
  const EdgeKey onHel{fund.id, w.eur.id, w.hel, w.a}, onSto{fund.id, w.eur.id, w.sto, w.a};

  // An edge exists once it has data: before that it takes nothing.
  w.store->setListing({onHel, "XAIX", "XAIX", "book-h", "UETF"});
  CHECK(!w.snap()->listing(onHel) && !w.snap()->findEdge("XAIX", w.hel));

  w.store->appendCandles(onHel, Resolution::Day, {bar(1, 10)});
  w.store->appendCandles(onSto, Resolution::Day, {bar(1, 11)});
  w.store->setListing({onHel, "XAIX", "XAIX", "book-h", "UETF"});
  w.store->setListing({onSto, "XAIX-S", "419021", "book-s", "UETF"});

  // One instrument, a listing for each market's edge, found by the market.
  CHECK(w.snap()->symbol(onHel) == "XAIX" && w.snap()->symbol(onSto) == "XAIX-S");
  CHECK(w.snap()->listing(onSto)->identifier == "419021" && w.snap()->listing(onSto)->orderBook == "book-s");
  CHECK(*w.snap()->findEdge("XAIX", w.hel) == onHel);
  CHECK(!w.snap()->findEdge("XAIX", w.sto));  // that market calls it something else
  CHECK(*w.snap()->findEdge("XAIX-S", w.sto) == onSto);
  CHECK(w.snap()->findEdge("XAIX", w.hel)->base == fund.id);

  // A symbol changing replaces the old, and the old finds nothing; an unchanged write makes no new version.
  w.store->setListing({onHel, "XAIX2", "XAIX", "book-h", "UETF"});
  CHECK(!w.snap()->findEdge("XAIX", w.hel) && *w.snap()->findEdge("XAIX2", w.hel) == onHel);
  const uint64_t version = w.snap()->version();
  w.store->setListing({onHel, "XAIX2", "XAIX", "book-h", "UETF"});
  CHECK(w.snap()->version() == version);

  // The same data from another dataset is another edge, with its own listing.
  const EdgeKey other{fund.id, w.eur.id, w.hel, w.b};
  w.store->appendCandles(other, Resolution::Day, {bar(1, 10)});
  w.store->setListing({other, "XAIX.HE", "", "", "ETF"});
  CHECK(w.snap()->symbol(other) == "XAIX.HE" && *w.snap()->findEdge("XAIX2", w.hel) == onHel);
}

TEST(a_thin_edge_is_a_triple_of_a_node_with_no_dataset_and_a_value_that_changes_replaces_it) {
  World w;
  const NodeRef nokia = w.nokia.id;
  CHECK(!w.snap()->attribute(nokia, predicate::iconUrl));
  const size_t own = w.snap()->attributes(nokia).size();  // the instrument's own: symbol, name, kind

  // (subject, predicate, object): text, a whole number, a real number, a truth value, a day. Icons, shares and the like
  // are not part of a market, so no dataset says them.
  w.store->setAttributes(nokia, {{predicate::iconUrl, "https://x/nokia.png"},
                                 {"owners", int64_t(1234)},
                                 {"pe", 12.5},
                                 {"tradable", false},
                                 {"dividend-ex-day", int64_t(dayNumber(2026, 12, 1))}});
  auto s = w.snap();
  CHECK(asText(*s->attribute(nokia, predicate::iconUrl)) == "https://x/nokia.png");
  CHECK(asInteger(*s->attribute(nokia, "owners")) == 1234 && asReal(*s->attribute(nokia, "owners")) == 1234.0);
  CHECK(asReal(*s->attribute(nokia, "pe")) == 12.5 && !asInteger(*s->attribute(nokia, "pe")));
  CHECK(asBoolean(*s->attribute(nokia, "tradable")) == false);  // false is said too
  CHECK(asInteger(*s->attribute(nokia, "dividend-ex-day")) == dayNumber(2026, 12, 1));
  CHECK(!s->attribute(nokia, "eps"));  // what is not said is not there
  CHECK(s->attributes(nokia).size() == own + 5);

  // The same node says the same things as before and more: the name, found by its text, beside the icon.
  CHECK(w.snap()->findInstrument("NOKIA")->id == w.nokia.id);

  // A value that changes replaces the one there; one that does not makes no new version.
  const uint64_t version = s->version();
  w.store->setAttributes(nokia, {{"pe", 12.5}, {"owners", int64_t(1234)}});
  CHECK(w.snap()->version() == version);
  w.store->setAttributes(nokia, {{"pe", 13.0}});
  CHECK(w.snap()->version() == version + 1 && asReal(*w.snap()->attribute(nokia, "pe")) == 13.0);
  CHECK(w.snap()->attributes(nokia).size() == s->attributes(nokia).size());

  // Empty, undefined and nothing say nothing; so does a key that is no node's (none, or a number that is no ISIN).
  w.store->setAttributes(nokia, {{"x", std::string()}, {"y", std::numeric_limits<double>::quiet_NaN()}, {"z", Value{}}});
  w.store->setAttribute(NodeRef(InstrumentId{}), {"pe", 1.0});
  w.store->setAttribute(NodeRef(InstrumentId{(uint64_t{1} << 60) - 1}), {"pe", 1.0});
  CHECK(w.snap()->attributes(nokia).size() == s->attributes(nokia).size() && w.snap()->version() == version + 1);

  // Text is found by its value; other values are not.
  CHECK(w.snap()->findByAttribute(NodeType::Instrument, predicate::iconUrl, "https://x/nokia.png")->id == w.nokia.id.value);
  CHECK(!w.snap()->findByAttribute(NodeType::Instrument, "owners", "1234"));
}

TEST(edges_can_be_selected_by_the_type_of_their_base_instrument) {
  World w;
  Instrument etf = w.store->addInstrument({{}, "FLXK", "A fund", InstrumentKind::Etf, "IE00BHZRR030", "UETF"});
  Instrument cert = w.store->addInstrument({{}, "4GLD", "A certificate", InstrumentKind::Other, "DE000A0S9GB0", "PTC"});
  EdgeKey a{etf.id, w.eur.id, w.hel, w.a}, b{cert.id, w.eur.id, w.hel, w.a}, c{w.nokia.id, w.eur.id, w.hel, w.a};
  for (const EdgeKey & k : {a, b, c}) w.store->appendCandles(k, Resolution::Min1, {bar(0, 1)});
  CHECK(w.snap()->findInstrument("FLXK")->type == "UETF" && w.snap()->findInstrument("NOKIA")->type.empty());
  CHECK(w.snap()->edges(w.hel).size() == 3);
  auto isType = [](const char * t) { return [t](const Instrument & i) { return i.type == t; }; };
  CHECK(w.snap()->edges(w.hel, isType("PTC")).size() == 1 && w.snap()->edges(w.hel, isType("PTC"))[0] == b);
  CHECK(w.snap()->edges(w.hel, isType("UETF")).size() == 1);
  CHECK(w.snap()->edges(w.hel, isType("nothing")).empty());
  CHECK(w.snap()->edges(w.hel, [](const Instrument & i) { return i.kind == InstrumentKind::Etf; }).size() == 1);
  CHECK(w.snap()->edges(w.sto, isType("PTC")).empty());
}

TEST(an_instrument_with_no_edge_is_not_seen_by_a_market) {
  World w;
  w.store->addInstrument({{}, "LONELY", "No data", InstrumentKind::Etf, "XX0000000002", "UETF"});
  CHECK(w.snap()->findInstrument("LONELY"));  // it exists, as a node
  CHECK(w.snap()->edges(w.hel).empty() && w.snap()->edges(w.sto).empty() && w.snap()->edgeMarkets().empty());
}
