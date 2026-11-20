#pragma once

#include <cellui/Table.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

// One row is one edge: what one unit of `base` costs in `quote`.
struct Instrument {
  std::string base, quote, name;
  double price = 0, change = 0;  // change in percent, over the last day
  std::vector<float> history;
};

// A random walk per instrument of preset 0 (commodities), 1 (Nasdaq Helsinki) or 2 (currencies), the same every run.
std::vector<Instrument> makeRows(int preset);

// A number with `decimals` places and a decimal point whatever the locale, with a sign when `sign`.
std::string fixed(double v, int decimals, bool sign = false);

// The rows of a preset as a table: base, quote, name, price, change and a trace of the history.
class Quotes : public cellui::TableModel {
 public:
  enum Column { BASE, QUOTE, NAME, PRICE, CHANGE, HISTORY };

  explicit Quotes(std::vector<Instrument> r) : rows(std::move(r)) {}
  std::vector<Instrument> rows;
  int rowCount() const override { return static_cast<int>(rows.size()); }
  std::string text(int r, int c) const override;
  std::span<const float> trace(int r, int) const override { return at(r).history; }
  std::optional<cellui::Color> color(int r, int c) const override;
  // Prices and changes sort by their values, not their rounded text.
  std::optional<double> number(int r, int c) const override;

 private:
  const Instrument & at(int r) const { return rows[static_cast<size_t>(r)]; }
};
