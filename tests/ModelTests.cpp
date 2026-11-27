#include "TestFramework.h"

#include "model/Dates.h"
#include "model/Isin.h"
#include "model/Period.h"
#include "model/Store.h"
#include "portfolio/Portfolio.h"

#include <atomic>
#include <thread>

namespace {

struct World {
  std::unique_ptr<Store> store = makeMemoryStore();
  InstrumentId nokia = store->addInstrument("NOKIA", "Nokia", InstrumentKind::Company);
  InstrumentId eur = store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  MarketId hel = store->addMarket("XHEL", "Nasdaq Helsinki");
  MarketId sto = store->addMarket("XSTO", "Nasdaq Stockholm");
  DatasetId a = store->addDataset("providera");
  DatasetId b = store->addDataset("providerb");
  std::shared_ptr<const Snapshot> snap() const { return store->snapshot(); }
  EdgeKey key(MarketId m, DatasetId d) const { return {nokia, eur, m, d}; }
};

Candle bar(Timestamp t, double close) { return {t, close, close, close, close, 1}; }

}  // namespace

TEST(adding_the_same_symbol_or_name_returns_the_record_already_there) {
  World w;
  InstrumentId again = w.store->addInstrument("NOKIA", "Other name", InstrumentKind::Index);
  CHECK(again == w.nokia && w.snap()->name(again) == "Other name" && w.snap()->kind(again) == InstrumentKind::Index);  // the last said stands
  CHECK(w.store->addMarket("XHEL", "Nasdaq Helsinki") == w.hel);
  CHECK(w.store->addDataset("providera") == w.a);
  CHECK(w.snap()->nodeCount(NodeType::Instrument) == 2 && w.snap()->nodeCount(NodeType::Market) == 2 && w.snap()->nodeCount(NodeType::Dataset) == 2);
}

