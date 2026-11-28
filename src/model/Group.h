#pragma once

#include "model/Period.h"
#include "model/Store.h"

#include <functional>
#include <optional>
#include <vector>

// One instrument priced in one unit is one *group* (`GroupKey`, Types.h); the edges in it, one for each market and dataset that prices it, are its
// *members*. A table shows a group as one row and reads every number of it from its members, so a row is what the datasets say together
// and not what one of them says. The store keeps the edges apart, as they were written: nothing here is stored.
// The quote instrument is part of the group's key, so every member is priced in the same unit (a currency, or per cent for a rate):
// the members can be compared and combined without any conversion, and a rate is never mixed with a price.

// Which datasets count: the user's switches. Empty: every dataset does.
using DatasetFilter = std::function<bool(DatasetId)>;

// The edges of a group that have a row (`Snapshot::listed`), sorted by key (market, then dataset). `market`, where given, keeps only that
// market's: a preset of one market shows what that market says and no other.
std::vector<EdgeKey> membersOf(const Snapshot &, const GroupKey &, std::optional<MarketId> market = std::nullopt);

// How the members' values are made one: the median where there are three or more, the mean of two, the value itself of one; none
// for no value. A value that is not defined is not one. The median keeps a single wrong dataset from moving the number.
double combine(std::vector<double> values);
// How much the values disagree: the median distance of a value from `combine(values)`, as a fraction of its size (0.01: a member is typically
// one per cent off). Undefined for fewer than two values or a combined value of 0.
double disagreement(const std::vector<double> & values);

// How timely a dataset says it is (the `timeliness` fact of the dataset, which its source states): a *realtime* one says what is happening now, a
// *delayed* one (also one that says nothing: it is not known to be faster) is late by a pipeline or a feed, and a *daily* one says a price a day and
// not the second. The faster say what the slower only say later, so the numbers of a row are the first of these that a row has, never mixed with the next:
// a delayed price is older than it looks, and a daily one has no exact time.
enum class Timeliness : uint8_t { Realtime, Delayed, Daily };
Timeliness timelinessOf(const Snapshot &, DatasetId);

// What counts as *live*, and what is left out, among the members of one kind of timeliness. The newest of them is live when it said its price within
// `liveWithin` (a minute for realtime, an hour for delayed) of now. Where one is, a member more than that behind it is *stale*: it has stopped (a market
// that has closed, a feed that fell behind), so what it last said is not what the others say, and it is left out. Where none is live (a weekend, a night,
// every market closed), nobody is fresher than another by being later, and they all count, the median of the last prices: a member is left out only
// when it is more than `kClosedWindow` older than the newest (a market shut for days). A daily dataset is never live (its time is a date).
inline constexpr int64_t kRealtimeWindow = 60;
inline constexpr int64_t kDelayedWindow = 3600;
inline constexpr int64_t kClosedWindow = 4 * 86400;
// The time of the machine, UTC seconds: what `now` is where a reader does not say.
Timestamp systemNow();
// What tells the time to a reader that works out a mix: the machine's by default (an empty one), a test's own.
using TimeFn = std::function<Timestamp()>;

enum class MemberState : uint8_t {
  Used,     // counts in the combined numbers
  Stale,    // says nothing as new as the others of its kind: left out
  Behind,   // a faster kind of dataset (realtime before delayed before daily) says the price: left out
  Off,      // its dataset is switched off (the filter): left out
  NoPrice,  // has no price to say: left out
};

struct Member {
  EdgeKey edge;
  MemberState state = MemberState::NoPrice;
  Timeliness timeliness = Timeliness::Delayed;
  double price = kUndefined;   // the member's own last price
  Timestamp time = 0;          // when it said it, UTC seconds; 0 where not known
  double offset = kUndefined;  // its price against the combined one as a fraction (0.01: one per cent above); undefined for a combined 0
};

// What a group's members say together: the members in dataset order, whether each counts, and the combined price.
struct Mix {
  std::vector<Member> members;
  double price = kUndefined;          // `combine` of the used members' prices
  double disagreement = kUndefined;   // of the used members' prices
  size_t used = 0;                    // how many members count
  bool combined() const { return used > 1; }
};
Mix mixOf(const Snapshot &, const GroupKey &, const DatasetFilter & filter = {}, std::optional<MarketId> market = std::nullopt, Timestamp now = 0);  // now 0: `systemNow()`

// The change of a group over a period, in the unit of `PeriodView::change()`: `combine` of the used members' own changes
// (`Snapshot::changes`); undefined where none says one.
double groupChange(const Snapshot &, const GroupKey &, Period, const DatasetFilter & filter = {});
// The same from the mix of the group, which a reader that has worked it out already has.
double groupChange(const Snapshot &, const Mix &, Period);
// The highest (or the lowest) price of the period: `combine` of the used members' own (`Snapshot::ranges`); undefined where none says one.
double groupRange(const Snapshot &, const Mix &, Period, bool high);

// What a row shows for a period, from the used members: the trace is their closes made one (`spread`: how much they disagree at each), at each time any of them has a bar (a member
// that has none at that time counts with its last close before it, and not at all before its first), and the base and the last price are the
// members' made one. One used member: its own view, exactly. `mix`, where given, is the group's mix, so it is not worked out twice.
PeriodView groupPeriodView(const Snapshot &, const GroupKey &, Period, bool withTrace, const DatasetFilter & filter = {}, const Mix * mix = nullptr);
