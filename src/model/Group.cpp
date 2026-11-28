#include "model/Group.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

std::vector<EdgeKey> membersOf(const Snapshot & snapshot, const GroupKey & group, std::optional<MarketId> market) {
  std::vector<EdgeKey> out;
  for (const EdgeKey & e : snapshot.edgesOf(group.base))  // sorted by market, base, quote, dataset
    if (e.quote == group.quote && (!market || e.market == *market) && snapshot.listed(e)) out.push_back(e);
  return out;
}

namespace {

double median(std::vector<double> & v) {
  const size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
  const double upper = v[mid];
  if (v.size() % 2 == 1) return upper;
  return (*std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid)) + upper) / 2;
}

std::vector<double> defined(const std::vector<double> & values) {
  std::vector<double> out;
  out.reserve(values.size());
  for (double v : values)
    if (isDefined(v)) out.push_back(v);
  return out;
}

}  // namespace

double combine(std::vector<double> values) {
  values = defined(values);
  if (values.empty()) return kUndefined;
  if (values.size() == 2) return (values[0] + values[1]) / 2;
  return median(values);
}

double disagreement(const std::vector<double> & values) {
  const std::vector<double> v = defined(values);
  if (v.size() < 2) return kUndefined;
  const double c = combine(v);
  if (c == 0) return kUndefined;
  std::vector<double> distance;
  distance.reserve(v.size());
  for (double x : v) distance.push_back(std::abs(x - c));
  return median(distance) / std::abs(c);
}

Timeliness timelinessOf(const Snapshot & snapshot, DatasetId dataset) {
  const std::string said = snapshot.said(NodeRef(dataset), predicate::timeliness);
  return said == "realtime" ? Timeliness::Realtime : said == "daily" ? Timeliness::Daily : Timeliness::Delayed;
}

Timestamp systemNow() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

Mix mixOf(const Snapshot & snapshot, const GroupKey & group, const DatasetFilter & filter, std::optional<MarketId> market, Timestamp now) {
  Mix mix;
  for (const EdgeKey & e : membersOf(snapshot, group, market)) {
    Member m;
    m.edge = e;
    const std::optional<Quote> quote = snapshot.quote(e);
    if (quote && isDefined(quote->last)) {
      m.price = quote->last;
      m.time = quote->time;
    }
    if (filter && !filter(e.dataset)) m.state = MemberState::Off;
    else if (!isDefined(m.price)) m.state = MemberState::NoPrice;
    else {
      m.state = MemberState::Used;
    }
    m.timeliness = timelinessOf(snapshot, e.dataset);
    mix.members.push_back(m);
  }
  if (now == 0) now = systemNow();
  // The fastest kind that has a member that counts and is not long dead (a realtime dataset that stopped days ago does not hide the others).
  std::optional<Timestamp> overall;
  for (const Member & m : mix.members)
    if (m.state == MemberState::Used && m.time != 0) overall = overall ? std::max(*overall, m.time) : m.time;
  auto newestOf = [&](Timeliness kind) {
    std::optional<Timestamp> t;
    for (const Member & m : mix.members)
      if (m.state == MemberState::Used && m.timeliness == kind && m.time != 0) t = t ? std::max(*t, m.time) : m.time;
    return t;
  };
  std::optional<Timeliness> kind;
  for (Timeliness k : {Timeliness::Realtime, Timeliness::Delayed, Timeliness::Daily}) {
    bool any = false;
    for (const Member & m : mix.members) any = any || (m.state == MemberState::Used && m.timeliness == k && (m.time == 0 || !overall || *overall - m.time <= kClosedWindow));
    if (any) {
      kind = k;
      break;
    }
  }
  std::optional<Timestamp> newest;
  int64_t window = kClosedWindow;
  if (kind) {
    newest = newestOf(*kind);
    const int64_t live = *kind == Timeliness::Realtime ? kRealtimeWindow : *kind == Timeliness::Delayed ? kDelayedWindow : -1;
    if (newest && live >= 0 && now - *newest <= live) window = live;  // someone is live: the others of the kind are as late as they are behind it
  }
  std::vector<double> prices;
  for (Member & m : mix.members) {
    if (m.state != MemberState::Used) continue;
    if (kind && m.timeliness != *kind) m.state = m.time != 0 && overall && *overall - m.time > kClosedWindow ? MemberState::Stale : MemberState::Behind;
    else if (m.time != 0 && newest && *newest - m.time > window) m.state = MemberState::Stale;  // a time of 0 is not known: not stale
    if (m.state == MemberState::Used) prices.push_back(m.price);
  }
  mix.used = prices.size();
  mix.price = combine(prices);
  mix.disagreement = disagreement(prices);
  for (Member & m : mix.members)
    if (isDefined(m.price) && isDefined(mix.price) && mix.price != 0) m.offset = m.price / mix.price - 1;
  return mix;
}

