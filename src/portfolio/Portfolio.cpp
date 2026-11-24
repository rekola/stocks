#include "portfolio/Portfolio.h"

#include "source/DatasetCodes.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace portfolio {

namespace {

constexpr double kEpsilon = 1e-9;  // what is left of a position that was sold: nothing

bool finite(double v) { return std::isfinite(v); }

std::string checkTransaction(const Transaction & t) {
  if (!t.instrument.valid() || !t.market.valid() || !t.quote.valid()) return "it has no instrument, market or currency";
  if (!finite(t.quantity) || t.quantity <= 0) return "its quantity is not above zero";
  if (!finite(t.price) || t.price < 0) return "its price is not a number of zero or more";
  if (!finite(t.fee) || t.fee < 0) return "its fee is not a number of zero or more";
  return {};
}

struct Group {
  Position position;
  double cost = 0;  // total, in the position's currency
  size_t order = 0;
};

}  // namespace

Derived derivePositions(std::vector<Transaction> transactions) {
  std::stable_sort(transactions.begin(), transactions.end(), [](const Transaction & a, const Transaction & b) {
    return std::make_tuple(a.day, a.side == Side::Sell, a.id) < std::make_tuple(b.day, b.side == Side::Sell, b.id);
  });
  Derived out;
  std::map<std::tuple<uint64_t, uint64_t, uint64_t>, Group> groups;
  size_t order = 0;
  for (const Transaction & t : transactions) {
    if (const std::string why = checkTransaction(t); !why.empty()) {
      out.refused.push_back({t.id, why});
      continue;
    }
    auto [it, fresh] = groups.try_emplace({t.instrument.value, t.market.value, t.quote.value});
    Group & g = it->second;
    if (fresh) {
      g.position.instrument = t.instrument;
      g.position.market = t.market;
      g.position.quote = t.quote;
      g.order = order++;
    }
    Position & p = g.position;
    if (t.side == Side::Sell && t.quantity > p.quantity + kEpsilon * std::max(1.0, p.quantity)) {
      out.refused.push_back({t.id, "it sells more than is held"});
      continue;
    }
    if (t.side == Side::Buy) {
      g.cost += t.quantity * t.price + t.fee;
      p.quantity += t.quantity;
    } else {
      p.quantity -= t.quantity;
      if (p.quantity <= kEpsilon * std::max(1.0, t.quantity)) p.quantity = 0;
      g.cost = p.quantity > 0 ? g.cost * (p.quantity / (p.quantity + t.quantity)) : 0;
    }
    p.average_cost = p.quantity > 0 ? g.cost / p.quantity : 0;
    if (!p.history.empty() && p.history.back().day == t.day) p.history.back().quantity = p.quantity;
    else p.history.push_back({t.day, p.quantity});
  }
  std::vector<const Group *> open;
  for (const auto & [key, g] : groups)
    if (g.position.quantity > 0) open.push_back(&g);
  std::sort(open.begin(), open.end(), [](const Group * a, const Group * b) { return a->order < b->order; });
  for (const Group * g : open) out.positions.push_back(g->position);
  return out;
}

std::optional<InstrumentId> currencyOf(const Snapshot & snapshot, PortfolioId id) {
  const auto link = snapshot.attribute(NodeRef(id), predicate::currency);
  const auto key = link ? asNode(*link) : std::nullopt;
  if (!key || !key->valid()) return std::nullopt;
  return InstrumentId{key->value};
}

DatasetId ledgerOf(const Snapshot & snapshot, PortfolioId id) {
  const auto link = snapshot.attribute(NodeRef(id), predicate::ledger);
  if (const auto key = link ? asNode(*link) : std::nullopt; key && key->kind() == NodeKind::Dataset) return DatasetId{key->value};
  const auto own = datasetKey(kUserDataset);
  return DatasetId{own ? own->value : 0};
}

Derived holdings(const Snapshot & snapshot, PortfolioId id) {
  const Seq<Transaction> all = snapshot.transactions(LedgerKey{id, ledgerOf(snapshot, id)});
  return derivePositions(std::vector<Transaction>(all.begin(), all.end()));
}

