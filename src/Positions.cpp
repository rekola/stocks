#include "Positions.h"

#include "AttributeView.h"
#include "Quotes.h"

#include <algorithm>
#include <cmath>

namespace {

std::string currencyCode(const Snapshot & snapshot, InstrumentId id) {
  std::string code = snapshot.symbol(id);
  return code.empty() ? snapshot.name(NodeRef(id)) : code;
}

// An amount with two decimals and thousands kept together by nothing: the Table right-aligns, and a locale's separators would
// make the sort read another number.
std::string amount(double v) { return fixed(v, 2); }

}  // namespace

Positions::Positions(std::shared_ptr<const Snapshot> snapshot, PortfolioId portfolio, std::shared_ptr<IconLoader> icons)
    : icons_(std::move(icons)), portfolio_(portfolio) {
  setSnapshot(std::move(snapshot));
}

void Positions::setSnapshot(std::shared_ptr<const Snapshot> snapshot) {
  snapshot_ = std::move(snapshot);
  traces_.clear();
  name_ = portfolio_.valid() ? snapshot_->name(NodeRef(portfolio_)) : std::string();
  std::vector<portfolio::Position> held;
  if (portfolio_.valid()) held = portfolio::holdings(*snapshot_, portfolio_).positions;
  // The rows keep the order they had where they still are (a refresh does not shuffle them): the order the positions were opened in.
  InstrumentId currency;
  if (portfolio_.valid()) {
    if (const auto said = portfolio::currencyOf(*snapshot_, portfolio_)) currency = *said;
  }
  if (!currency.valid() && !held.empty()) currency = held.front().quote;
  valuation_ = portfolio::value(held, *snapshot_, currency);
}

void Positions::setPeriod(Period period) {
  period_ = period;
  traces_.clear();
}

EdgeKey Positions::key(int r) const {
  const portfolio::Position & p = row(r).position;
  return EdgeKey{p.instrument, p.quote, p.market, DatasetId{}};
}

std::optional<EdgeKey> Positions::priceEdge(int r) const {
  const auto & price = row(r).price;
  return price ? std::optional<EdgeKey>(price->edge) : std::nullopt;
}

std::string Positions::iconUrl(int r) const {
  auto url = snapshot_->attribute(row(r).position.instrument, predicate::iconUrl);
  return url ? asText(*url).value_or("") : std::string();
}

std::shared_ptr<const cellui::TableIcon> Positions::icon(int r, int c) const {
  if (!icons_ || c != ICON) return nullptr;
  const std::string url = iconUrl(r);
  return url.empty() ? nullptr : icons_->get(url);
}

std::string Positions::iconFallback(int r, int c) const {
  return c == ICON ? iconSymbolOf(*snapshot_, row(r).position.instrument) : std::string();
}

std::string Positions::symbolOf(int r) const {
  if (const auto edge = priceEdge(r)) return snapshot_->symbol(*edge);
  return snapshot_->symbol(row(r).position.instrument);
}

double Positions::change(int r) const {
  const auto edge = priceEdge(r);
  return edge ? periodView(*snapshot_, *edge, period_, false).change() : kUndefined;
}

std::string Positions::text(int r, int c) const {
  const portfolio::PositionValue & v = row(r);
  const portfolio::Position & p = v.position;
  switch (c) {
    case SYMBOL: return symbolOf(r);
    case NAME: {
      if (const auto edge = priceEdge(r))
        if (auto listing = snapshot_->listing(*edge); listing && !listing->name.empty()) return listing->name;
      return snapshot_->name(NodeRef(p.instrument));
    }
    case QUANTITY: return realText(p.quantity);
    case COST: return amount(p.average_cost);
    case PRICE: return v.price ? amount(v.price->close) : "";
    case VALUE:
      if (v.value) return amount(*v.value);
      // No rate: in its own currency, and the code says so.
      return isDefined(v.market_value) ? amount(v.market_value) + " " + currencyCode(*snapshot_, p.quote) : "";
    case WEIGHT: return v.weight ? fixed(*v.weight * 100, 1) + "%" : "";
    case PROFIT: return isDefined(v.profit) ? fixed(v.profit, 2, true) : "";
    case PROFIT_PERCENT: return isDefined(v.profit_percent) ? fixed(v.profit_percent, 2, true) + "%" : "";
    case CHANGE: {
      const double d = change(r);
      return isDefined(d) ? fixed(d, 2, true) + "%" : "";
    }
    default: return "";
  }
}

std::span<const float> Positions::trace(int r, int) const {
  auto it = traces_.find(r);
  if (it == traces_.end()) {
    const auto edge = priceEdge(r);
    it = traces_.emplace(r, edge ? periodView(*snapshot_, *edge, period_, true).closes : std::vector<float>{}).first;
  }
  return it->second;
}

std::optional<cellui::Color> Positions::color(int r, int c) const {
  std::optional<bool> up;
  if (c == CHANGE || c == HISTORY) up = trendUp(change(r));
  else if (c == PROFIT || c == PROFIT_PERCENT) up = trendUp(row(r).profit);
  if (!up) return std::nullopt;
  return *up ? cellui::Color(0x3f, 0xb9, 0x50) : cellui::Color(0xf8, 0x51, 0x49);
}

std::optional<double> Positions::number(int r, int c) const {
  const portfolio::PositionValue & v = row(r);
  auto some = [](double d) { return isDefined(d) ? std::optional<double>(d) : std::nullopt; };
  switch (c) {
    case QUANTITY: return v.position.quantity;
    case COST: return v.position.average_cost;
    case PRICE: return v.price ? std::optional<double>(v.price->close) : std::nullopt;
    case VALUE: return v.value ? v.value : some(v.market_value);
    case WEIGHT: return v.weight;
    case PROFIT: return some(v.profit);
    case PROFIT_PERCENT: return some(v.profit_percent);
    case CHANGE: return some(change(r));
    default: return std::nullopt;
  }
}

std::string Positions::valueTitle() const {
  return "Value " + (valuation_.currency.valid() ? currencyCode(*snapshot_, valuation_.currency) : std::string()) + (valuation_.incomplete ? "*" : "");
}

std::string Positions::summary() const {
  const size_t n = valuation_.rows.size();
  std::string out = name_.empty() ? "Portfolio" : name_;
  if (n == 0) return out + ": no positions";
  out += ": " + amount(valuation_.total) + " " + currencyCode(*snapshot_, valuation_.currency) + " in " + std::to_string(n) + (n == 1 ? " position" : " positions");
  size_t missing = 0;
  for (const auto & v : valuation_.rows) missing += !v.value;
  if (valuation_.incomplete) out += " (incomplete: " + std::to_string(missing) + (missing == 1 ? " has" : " have") + " no price or rate)";
  return out;
}