double groupChange(const Snapshot & snapshot, const GroupKey & group, Period period, const DatasetFilter & filter) {
  return groupChange(snapshot, mixOf(snapshot, group, filter), period);
}

double groupChange(const Snapshot & snapshot, const Mix & mix, Period period) {
  std::vector<double> changes;
  for (const Member & m : mix.members)
    if (m.state == MemberState::Used) changes.push_back(snapshot.changes(m.edge).pct[static_cast<size_t>(period)]);
  return combine(changes);
}

double groupRange(const Snapshot & snapshot, const Mix & mix, Period period, bool high) {
  std::vector<double> values;
  for (const Member & m : mix.members)
    if (m.state == MemberState::Used) {
      const Ranges r = snapshot.ranges(m.edge);
      values.push_back((high ? r.high : r.low)[static_cast<size_t>(period)]);
    }
  return combine(values);
}

PeriodView groupPeriodView(const Snapshot & snapshot, const GroupKey & group, Period period, bool withTrace, const DatasetFilter & filter, const Mix * given) {
  const Mix own = given ? Mix{} : mixOf(snapshot, group, filter);
  const Mix & mix = given ? *given : own;
  std::vector<PeriodView> views;
  for (const Member & m : mix.members)
    if (m.state == MemberState::Used) views.push_back(periodView(snapshot, m.edge, period, withTrace));
  if (views.empty()) return {};
  if (views.size() == 1) return std::move(views[0]);  // sources is 1, its own

  PeriodView out;
  out.rate = views[0].rate;  // one unit for the group
  out.sources = views.size();
  out.dated = true;
  out.span = 0;
  std::vector<double> bases, lasts, highs, lows;
  for (const PeriodView & v : views) {
    out.primary = out.primary || v.primary;
    out.dated = out.dated && v.dated;
    out.span = std::max(out.span, v.span);
    bases.push_back(v.base);
    lasts.push_back(v.last);
    highs.push_back(v.high);
    lows.push_back(v.low);
  }
  out.base = combine(bases);
  out.last = combine(lasts);
  out.high = combine(highs);
  out.low = combine(lows);
  if (!withTrace) return out;

  // The trace: at each time any member has a bar, the members' closes made one.
  std::vector<Timestamp> times;
  bool volumes = false;
  for (const PeriodView & v : views) {
    times.insert(times.end(), v.times.begin(), v.times.end());
    volumes = volumes || !v.volumes.empty();
  }
  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());
  // A member's last close is carried over only as far as it is still live: an hour, or four of its own bars where they are further apart (daily bars
  // over a holiday; four days at least), so a market that has closed does not stay in the median while another trades on.
  std::vector<int64_t> reach(views.size(), kDelayedWindow);
  for (size_t i = 0; i < views.size(); i++) {
    if (views[i].dated) reach[i] = 4 * 86400;  // a bar is a day: a market that was shut for a holiday is carried over it
    if (views[i].times.size() > 1) reach[i] = std::max<int64_t>(reach[i], 4 * (views[i].times.back() - views[i].times.front()) / static_cast<int64_t>(views[i].times.size() - 1));
  }
  std::vector<size_t> at(views.size(), 0);  // the member's next bar to look at
  std::vector<double> values, traded;
  for (Timestamp t : times) {
    values.clear();
    traded.clear();
    float bright = 0;
    for (size_t i = 0; i < views.size(); i++) {
      const PeriodView & v = views[i];
      while (at[i] < v.times.size() && v.times[at[i]] <= t) at[i]++;
      if (at[i] == 0) continue;  // not begun
      const size_t bar = at[i] - 1;
      if (t - v.times[bar] > reach[i]) continue;  // it has stopped: no longer what the others say
      values.push_back(v.closes[bar]);
      if (v.times[bar] != t) continue;  // carried over from an earlier bar: no volume of its own here
      if (bar < v.volumes.size() && isDefined(v.volumes[bar])) traded.push_back(v.volumes[bar]);
      if (bar < v.light.size()) bright = std::max(bright, v.light[bar]);
    }
    out.times.push_back(t);
    out.closes.push_back(static_cast<float>(combine(values)));
    const double spread = disagreement(values);
    out.spread.push_back(isDefined(spread) ? static_cast<float>(spread) : 0.0f);
    if (volumes) {
      double sum = 0;
      for (double x : traded) sum += x;
      out.volumes.push_back(traded.empty() ? kUndefined : sum);
      out.light.push_back(bright);
    }
  }
  return out;
}
