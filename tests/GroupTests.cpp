#include "TestFramework.h"

#include "AttributeView.h"
#include "model/Dates.h"
#include "model/Group.h"

#include <cmath>

namespace {

constexpr Timestamp kNow = 1'800'000'000;

struct World {
  std::unique_ptr<Store> store = makeMemoryStore();
  InstrumentId gold = store->addInstrument("GOLD", "Gold", InstrumentKind::Commodity);
  InstrumentId eur = store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = store->addInstrument("USD", "Dollar", InstrumentKind::Currency);
  MarketId off = store->addMarket("XOFF", "Off exchange");
  MarketId lbm = store->addMarket("XLBM", "London Bullion Market");
  DatasetId a = store->addDataset("DSA"), b = store->addDataset("DSB"), c = store->addDataset("DSC");
  std::shared_ptr<const Snapshot> snap() const { return store->snapshot(); }
  EdgeKey key(DatasetId d, InstrumentId quote = InstrumentId{}, MarketId market = MarketId{}) const { return {gold, quote.value ? quote : eur, market.value ? market : off, d}; }
  GroupKey group() const { return {gold, eur}; }
  // The edge says a price at a time, and has daily bars to the same close.
  void say(DatasetId d, double price, Timestamp time = kNow, MarketId market = MarketId{}) {
    const EdgeKey k = key(d, InstrumentId{}, market);
    Quote q;
    q.last = price;
    q.time = time;
    store->setQuote(k, q);
    store->appendCandles(k, Resolution::Day, {{dayNumberOfInstant(time), price, price, price, price, 1}});
  }
};

}  // namespace

TEST(combine_is_the_median_of_three_or_more_the_mean_of_two_and_the_value_of_one) {
  CHECK(combine({5}) == 5);
  CHECK(combine({4, 6}) == 5);
  CHECK(combine({1, 100, 3}) == 3);                  // one wrong dataset does not move it
  CHECK(combine({1, 2, 3, 100}) == 2.5);
  CHECK(!isDefined(combine({})));
  CHECK(combine({kUndefined, 4, kUndefined, 8}) == 6);  // what is not said is not a value
}

TEST(disagreement_is_the_typical_distance_from_the_combined_value) {
  CHECK(!isDefined(disagreement({5})));
  CHECK(std::abs(disagreement({99, 101}) - 0.01) < 1e-12);
  CHECK(disagreement({100, 100, 100}) == 0);
  CHECK(!isDefined(disagreement({-1, 1})));  // a combined value of 0 has no relative spread
}

TEST(the_members_of_a_group_are_the_edges_in_one_unit_and_a_group_in_another_unit_is_another_row) {
  World w;
  w.say(w.a, 100);
  w.say(w.b, 102);
  w.store->setQuote(w.key(w.c, w.usd), Quote{110, kUndefined, kUndefined, kNow});
  w.store->appendCandles(w.key(w.c, w.usd), Resolution::Day, {{dayNumberOfInstant(kNow), 110, 110, 110, 110, 1}});
  const auto members = membersOf(*w.snap(), w.group());
  CHECK(members.size() == 2 && members[0].dataset == w.a && members[1].dataset == w.b);
  CHECK(membersOf(*w.snap(), {w.gold, w.usd}).size() == 1);
  CHECK(groupOf(w.key(w.a)) == w.group());
}

TEST(the_members_of_a_group_are_of_every_market_and_a_market_can_be_asked_for_alone) {
  World w;
  w.say(w.a, 100);
  w.say(w.a, 104, kNow, w.lbm);  // the same instrument in the same unit on another market
  w.say(w.b, 102, kNow, w.lbm);
  const auto all = membersOf(*w.snap(), w.group());
  CHECK(all.size() == 3);
  CHECK(membersOf(*w.snap(), w.group(), w.lbm).size() == 2 && membersOf(*w.snap(), w.group(), w.off).size() == 1);
  CHECK(groupOf(w.key(w.a, InstrumentId{}, w.lbm)) == groupOf(w.key(w.a)));
  const Mix mix = mixOf(*w.snap(), w.group());
  CHECK(mix.used == 3 && mix.price == 102);  // the median of the three, whatever market each is on
  const Mix only = mixOf(*w.snap(), w.group(), {}, w.lbm);  // a preset of one market says what that market says
  CHECK(only.used == 2 && only.price == 103 && only.members[0].edge.market == w.lbm);
}

