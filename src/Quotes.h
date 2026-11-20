#pragma once

#include <cellui/Table.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// One table row is one edge: what one unit of `base` costs in `quote`.
struct QuoteRow {
  std::string base, quote, name;
  double price, change;  // change in percent
  std::vector<float> history;
};

// A random walk per instrument of preset 0 (commodities), 1 (Nasdaq Helsinki) or 2 (currencies), the same every run.
std::vector<QuoteRow> makeRows(int preset);

// A number with `decimals` places, with a sign when `sign`.
std::string fixed(double v, int decimals, bool sign = false);

// The rows of a preset as a table: base, quote, name, price, change and a trace of the history.
class Quotes : public cellui::TableModel {
 public:
  explicit Quotes(std::vector<QuoteRow> r) : rows(std::move(r)) {}
  std::vector<QuoteRow> rows;
  int rowCount() const override { return static_cast<int>(rows.size()); }
  std::string text(int r, int c) const override {
    const QuoteRow & i = rows[static_cast<size_t>(r)];
    switch (c) {
      case 0: return i.base;
      case 1: return i.quote;
      case 2: return i.name;
      case 3: return fixed(i.price, 2);
      case 4: return fixed(i.change, 2, true) + "%";
      default: return "";
    }
  }
  std::vector<float> trace(int r, int) const override { return rows[static_cast<size_t>(r)].history; }
  std::optional<cellui::Color> color(int r, int c) const override {
    const QuoteRow & i = rows[static_cast<size_t>(r)];
    if (c != 4 && c != 5) return std::nullopt;
    bool up = c == 4 ? i.change >= 0 : i.history.back() >= i.history.front();
    return up ? cellui::Color(0x3f, 0xb9, 0x50) : cellui::Color(0xf8, 0x51, 0x49);
  }
};