bool tradable(MarketId market) {
  static const uint64_t none = marketKey(kNoMarket)->value;
  return market.valid() && market.value != none;
}

bool tradable(const Snapshot &, const EdgeKey & edge) { return edge.base.valid() && tradable(edge.market); }

namespace {

// The latest close of the daily bars of the edges `keep` accepts: the one with the latest bar, the first by key on a tie.
template <class Keep>
std::optional<Price> latestOf(const Snapshot & snapshot, const std::vector<EdgeKey> & edges, Keep keep) {
  std::optional<Price> best;
  for (const EdgeKey & key : edges) {
    if (!keep(key)) continue;
    const CandleSeries last = snapshot.lastCandles(key, Resolution::Day, 1);
    if (last.candles.empty() || !isDefined(last.candles.back().close)) continue;
    const Candle & bar = last.candles.back();
    if (!best || bar.time > best->day || (bar.time == best->day && key < best->edge)) best = Price{key, bar.close, bar.time};
  }
  return best;
}

bool isCurrency(const Snapshot & snapshot, InstrumentId id) { return snapshot.kind(id) == InstrumentKind::Currency; }

// One edge between two currencies, either way round.
std::optional<double> direct(const Snapshot & snapshot, InstrumentId from, InstrumentId to) {
  if (const auto p = latestOf(snapshot, snapshot.edgesOf(from), [&](const EdgeKey & k) { return k.quote == to; }); p && p->close > 0) return p->close;
  if (const auto p = latestOf(snapshot, snapshot.edgesOf(to), [&](const EdgeKey & k) { return k.quote == from; }); p && p->close > 0) return 1 / p->close;
  return std::nullopt;
}

}  // namespace

std::optional<Price> latestPrice(const Snapshot & snapshot, InstrumentId instrument, MarketId market, InstrumentId quote) {
  return latestOf(snapshot, snapshot.edgesOf(instrument), [&](const EdgeKey & k) { return k.market == market && k.quote == quote; });
}

std::optional<double> rate(const Snapshot & snapshot, InstrumentId from, InstrumentId to) {
  if (!from.valid() || !to.valid()) return std::nullopt;
  if (from == to) return 1.0;
  if (!isCurrency(snapshot, from) || !isCurrency(snapshot, to)) return std::nullopt;
  if (const auto r = direct(snapshot, from, to)) return r;
  // Through one other currency, the lowest by key that has an edge to each end.
  std::vector<InstrumentId> middles;
  for (const EdgeKey & k : snapshot.edgesOf(from)) middles.push_back(k.quote);
  for (const EdgeKey & k : snapshot.edgesTo(from)) middles.push_back(k.base);
  std::sort(middles.begin(), middles.end());
  middles.erase(std::unique(middles.begin(), middles.end()), middles.end());
  for (InstrumentId via : middles) {
    if (via == from || via == to || !isCurrency(snapshot, via)) continue;
    const auto first = direct(snapshot, from, via);
    const auto second = first ? direct(snapshot, via, to) : std::nullopt;
    if (first && second) return *first * *second;
  }
  return std::nullopt;
}

Valuation value(const std::vector<Position> & positions, const Snapshot & snapshot, InstrumentId currency) {
  Valuation out;
  out.currency = currency;
  for (const Position & p : positions) {
    PositionValue row;
    row.position = p;
    row.price = latestPrice(snapshot, p.instrument, p.market, p.quote);
    if (row.price) {
      row.market_value = p.quantity * row.price->close;
      row.profit = row.market_value - p.cost();
      row.profit_percent = p.cost() > 0 ? row.profit / p.cost() * 100 : kUndefined;
      row.rate = rate(snapshot, p.quote, currency);
      if (row.rate) row.value = row.market_value * *row.rate;
    }
    if (row.value) out.total += *row.value;
    else out.incomplete = true;
    out.rows.push_back(std::move(row));
  }
  if (out.total > 0)
    for (PositionValue & row : out.rows)
      if (row.value) row.weight = *row.value / out.total;
  return out;
}

}  // namespace portfolio