static const Member & memberOf(const Mix & mix, DatasetId d) {
  for (const Member & m : mix.members)
    if (m.edge.dataset == d) return m;
  return mix.members.front();
}

TEST(a_member_that_is_not_live_is_left_out_where_another_is_and_all_stand_where_none_is) {
  World w;
  w.say(w.a, 100, kNow);
  w.say(w.b, 120, kNow - 3 * 86400, w.lbm);  // a market that has been closed for three days (a holiday): no longer what the others say
  Mix mix = mixOf(*w.snap(), w.group());
  CHECK(mix.used == 1 && mix.price == 100 && memberOf(mix, w.b).state == MemberState::Stale);
  w.say(w.c, 102, kNow - 1800);  // half an hour behind the newest: a delayed feed is still live
  mix = mixOf(*w.snap(), w.group());
  CHECK(mix.used == 2 && mix.price == 101);
  World slow;
  slow.say(slow.a, 100, kNow);
  slow.say(slow.c, 102, kNow - 3 * 3600);  // three hours behind: the other is live, this is not
  mix = mixOf(*slow.snap(), slow.group());
  CHECK(mix.used == 1 && mix.price == 100 && memberOf(mix, slow.c).state == MemberState::Stale);
  // Where none is live (all closed), the newest stands and the others are no older than it by a night: they all count.
  World closed;
  closed.say(closed.a, 100, kNow - 20 * 86400);
  closed.say(closed.b, 110, kNow - 20 * 86400 - 600);
  CHECK(mixOf(*closed.snap(), closed.group()).used == 2);
}

TEST(a_closed_market_does_not_stay_in_the_trace_while_another_trades_on) {
  World w;
  const int64_t day = dayNumberOfInstant(kNow);
  auto bars = [&](const EdgeKey & k, double close, int from, int to) {
    std::vector<Candle> v;
    for (int s = from; s <= to; s++) v.push_back({slotOfInstant(Resolution::Min5, (day - kUnixEpochDayNumber) * 86400 + 7 * 3600 + s * 300), close, close, close, close, 1});
    w.store->appendCandles(k, Resolution::Min5, v);
  };
  w.store->appendCandles(w.key(w.a), Resolution::Day, {{day, 100, 100, 100, 100, 1}});
  w.store->appendCandles(w.key(w.b, InstrumentId{}, w.lbm), Resolution::Day, {{day, 100, 100, 100, 100, 1}});
  bars(w.key(w.a), 100, 0, 40);                          // one trades all morning at 100
  bars(w.key(w.b, InstrumentId{}, w.lbm), 200, 0, 5);    // the other closed after half an hour at 200
  const Quote q{100, kUndefined, kUndefined, kNow + 8400};  // both have just said a price, so both count in the row
  w.store->setQuote(w.key(w.a), q);
  w.store->setQuote(w.key(w.b, InstrumentId{}, w.lbm), q);
  const PeriodView v = groupPeriodView(*w.snap(), w.group(), Period::Day, true);
  CHECK(v.closes.size() >= 30);
  CHECK(v.closes.front() == 150.0f);  // both trade
  CHECK(v.closes[10] == 150.0f);      // the closed one's last close is still what it says, for an hour
  CHECK(v.closes.back() == 100.0f);   // and is not carried on for the rest of the day
}