TEST(records_are_found_by_id_and_by_name) {
  World w;
  CHECK(w.snap()->symbol(w.eur) == "EUR");
  CHECK(*w.snap()->findInstrument("NOKIA") == w.nokia);
  CHECK(!w.snap()->findInstrument("NOPE"));
  CHECK(!w.snap()->known(NodeRef(InstrumentId{})) && !w.snap()->known(NodeRef(InstrumentId{99})));
  CHECK(*w.snap()->findMarket("XSTO") == w.sto &&
        w.snap()->name(w.hel) == "Nasdaq Helsinki");
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

namespace {
NewsItem item(const char * id, Timestamp time, const char * headline, std::vector<NodeRef> about = {}, const char * type = "") {
  NewsItem n;
  n.id = id;
  n.time = time;
  n.headline = headline;
  n.about = std::move(about);
  if (*type) n.attributes.push_back({predicate::type, type});
  return n;
}
}  // namespace

TEST(news_is_kept_per_dataset_by_its_id_and_never_replaced) {
  World w;
  w.store->appendNews(w.a, {item("n1", 2000, "Q2 results", {w.nokia}), item("n2", 1000, "Raises guidance", {w.nokia})});
  // Said again: the same item, left as it was (even if the source now words it differently).
  const uint64_t version = w.snap()->version();
  w.store->appendNews(w.a, {item("n1", 2000, "Q2 results (corrected)", {w.nokia})});
  CHECK(w.snap()->version() == version && w.snap()->newsCount() == 2);
  // The same time and headline under another id is another item: the dataset's own duplicates are its to clean up.
  w.store->appendNews(w.a, {item("n3", 2000, "Q2 results", {w.nokia})});
  // Another dataset with the same story has its own item.
  w.store->appendNews(w.b, {item("n1", 2000, "Q2 results", {w.nokia})});
  CHECK(w.snap()->newsCount() == 4);
  auto all = w.snap()->news();
  CHECK(all.size() == 4 && all[3].headline == "Raises guidance");  // newest first: the last is the oldest
  CHECK(all[0].time == 2000 && all[1].time == 2000 && all[2].time == 2000);
  CHECK(all[0].dataset == w.a && all[0].id == "n1" && all[1].dataset == w.a && all[1].id == "n3" && all[2].dataset == w.b);  // ties: dataset, id
  CHECK(w.snap()->news({{1500, 2500}}).size() == 3);
}

TEST(news_is_found_by_whom_it_is_about_by_what_it_says_and_the_newest_come_first) {
  World w;
  const NodeRef finland{CountryId{countryKey("FI")->value}};
  const NodeRef sweden{CountryId{countryKey("SE")->value}};
  w.store->appendNews(w.a, {item("1", 100, "Nokia news", {w.nokia, finland}, "press release"),
                            item("2", 200, "Finland's economy", {finland}, "report"),
                            item("3", 300, "Sweden's rates", {sweden, w.hel}, "report"),
                            item("4", 400, "The world", {}, "report")});  // about nothing in particular
  auto snap = w.snap();
  CHECK(snap->news().size() == 4 && snap->news()[0].headline == "The world");
  NewsQuery q;
  q.about = finland;
  CHECK(snap->news(q).size() == 2 && snap->news(q)[0].headline == "Finland's economy");
  q.about = NodeRef(w.nokia);
  CHECK(snap->news(q).size() == 1);
  q.about = NodeRef(w.hel);
  CHECK(snap->news(q).size() == 1 && snap->news(q)[0].headline == "Sweden's rates");
  q = {};
  q.keep = [](const NewsItem & n) { return newsText(n, predicate::type) == "report"; };  // a preset: by an attribute
  CHECK(snap->news(q).size() == 3);
  q = {};
  q.limit = 2;
  CHECK(snap->news(q).size() == 2 && snap->news(q)[1].headline == "Sweden's rates");
  CHECK(snap->news({{150, 350}}).size() == 2);
  CHECK(w.snap()->news({{1000, 2000}}).empty() && sweden.type == NodeType::Country && nodeOf(*countryKey("SE")) == sweden);
}

TEST(an_item_of_news_with_no_id_or_no_dataset_is_refused_and_nothing_of_the_call_is_written) {
  World w;
  const uint64_t version = w.snap()->version();
  bool refusedId = false, refusedDataset = false;
  try {
    w.store->appendNews(w.a, {item("ok", 1, "fine"), item("", 2, "no id")});
  } catch (const std::invalid_argument &) {
    refusedId = true;
  }
  try {
    w.store->appendNews(DatasetId{}, {item("x", 1, "no dataset")});
  } catch (const std::invalid_argument &) {
    refusedDataset = true;
  }
  CHECK(refusedId && refusedDataset && w.snap()->version() == version && w.snap()->newsCount() == 0);
}

TEST(corporate_actions_carry_their_kind_and_value) {
  World w;
  EventKey k{w.nokia, w.a};
  CorporateAction div{5000, ActionKind::Dividend, 0.14, w.eur, ""};
  CorporateAction split{3000, ActionKind::Split, 2.0, {}, "2:1"};
  w.store->appendActions(k, {div, split});
  auto acts = w.snap()->actions(k);
  CHECK(acts.size() == 2 && acts[0].kind == ActionKind::Split && acts[1].kind == ActionKind::Dividend);
  CHECK_NEAR(acts[1].value, 0.14, 1e-12);
  CHECK(acts[1].currency == w.eur);
  w.store->appendActions(k, {{5000, ActionKind::Dividend, 0.15, w.eur, "revised"}});
  CHECK(w.snap()->actions(k).size() == 2 && w.snap()->actions(k)[1].note == "revised");
}

TEST(events_lists_the_datasets_that_say_something_about_an_instrument) {
  World w;
  CHECK(w.snap()->events(w.nokia).empty());
  w.store->appendActions({w.nokia, w.a}, {{1, ActionKind::Split, 2, {}, ""}});
  w.store->appendActions({w.nokia, w.b}, {{1, ActionKind::Split, 2, {}, ""}});
  CHECK(w.snap()->events(w.nokia).size() == 2);
  CHECK(w.snap()->events(w.eur).empty());
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
  w.store->addInstrument("NOKIA", "Nokia", InstrumentKind::Company);
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
  InstrumentId a = w.store->addInstrument("", "Xtrackers AI", InstrumentKind::Etf, "IE00BGV5VN51");
  InstrumentId again = w.store->addInstrument("", "Other name", InstrumentKind::Etf, "IE00BGV5VN51");  // another listing
  CHECK(a.valid() && again == a);
  CHECK(*w.snap()->findInstrumentByIsin("IE00BGV5VN51") == a);
  CHECK(!w.snap()->findInstrumentByIsin("IE0000000004"));
  InstrumentId b = w.store->addInstrument("", "A different fund", InstrumentKind::Etf, "LU0000000009");
  CHECK(b.valid() && b != a);
  // A currency has no ISIN, and is its code.
  CHECK(w.store->addInstrument("EUR", "Euro", InstrumentKind::Currency) == w.eur);
}

TEST(a_fat_edge_has_a_typed_listing_and_only_once_it_has_data) {
  World w;
  InstrumentId fund = w.store->addInstrument("", "Xtrackers AI", InstrumentKind::Etf, "IE00BGV5VN51");
  const EdgeKey onHel{fund, w.eur, w.hel, w.a}, onSto{fund, w.eur, w.sto, w.a};

  // An edge exists once it has data: before that its listing is kept but nothing finds the edge.
  w.store->setListing({onHel, "XAIX", "XAIX", "book-h", "UETF"});
  CHECK(w.snap()->listing(onHel) && !w.snap()->findEdge("XAIX", w.hel) && w.snap()->edges(w.hel).empty());

  w.store->appendCandles(onHel, Resolution::Day, {bar(1, 10)});
  w.store->appendCandles(onSto, Resolution::Day, {bar(1, 11)});
  w.store->setListing({onHel, "XAIX", "XAIX", "book-h", "UETF"});
  w.store->setListing({onSto, "XAIX-S", "419021", "book-s", "UETF"});

  // One instrument, a listing for each market's edge, found by the market.
  CHECK(w.snap()->symbol(onHel) == "XAIX" && w.snap()->symbol(onSto) == "XAIX-S");
  CHECK(w.snap()->listing(onSto)->nordnetIdentifier == "419021" && w.snap()->listing(onSto)->nordnetOrderBookId == "book-s");
  CHECK(*w.snap()->findEdge("XAIX", w.hel) == onHel);
  CHECK(!w.snap()->findEdge("XAIX", w.sto));  // that market calls it something else
  CHECK(*w.snap()->findEdge("XAIX-S", w.sto) == onSto);
  CHECK(w.snap()->findEdge("XAIX", w.hel)->base == fund);

  // A symbol changing replaces the old, and the old finds nothing; an unchanged write makes no new version.
  w.store->setListing({onHel, "XAIX2", "XAIX", "book-h", "UETF"});
  CHECK(!w.snap()->findEdge("XAIX", w.hel) && *w.snap()->findEdge("XAIX2", w.hel) == onHel);
  const uint64_t version = w.snap()->version();
  w.store->setListing({onHel, "XAIX2", "XAIX", "book-h", "UETF"});
  CHECK(w.snap()->version() == version);

  // The same data from another dataset is another edge, with its own listing.
  const EdgeKey other{fund, w.eur, w.hel, w.b};
  w.store->appendCandles(other, Resolution::Day, {bar(1, 10)});
  w.store->setListing({other, "XAIX.HE", "", "", "ETF"});
  CHECK(w.snap()->symbol(other) == "XAIX.HE" && *w.snap()->findEdge("XAIX2", w.hel) == onHel);
}

TEST(a_thin_edge_is_a_triple_of_a_node_with_no_dataset_and_a_value_that_changes_replaces_it) {
  World w;
  const NodeRef nokia = w.nokia;
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
  CHECK(*w.snap()->findInstrument("NOKIA") == w.nokia);

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
  CHECK(w.snap()->findByAttribute(NodeType::Instrument, predicate::iconUrl, "https://x/nokia.png")->id == w.nokia.value);
  CHECK(!w.snap()->findByAttribute(NodeType::Instrument, "owners", "1234"));
}

TEST(a_value_may_be_a_node_and_is_found_by_the_node_it_names) {
  World w;
  const NodeKey eur{w.eur.value}, nokia{w.nokia.value};
  const NodeRef share = w.nokia;
  CHECK(w.snap()->findLinked(NodeType::Instrument, "quoted-in", eur).empty());
  w.store->setAttribute(share, {"quoted-in", eur});
  auto s = w.snap();
  CHECK(asNode(*s->attribute(share, "quoted-in")) == eur && !asText(*s->attribute(share, "quoted-in")));
  CHECK(s->findLinked(NodeType::Instrument, "quoted-in", eur).size() == 1 && s->findLinked(NodeType::Instrument, "quoted-in", eur)[0].id == nokia.value);
  CHECK(s->findLinked(NodeType::Instrument, "other", eur).empty() && s->findLinked(NodeType::Market, "quoted-in", eur).empty());
  CHECK(!s->findByAttribute(NodeType::Instrument, "quoted-in", std::to_string(eur.value)));  // a link is not a text

  // Said again: no new version. Another node replaces it, and the old one no longer finds it.
  const uint64_t version = s->version();
  w.store->setAttribute(share, {"quoted-in", eur});
  CHECK(w.snap()->version() == version);
  const NodeKey sek = *currencyKey("SEK");
  w.store->addInstrument("SEK", "Krona", InstrumentKind::Currency);
  w.store->setAttribute(share, {"quoted-in", sek});
  CHECK(w.snap()->version() == version + 2 && w.snap()->findLinked(NodeType::Instrument, "quoted-in", eur).empty());
  CHECK(w.snap()->findLinked(NodeType::Instrument, "quoted-in", sek).size() == 1);

  // Two nodes can link to one, and a link to no node says nothing.
  w.store->setAttribute(NodeRef(w.eur), {"quoted-in", sek});
  CHECK(w.snap()->findLinked(NodeType::Instrument, "quoted-in", sek).size() == 2);
  const uint64_t after = w.snap()->version();
  w.store->setAttribute(share, {"quoted-in", NodeKey{}});
  CHECK(w.snap()->version() == after && asNode(*w.snap()->attribute(share, "quoted-in")) == sek);
}

TEST(edges_can_be_selected_by_the_type_of_their_base_instrument) {
  World w;
  InstrumentId etf = w.store->addInstrument("FLXK", "A fund", InstrumentKind::Etf, "IE00BHZRR030", "UETF");
  InstrumentId cert = w.store->addInstrument("4GLD", "A certificate", InstrumentKind::Other, "DE000A0S9GB0", "PTC");
  EdgeKey a{etf, w.eur, w.hel, w.a}, b{cert, w.eur, w.hel, w.a}, c{w.nokia, w.eur, w.hel, w.a};
  for (const EdgeKey & k : {a, b, c}) w.store->appendCandles(k, Resolution::Min1, {bar(0, 1)});
  CHECK(w.snap()->said(NodeRef(*w.snap()->findInstrument("FLXK")), predicate::type) == "UETF" && w.snap()->said(NodeRef(*w.snap()->findInstrument("NOKIA")), predicate::type).empty());
  CHECK(w.snap()->edges(w.hel).size() == 3);
  auto isType = [&w](const char * t) { return [&w, t](InstrumentId i) { return w.snap()->said(NodeRef(i), predicate::type) == t; }; };
  CHECK(w.snap()->edges(w.hel, isType("PTC")).size() == 1 && w.snap()->edges(w.hel, isType("PTC"))[0] == b);
  CHECK(w.snap()->edges(w.hel, isType("UETF")).size() == 1);
  CHECK(w.snap()->edges(w.hel, isType("nothing")).empty());
  CHECK(w.snap()->edges(w.hel, [&w](InstrumentId i) { return w.snap()->kind(i) == InstrumentKind::Etf; }).size() == 1);
  CHECK(w.snap()->edges(w.sto, isType("PTC")).empty());
}

TEST(an_instrument_with_no_edge_is_not_seen_by_a_market) {
  World w;
  w.store->addInstrument("LONELY", "No data", InstrumentKind::Etf, "XX0000000002", "UETF");
  CHECK(w.snap()->findInstrument("LONELY"));  // it exists, as a node
  CHECK(w.snap()->edges(w.hel).empty() && w.snap()->edges(w.sto).empty() && w.snap()->edgeMarkets().empty());
}

TEST(an_instrument_is_its_id_and_what_is_said_of_it_is_read_from_the_facts) {
  World w;
  auto s = w.snap();
  const InstrumentId ericsson = w.store->addInstrument("ERIC", "Ericsson", InstrumentKind::Company, "SE0000108656", "EQUITY");
  s = w.snap();
  CHECK(s->isin(ericsson) == "SE0000108656" && s->symbol(ericsson) == "ERIC" && s->name(ericsson) == "Ericsson");
  CHECK(s->kind(ericsson) == InstrumentKind::Company && s->said(NodeRef(ericsson), predicate::type) == "EQUITY");
  CHECK(s->known(NodeRef(ericsson)) && *s->findInstrumentByIsin("SE0000108656") == ericsson && *s->findInstrument("ERIC") == ericsson);
  // A currency has no ISIN: its code is its key and its symbol.
  CHECK(s->isin(w.eur).empty() && s->symbol(w.eur) == "EUR" && s->kind(w.eur) == InstrumentKind::Currency);
  // What nothing is said of reads as nothing.
  const InstrumentId nobody{};
  CHECK(!s->known(NodeRef(nobody)) && s->symbol(nobody).empty() && s->name(nobody).empty() && s->kind(nobody) == InstrumentKind::Other);
  CHECK(!s->findInstrumentByIsin("SE0000000009") && !s->findInstrument("NOPE"));
}

TEST(
    what_is_said_of_an_edge_with_a_part_that_was_refused_throws_and_writes_nothing) {
  World w;
  const InstrumentId refused = w.store->addInstrument("X", "A wrong ISIN", InstrumentKind::Company, "FI0009000680");  // the check digit is wrong
  const MarketId noMarket = w.store->addMarket("not a mic");
  CHECK(!refused.valid() && !noMarket.valid());
  const uint64_t version = w.snap()->version();
  auto throws = [](auto &&write) {
    try {
      write();
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  for (const EdgeKey &bad : {EdgeKey{refused, w.eur, w.hel, w.a},
                             EdgeKey{w.eur, w.eur, noMarket, w.a}}) {
    CHECK(throws(
        [&] { w.store->appendCandles(bad, Resolution::Day, {bar(1, 1.0)}); }));
    CHECK(throws([&] { w.store->appendBooks(bad, {BookSnapshot{}}); }));
    CHECK(throws([&] { w.store->setPreviousClose(bad, 1, 1.0); }));
    CHECK(throws([&] { w.store->setListing({bad, "X", "", "", "", ""}); }));
  }
  CHECK(throws(
      [&] { w.store->appendActions({refused, w.a}, {CorporateAction{}}); }));
  CHECK(w.snap()->version() ==
        version); // nothing changed: no edge under a part that is none
  CHECK(w.snap()->edges(w.hel).empty() && w.snap()->events(refused).empty());
}

TEST(a_batch_makes_one_version_and_one_snapshot_however_many_writes_it_holds) {
  World w;
  const auto before = w.snap();
  EdgeKey edge = w.key(w.hel, w.a);
  InstrumentId made;
  w.store->batch([&](Batch & b) {
    made = b.addInstrument("", "Neste", InstrumentKind::Company, "FI0009013296");
    for (int i = 0; i < 50; i++) b.appendCandles(edge, Resolution::Day, {bar(i, 1 + i)});
    b.setListing({edge, "NOKIA", "", "", "", "Nokia"});
    // What the batch has written is read from inside it, and the store shows none of it yet.
    CHECK(b.view().findInstrumentByIsin("FI0009013296") == made);
    CHECK(!w.snap()->findInstrumentByIsin("FI0009013296") && w.snap() == before);
  });
  const auto after = w.snap();
  CHECK(after != before && after->version() == before->version() + 1);
  // A reader holding the snapshot from before sees none of it.
  CHECK(!before->findInstrumentByIsin("FI0009013296") && before->candles(edge, Resolution::Day).candles.empty() && !before->listing(edge));
  CHECK(after->candles(edge, Resolution::Day).candles.size() == 50 && after->listing(edge) && after->name(made) == "Neste");
  CHECK(*after->findInstrumentByIsin("FI0009013296") == made);
}

TEST(a_batch_that_changes_nothing_publishes_nothing_and_one_that_throws_applies_nothing) {
  World w;
  const auto before = w.snap();
  w.store->batch([&](Batch & b) { b.addMarket("XHEL", "Nasdaq Helsinki"); });
  CHECK(w.snap() == before);
  bool thrown = false;
  try {
    w.store->batch([&](Batch & b) {
      b.addInstrument("", "Neste", InstrumentKind::Company, "FI0009013296");
      b.appendCandles(EdgeKey{}, Resolution::Day, {bar(1, 1)});
    });
  } catch (const std::invalid_argument &) {
    thrown = true;
  }
  CHECK(thrown && w.snap() == before && !w.snap()->findInstrumentByIsin("FI0009013296"));
}

TEST(a_listing_said_before_the_data_gives_the_same_reads_as_after_it) {
  World first, second;
  const EdgeKey edge = first.key(first.hel, first.a);
  const Listing listing{edge, "NOKIA", "id", "book", "STOCK", "Nokia"};
  first.store->setListing(listing);
  CHECK(first.snap()->edges(first.hel).empty() && first.snap()->quoteRowCount() == 0 && !first.snap()->findEdge("NOKIA", first.hel));
  first.store->appendCandles(edge, Resolution::Day, {bar(1, 5)});
  second.store->appendCandles(edge, Resolution::Day, {bar(1, 5)});
  second.store->setListing(listing);
  for (World * w : {&first, &second}) {
    const auto s = w->snap();
    CHECK(s->listing(edge) == listing && s->edges(w->hel).size() == 1 && s->quoteRowCount() == 1);
    CHECK(s->findEdge("NOKIA", w->hel) == edge && s->symbol(edge) == "NOKIA");
  }
}

TEST(the_same_bars_again_make_no_new_version_and_a_changed_close_does) {
  World w;
  const EdgeKey edge = w.key(w.hel, w.a);
  const Candle gap{1, kUndefined, kUndefined, kUndefined, 5, kUndefined};  // undefined numbers are NaN
  w.store->appendCandles(edge, Resolution::Day, {gap, bar(2, 6)});
  const auto first = w.snap();
  w.store->appendCandles(edge, Resolution::Day, {gap, bar(2, 6)});
  CHECK(w.snap() == first && w.snap()->version() == first->version());
  w.store->appendCandles(edge, Resolution::Day, {bar(2, 7)});
  CHECK(w.snap()->version() == first->version() + 1);
  CHECK(w.snap()->lastCandles(edge, Resolution::Day, 1).candles[0].close == 7);
  // The other series are the same: a repeat changes nothing.
  const NewsItem news = item("n", 10, "headline");
  const CorporateAction action{10, ActionKind::Split, 2, {}, ""};
  const BookSnapshot book{1.5, {{10, 1}}, {{11, 1}}};
  const EventKey event{w.nokia, w.a};
  for (int i = 0; i < 2; i++) {
    const uint64_t before = w.snap()->version();
    w.store->appendNews(w.a, {news});
    w.store->appendActions(event, {action});
    w.store->appendBooks(edge, {book});
    CHECK(w.snap()->version() == before + (i == 0 ? 3 : 0));
  }
}

TEST(an_instrument_with_no_isin_is_the_one_of_its_symbol_and_else_of_its_name_and_only_the_same_text_is_the_same_one) {
  auto store = makeMemoryStore();  // may invent ISINs: numbered as they come
  // The symbol is tried before the name: two names for one symbol are one instrument.
  const InstrumentId bySymbol = store->resolveInstrument({}, {{predicate::symbol, "ZN"}, {predicate::name, "Zinc"}});
  CHECK(bySymbol.valid() && store->snapshot()->findInstrumentByIsin(store->snapshot()->isin(bySymbol)) == std::optional<InstrumentId>(bySymbol));
  CHECK(store->resolveInstrument({}, {{predicate::symbol, "ZN"}, {predicate::name, "Zink"}}) == bySymbol);
  // With no symbol the name is the key, and the same text, written exactly, is the same instrument from any source.
  const InstrumentId byName = store->resolveInstrument({}, {{predicate::name, "Zinc"}});
  CHECK(byName.valid() && byName != bySymbol);
  CHECK(store->resolveInstrument({}, {{predicate::name, "Zinc"}}) == byName);
  CHECK(store->resolveInstrument({}, {{predicate::name, "ZINC"}}) != byName);  // not the same text: another one
}

TEST(a_row_is_a_rate_by_its_quotes_unit_alone) {
  World w;
  InstrumentId pct = w.store->addInstrument(kPercentUnit, "Per cent", InstrumentKind::Unit);
  InstrumentId pts = w.store->addInstrument(kPointsUnit, "Index points", InstrumentKind::Unit);
  CHECK(isRate(*w.snap(), {w.nokia, pct, w.hel, w.a}));
  CHECK(!isRate(*w.snap(), {w.nokia, pts, w.hel, w.a}) && !isRate(*w.snap(), {w.nokia, w.eur, w.hel, w.a}));
  // A change is basis points for a rate, per cent for a price, and a rate's base may be 0.
  PeriodView rate{.rate = true, .base = 0, .last = 0.25}, price{.base = 3.00, .last = 3.10};
  CHECK(rate.hasChange() && std::abs(rate.change() - 25) < 1e-9);
  CHECK(std::abs(price.change() - 10.0 / 3) < 1e-9);
  PeriodView zeroBase{.base = 0, .last = 1};
  CHECK(!zeroBase.hasChange());
}

// ---- Portfolios

namespace {

portfolio::Transaction tradeOf(const World & w, uint64_t id, MarketId market, double quantity = 10, double price = 4, int64_t day = 100) {
  portfolio::Transaction t;
  t.id = id;
  t.day = day;
  t.instrument = w.nokia;
  t.market = market;
  t.quote = w.eur;
  t.quantity = quantity;
  t.price = price;
  return t;
}

LedgerKey ledgerOf(const World & w, PortfolioId p, const char * dataset = "user") { return {p, w.store->addDataset(dataset)}; }

PortfolioId somePortfolio(uint64_t identity = 1) { return PortfolioId{portfolioKey(identity)->value}; }

}  // namespace

TEST(a_transaction_on_a_market_that_prices_and_does_not_sell_is_refused_and_says_so) {
  World w;
  const PortfolioId p = somePortfolio();
  const LedgerKey ledger = ledgerOf(w, p);
  const MarketId none = w.store->addMarket(kNoMarket, "No market"), off = w.store->addMarket(kOffExchange, "Off exchange");
  std::string why;
  const uint64_t start = w.snap()->version();
  try {
    w.store->appendTransactions(ledger, {tradeOf(w, 1, none)});
  } catch (const std::invalid_argument & e) {
    why = e.what();
  }
  CHECK(why.find("XXXX") != std::string::npos && why.find("refused") != std::string::npos);
  CHECK(w.snap()->transactions(ledger).empty() && w.snap()->portfolios().empty() && w.snap()->version() == start);  // nothing written
  // One bad one among good ones writes none of them.
  bool thrown = false;
  try {
    w.store->appendTransactions(ledger, {tradeOf(w, 2, w.hel), tradeOf(w, 3, none)});
  } catch (const std::invalid_argument &) { thrown = true; }
  CHECK(thrown && w.snap()->transactions(ledger).empty());
  // A fund off exchange and a real market are kept.
  w.store->appendTransactions(ledger, {tradeOf(w, 4, off), tradeOf(w, 5, w.hel)});
  CHECK(w.snap()->transactions(ledger).size() == 2);
  // A part that is none is refused as well.
  portfolio::Transaction nowhere = tradeOf(w, 6, w.hel);
  nowhere.instrument = InstrumentId{};
  bool refused = false;
  try { w.store->appendTransactions(ledger, {nowhere}); } catch (const std::invalid_argument &) { refused = true; }
  CHECK(refused);
  bool badLedger = false;
  try { w.store->appendTransactions(LedgerKey{PortfolioId{}, ledger.dataset}, {tradeOf(w, 7, w.hel)}); } catch (const std::invalid_argument &) { badLedger = true; }
  CHECK(badLedger);
}

TEST(a_portfolio_exists_when_something_is_said_of_it_and_a_name_alone_shows) {
  World w;
  const PortfolioId p = somePortfolio();
  CHECK(w.snap()->portfolios().empty() && !w.snap()->known(NodeRef(p)));  // a key with nothing said of it is not shown
  w.store->setAttribute(NodeRef(p), {predicate::name, "Retirement"});
  CHECK(w.snap()->portfolios() == std::vector<PortfolioId>{p});
  CHECK(w.snap()->name(NodeRef(p)) == "Retirement" && w.snap()->nodeCount(NodeType::Portfolio) == 1);
  CHECK(w.snap()->transactions(ledgerOf(w, p)).empty() && w.snap()->ledgers(p).empty());  // shown, empty
  CHECK(portfolio::holdings(*w.snap(), p).positions.empty());
}

TEST(positions_and_no_name_show_under_the_key_shortened_until_it_is_named) {
  World w;
  const PortfolioId p = somePortfolio(0xabcdef12345ull);
  w.store->appendTransactions(ledgerOf(w, p), {tradeOf(w, 1, w.hel)});
  CHECK(w.snap()->portfolios() == std::vector<PortfolioId>{p} && !w.snap()->known(NodeRef(p)));
  const std::string placeholder = w.snap()->name(NodeRef(p));
  CHECK(placeholder.size() == 7 && placeholder[0] == 'P' && text(NodeKey{p.value}).rfind(placeholder, 0) == 0);
  w.store->setAttribute(NodeRef(p), {predicate::name, "Trading"});
  CHECK(w.snap()->name(NodeRef(p)) == "Trading" && w.snap()->portfolios().size() == 1);
  CHECK(w.snap()->ledgers(p).size() == 1 && w.snap()->ledgers(p)[0] == ledgerOf(w, p).dataset);
}

TEST(a_delete_makes_a_portfolio_vanish_whatever_was_said_of_it) {
  World w;
  const PortfolioId p = somePortfolio(), other = somePortfolio(2);
  w.store->setAttributes(NodeRef(p), {{predicate::name, "Gone"}, {predicate::currency, NodeKey{w.eur.value}}});
  w.store->appendTransactions(ledgerOf(w, p), {tradeOf(w, 1, w.hel)});
  w.store->appendTransactions(ledgerOf(w, p, "broker"), {tradeOf(w, 2, w.hel)});
  w.store->setAttribute(NodeRef(other), {predicate::name, "Kept"});
  CHECK(w.snap()->portfolios().size() == 2 && w.snap()->findLinked(NodeType::Portfolio, predicate::currency, NodeKey{w.eur.value}).size() == 1);
  w.store->removePortfolio(p);
  auto s = w.snap();
  CHECK(s->portfolios() == std::vector<PortfolioId>{other} && !s->known(NodeRef(p)) && s->ledgers(p).empty());
  CHECK(s->transactions(ledgerOf(w, p)).empty() && s->transactions(ledgerOf(w, p, "broker")).empty());
  CHECK(s->findLinked(NodeType::Portfolio, predicate::currency, NodeKey{w.eur.value}).empty());
  const uint64_t version = s->version();
  w.store->removePortfolio(p);  // again: nothing there, nothing changes
  CHECK(w.snap()->version() == version);
  // Taking back the last fact, or the last transaction, makes it vanish too.
  const PortfolioId third = somePortfolio(3);
  w.store->setAttribute(NodeRef(third), {predicate::name, "Third"});
  w.store->removeAttribute(NodeRef(third), predicate::name);
  CHECK(w.snap()->portfolios() == std::vector<PortfolioId>{other});
  w.store->appendTransactions(ledgerOf(w, third), {tradeOf(w, 9, w.hel)});
  CHECK(w.snap()->portfolios().size() == 2);
  w.store->removeTransactions(ledgerOf(w, third), {9});
  CHECK(w.snap()->portfolios() == std::vector<PortfolioId>{other});
  // Only what is a portfolio's can be taken back.
  bool refused = false;
  try { w.store->removeAttribute(NodeRef(w.nokia), predicate::name); } catch (const std::invalid_argument &) { refused = true; }
  CHECK(refused && !w.snap()->name(NodeRef(w.nokia)).empty());
}

TEST(several_portfolios_are_listed_by_name_and_each_has_its_own_transactions) {
  World w;
  const PortfolioId b = somePortfolio(1), a = somePortfolio(2), c = somePortfolio(3);
  w.store->setAttribute(NodeRef(b), {predicate::name, "Bonds"});
  w.store->setAttribute(NodeRef(a), {predicate::name, "Active"});
  w.store->appendTransactions(ledgerOf(w, c), {tradeOf(w, 1, w.hel)});  // unnamed: listed by its shortened key
  const auto listed = w.snap()->portfolios();
  CHECK(listed.size() == 3 && listed[0] == a && listed[1] == b && listed[2] == c);
  w.store->appendTransactions(ledgerOf(w, a), {tradeOf(w, 2, w.hel, 5), tradeOf(w, 3, w.hel, 7, 4, 90)});
  CHECK(w.snap()->transactions(ledgerOf(w, a)).size() == 2 && w.snap()->transactions(ledgerOf(w, b)).empty() && w.snap()->transactions(ledgerOf(w, c)).size() == 1);
  CHECK(w.snap()->transactions(ledgerOf(w, a))[0].id == 3);  // by day, then id
}

TEST(a_transaction_is_replaced_by_its_id_and_taken_back_by_it) {
  World w;
  const PortfolioId p = somePortfolio();
  const LedgerKey ledger = ledgerOf(w, p);
  w.store->appendTransactions(ledger, {tradeOf(w, 1, w.hel, 10, 4)});
  const uint64_t version = w.snap()->version();
  w.store->appendTransactions(ledger, {tradeOf(w, 1, w.hel, 10, 4)});  // the same again
  CHECK(w.snap()->version() == version && w.snap()->transactions(ledger).size() == 1);
  w.store->appendTransactions(ledger, {tradeOf(w, 1, w.hel, 12, 4, 50)});  // corrected, and on another day
  CHECK(w.snap()->version() == version + 1 && w.snap()->transactions(ledger).size() == 1 && w.snap()->transactions(ledger)[0].quantity == 12 && w.snap()->transactions(ledger)[0].day == 50);
  w.store->appendTransactions(ledger, {tradeOf(w, 2, w.hel, 1, 1, 10)});
  CHECK(w.snap()->transactions(ledger)[0].id == 2 && w.snap()->transactions(ledger)[1].id == 1);
  w.store->removeTransactions(ledger, {1, 77});
  CHECK(w.snap()->transactions(ledger).size() == 1 && w.snap()->transactions(ledger)[0].id == 2);
  // The old snapshot is what it was.
  CHECK(w.snap()->transactions(ledger).size() == 1);
}

TEST(holdings_come_from_the_transactions_of_the_one_dataset_the_portfolio_names) {
  World w;
  const PortfolioId p = somePortfolio();
  const LedgerKey mine = ledgerOf(w, p), broker = ledgerOf(w, p, "broker");
  w.store->setAttribute(NodeRef(p), {predicate::name, "Both"});
  // The same two trades, entered by hand and imported: two entries in two datasets, not one replacing the other.
  for (const LedgerKey & ledger : {mine, broker}) w.store->appendTransactions(ledger, {tradeOf(w, 1, w.hel, 10, 4), tradeOf(w, 2, w.hel, 5, 6, 101)});
  w.store->appendTransactions(broker, {tradeOf(w, 3, w.hel, 1, 7, 102)});
  CHECK(w.snap()->ledgers(p).size() == 2);
  // No ledger said: the user's own, and the quantities are not added up.
  auto own = portfolio::holdings(*w.snap(), p);
  CHECK(portfolio::ledgerOf(*w.snap(), p) == mine.dataset && own.positions.size() == 1 && own.positions[0].quantity == 15);
  w.store->setAttribute(NodeRef(p), {predicate::ledger, NodeKey{broker.dataset.value}});
  auto imported = portfolio::holdings(*w.snap(), p);
  CHECK(portfolio::ledgerOf(*w.snap(), p) == broker.dataset && imported.positions.size() == 1 && imported.positions[0].quantity == 16);
  w.store->setAttribute(NodeRef(p), {predicate::ledger, NodeKey{mine.dataset.value}});
  CHECK(portfolio::holdings(*w.snap(), p).positions[0].quantity == 15);
  // The currency is a link to a currency node.
  CHECK(!portfolio::currencyOf(*w.snap(), p));
  w.store->setAttribute(NodeRef(p), {predicate::currency, NodeKey{w.eur.value}});
  CHECK(portfolio::currencyOf(*w.snap(), p) == std::optional<InstrumentId>(w.eur));
}

TEST(what_is_said_of_a_portfolio_is_one_version_in_a_batch_and_a_refused_transaction_applies_none_of_it) {
  World w;
  const PortfolioId p = somePortfolio();
  const MarketId none = w.store->addMarket(kNoMarket);
  const uint64_t before = w.snap()->version();
  bool thrown = false;
  try {
    w.store->batch([&](Batch & b) {
      b.setAttribute(NodeRef(p), {predicate::name, "Half"});
      b.appendTransactions({p, b.addDataset("user")}, {tradeOf(w, 1, none)});
    });
  } catch (const std::invalid_argument &) { thrown = true; }
  CHECK(thrown && w.snap()->version() == before && w.snap()->portfolios().empty());
  w.store->batch([&](Batch & b) {
    b.setAttribute(NodeRef(p), {predicate::name, "Whole"});
    b.appendTransactions({p, b.addDataset("user")}, {tradeOf(w, 1, w.hel)});
  });
  CHECK(w.snap()->version() == before + 1 && w.snap()->portfolios().size() == 1);
}
