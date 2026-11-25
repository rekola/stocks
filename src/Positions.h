#pragma once

#include "icons/IconLoader.h"
#include "model/Period.h"
#include "model/Store.h"
#include "portfolio/Portfolio.h"

#include <cellui/Table.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// The positions of one portfolio as a table: what is held of each instrument, at what cost, and what it is worth at the prices
// the store has, in the portfolio's currency (portfolio/Portfolio.h does the arithmetic; this only shows it). A row is a position
// (an instrument on a market in a currency), and an edge of the instrument on that market in that currency with the latest bar is
// its price. A position with no rate to the portfolio's currency shows its value in its own currency, and the total is marked
// incomplete.
class Positions : public cellui::TableModel {
 public:
  enum Column { ICON, SYMBOL, NAME, QUANTITY, COST, PRICE, VALUE, WEIGHT, PROFIT, PROFIT_PERCENT, CHANGE, HISTORY };

  // `portfolio` need not exist (none, or one nothing is said of): the table is empty.
  Positions(std::shared_ptr<const Snapshot> snapshot, PortfolioId portfolio, std::shared_ptr<IconLoader> icons = nullptr);
  // The same portfolio from a newer snapshot: the positions are worked out again, the rows come in the same order.
  void setSnapshot(std::shared_ptr<const Snapshot> snapshot);
  void setPeriod(Period);
  Period period() const { return period_; }
  PortfolioId portfolio() const { return portfolio_; }

  // The currency the value is shown in: the portfolio's `currency` fact, else that of its first position.
  InstrumentId currency() const { return valuation_.currency; }
  const portfolio::Valuation & valuation() const { return valuation_; }
  // "Retirement: 12 345.60 EUR in 3 positions", with "(incomplete: 1 has no rate)" where the total is not all of it.
  std::string summary() const;
  // The text of the Value heading: "Value EUR", with a star where the total is incomplete.
  std::string valueTitle() const;

  int rowCount() const override { return static_cast<int>(valuation_.rows.size()); }
  std::string text(int r, int c) const override;
  std::span<const float> trace(int r, int) const override;
  std::span<const float> traceLight(int r, int) const override;
  std::shared_ptr<const cellui::TableIcon> icon(int r, int c) const override;
  std::string iconFallback(int r, int c) const override;
  std::optional<cellui::Color> color(int r, int c) const override;
  std::optional<double> number(int r, int c) const override;
  // A position is of something that is traded, so every row has all three buttons (the default).

  const portfolio::PositionValue & row(int r) const { return valuation_.rows[static_cast<size_t>(r)]; }
  // What the position is: instrument, market and currency (no dataset). Its identity across a refresh.
  EdgeKey key(int r) const;
  // The edge of its price, which the row's change, trace and history are of; none where no edge has bars.
  std::optional<EdgeKey> priceEdge(int r) const;
  std::string iconUrl(int r) const;

 private:
  double change(int r) const;
  std::string symbolOf(int r) const;

  std::shared_ptr<IconLoader> icons_;
  std::shared_ptr<const Snapshot> snapshot_;
  PortfolioId portfolio_;
  Period period_ = kDefaultPeriod;
  portfolio::Valuation valuation_;
  std::string name_;
  const PeriodView & traceOf(int r) const;  // the row's period view with its trace, kept while it is drawn
  mutable std::unordered_map<int, PeriodView> traces_;
};