TEST(a_mix_combines_the_used_members_and_says_what_each_is_off_by) {
  World w;
  w.say(w.a, 100);
  w.say(w.b, 102);
  w.say(w.c, 101);
  const Mix mix = mixOf(*w.snap(), w.group());
  CHECK(mix.used == 3 && mix.combined() && mix.price == 101);
  CHECK(mix.members.size() == 3 && std::abs(mix.members[0].offset - (100.0 / 101 - 1)) < 1e-12 && mix.members[2].offset == 0);
  CHECK(isDefined(mix.disagreement) && mix.disagreement > 0);
}

TEST(a_dataset_switched_off_and_a_stale_member_are_left_out) {
  World w;
  w.say(w.a, 100);
  w.say(w.b, 120, kNow - 5 * 86400);  // stopped five days ago
  w.say(w.c, 104);
  Mix mix = mixOf(*w.snap(), w.group());
  CHECK(mix.members[1].state == MemberState::Stale && mix.used == 2 && mix.price == 102);
  mix = mixOf(*w.snap(), w.group(), [&](DatasetId d) { return d != w.a; });
  CHECK(mix.members[0].state == MemberState::Off && mix.used == 1 && mix.price == 104);  // the stale one never counts against the newest that is on
  mix = mixOf(*w.snap(), w.group(), [&](DatasetId d) { return d == w.b; });
  CHECK(mix.used == 1 && mix.price == 120 && mix.members[1].state == MemberState::Used);  // alone, it is not stale
  mix = mixOf(*w.snap(), w.group(), [](DatasetId) { return false; });
  CHECK(mix.used == 0 && !isDefined(mix.price));
}

TEST(one_used_member_gives_its_own_view_and_several_give_the_combined_trace) {
  World w;
  const int64_t today = dayNumberOfInstant(kNow);
  const auto days = [&](DatasetId d, std::vector<double> closes, int64_t first) {
    std::vector<Candle> bars;
    for (size_t i = 0; i < closes.size(); i++) bars.push_back({first + static_cast<int64_t>(i), closes[i], closes[i], closes[i], closes[i], 1});
    w.store->appendCandles(w.key(d), Resolution::Day, bars);
    w.store->setQuote(w.key(d), Quote{closes.back(), kUndefined, kUndefined, kNow});
  };
  days(w.a, {10, 12, 14}, today - 2);
  days(w.b, {20, 22, 24}, today - 2);
  days(w.c, {30, 32, 100}, today - 2);  // the last day of the third is wrong

  const PeriodView one = groupPeriodView(*w.snap(), w.group(), Period::Month, true, [&](DatasetId d) { return d == w.a; });
  const PeriodView own = periodView(*w.snap(), w.key(w.a), Period::Month, true);
  CHECK(one.closes == own.closes && one.times == own.times && one.last == own.last);

  const PeriodView all = groupPeriodView(*w.snap(), w.group(), Period::Month, true);
  CHECK(all.closes.size() == 3 && all.closes[0] == 20 && all.closes[1] == 22 && all.closes[2] == 24);  // the median, not the wrong 100
  CHECK(all.times.size() == 3 && all.last == 24);

  const PeriodView two = groupPeriodView(*w.snap(), w.group(), Period::Month, true, [&](DatasetId d) { return d != w.c; });
  CHECK(two.closes[0] == 15 && two.closes[2] == 19);  // the mean of two
  CHECK(!groupPeriodView(*w.snap(), w.group(), Period::Month, true, [](DatasetId) { return false; }).hasChange());
}

TEST(a_member_with_fewer_bars_carries_its_last_close_and_counts_only_after_its_first) {
  World w;
  const int64_t today = dayNumberOfInstant(kNow);
  w.store->appendCandles(w.key(w.a), Resolution::Day, {{today - 2, 10, 10, 10, 10, 1}, {today - 1, 11, 11, 11, 11, 1}, {today, 12, 12, 12, 12, 1}});
  w.store->appendCandles(w.key(w.b), Resolution::Day, {{today - 1, 20, 20, 20, 20, 1}});  // starts a day later and has no bar today
  w.store->setQuote(w.key(w.a), Quote{12, kUndefined, kUndefined, kNow});
  w.store->setQuote(w.key(w.b), Quote{20, kUndefined, kUndefined, kNow});
  const PeriodView v = groupPeriodView(*w.snap(), w.group(), Period::Month, true);
  CHECK(v.closes.size() == 3 && v.closes[0] == 10 && v.closes[1] == 15.5 && v.closes[2] == 16);
}

