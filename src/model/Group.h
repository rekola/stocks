#pragma once

#include "model/Period.h"
#include "model/Store.h"

#include <functional>
#include <vector>

// One instrument priced in one unit on one market is one *group*; the edges in it, one for each dataset that prices it, are its
// *members*. A table shows a group as one row and reads every number of it from its members, so a row is what the datasets say together
// and not what one of them says. The store keeps the edges apart, as they were written: nothing here is stored.
//
// The quote instrument is part of the group's key, so every member is priced in the same unit (a currency, or per cent for a rate):
// the members can be compared and combined without any conversion, and a rate is never mixed with a price.
struct GroupKey {
  MarketId market;
  InstrumentId base, quote;
  friend bool operator==(const GroupKey &, const GroupKey &) = default;
  friend bool operator<(const GroupKey & a, const GroupKey & b) {
    return std::tie(a.market, a.base, a.quote) < std::tie(b.market, b.base, b.quote);
  }
};
template <>
struct std::hash<GroupKey> {
  size_t operator()(const GroupKey & k) const {
    size_t h = std::hash<InstrumentId>()(k.base);
    h = hashCombine(h, std::hash<InstrumentId>()(k.quote));
    return hashCombine(h, std::hash<MarketId>()(k.market));
  }
};

inline GroupKey groupOf(const EdgeKey & e) { return {e.market, e.base, e.quote}; }

// Which datasets count: the user's switches. Empty: every dataset does.
using DatasetFilter = std::function<bool(DatasetId)>;

// The edges of a group that have a row (`Snapshot::listed`), sorted by key (so by dataset).
std::vector<EdgeKey> membersOf(const Snapshot &, const GroupKey &);

// How the members' values are made one: the median where there are three or more, the mean of two, the value itself of one; none
// for no value. A value that is not defined is not one. The median keeps a single wrong dataset from moving the number.
double combine(std::vector<double> values);
// How much the values disagree: the median distance of a value from `combine(values)`, as a fraction of its size (0.01: a member is typically
// one per cent off). Undefined for fewer than two values or a combined value of 0.
double disagreement(const std::vector<double> & values);

// A member that says nothing newer than this much (seconds) before the newest of the members is *stale*: its dataset has stopped, and what
// it last said is no longer what the others are saying, so it is left out of the combined number.
inline constexpr int64_t kStaleAfter = 4 * 86400;

enum class MemberState : uint8_t {
  Used,     // counts in the combined numbers
  Stale,    // says nothing as new as the others: left out
  Off,      // its dataset is switched off (the filter): left out
  NoPrice,  // has no price to say: left out
};

struct Member {
  EdgeKey edge;
  MemberState state = MemberState::NoPrice;
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
Mix mixOf(const Snapshot &, const GroupKey &, const DatasetFilter & filter = {});

// The change of a group over a period, in the unit of `PeriodView::change()`: `combine` of the used members' own changes
// (`Snapshot::changes`); undefined where none says one.
double groupChange(const Snapshot &, const GroupKey &, Period, const DatasetFilter & filter = {});

// What a row shows for a period, from the used members: the trace is their closes made one, at each time any of them has a bar (a member
// that has none at that time counts with its last close before it, and not at all before its first), and the base and the last price are the
// members' made one. One used member: its own view, exactly. `mix`, where given, is the group's mix, so it is not worked out twice.
PeriodView groupPeriodView(const Snapshot &, const GroupKey &, Period, bool withTrace, const DatasetFilter & filter = {}, const Mix * mix = nullptr);