TEST(a_groups_change_is_the_combined_change_of_its_members) {
  World w;
  const int64_t today = dayNumberOfInstant(kNow);
  const auto days = [&](DatasetId d, double from, double to) {
    std::vector<Candle> bars;
    for (int i = 0; i <= 30; i++) {
      const double v = from + (to - from) * i / 30;
      bars.push_back({today - 30 + i, v, v, v, v, 1});
    }
    w.store->appendCandles(w.key(d), Resolution::Day, bars);
    w.store->setQuote(w.key(d), Quote{to, kUndefined, kUndefined, kNow});
  };
  days(w.a, 100, 110);
  days(w.b, 100, 120);
  days(w.c, 100, 160);
  const auto own = [&](DatasetId d) { return w.snap()->changes(w.key(d)).pct[static_cast<size_t>(Period::Month)]; };
  CHECK(isDefined(own(w.a)) && own(w.a) < own(w.b) && own(w.b) < own(w.c));
  CHECK(groupChange(*w.snap(), w.group(), Period::Month) == own(w.b));  // the median of the three
  CHECK(std::abs(groupChange(*w.snap(), w.group(), Period::Month, [&](DatasetId d) { return d != w.c; }) - (own(w.a) + own(w.b)) / 2) < 1e-9);
  CHECK(!isDefined(groupChange(*w.snap(), w.group(), Period::Month, [](DatasetId) { return false; })));
}

TEST(the_info_of_a_group_says_what_each_dataset_says_and_whether_it_counts) {
  World w;
  w.say(w.a, 100);
  w.say(w.b, 102);
  w.say(w.c, 90, kNow - 6 * 86400);
  const Mix mix = mixOf(*w.snap(), w.group());
  const std::vector<InfoSection> info = buildInfo(*w.snap(), w.key(w.a), &mix);
  CHECK(!info.empty() && info[0].title == "Sources");
  const auto row = [&](const std::string & name) {
    for (const InfoRow & r : info[0].rows)
      if (r.predicate == name) return r.value.text;
    return std::string();
  };
  CHECK(row("combined").find("101") != std::string::npos && row("combined").find("mean of 2") != std::string::npos);
  CHECK(row("Off exchange \xC2\xB7 DSA").find("100") != std::string::npos && row("Off exchange \xC2\xB7 DSC").find("left out: older") != std::string::npos);
  bool own_sections = false;
  for (const InfoSection & s : info) own_sections = own_sections || s.title == "Summary \xC2\xB7 Off exchange \xC2\xB7 DSB";
  CHECK(own_sections);
  CHECK(buildInfo(*w.snap(), w.key(w.a))[0].title == "Listing");  // no mix: as it was
}

TEST(a_groups_high_and_low_are_the_combined_highs_and_lows_of_its_members) {
  World w;
  w.say(w.a, 100);
  w.say(w.b, 102);
  w.say(w.c, 101);
  const auto range = [&](DatasetId d, double high, double low) {
    Ranges r;
    r.high[static_cast<size_t>(Period::Month)] = high;
    r.low[static_cast<size_t>(Period::Month)] = low;
    w.store->setRanges(w.key(d), r);
  };
  range(w.a, 110, 90);
  range(w.b, 130, 95);
  range(w.c, 112, 92);
  const Mix mix = mixOf(*w.snap(), w.group());
  CHECK(groupRange(*w.snap(), mix, Period::Month, true) == 112 && groupRange(*w.snap(), mix, Period::Month, false) == 92);  // the medians
  CHECK(!isDefined(groupRange(*w.snap(), mix, Period::Week, true)));
}
