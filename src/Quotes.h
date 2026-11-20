#pragma once

#include "model/Store.h"

#include <cellui/Table.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// One table row is one edge: what one unit of `base` costs in `quote`, as one dataset reports it on one market.
struct QuoteRow {
  EdgeKey key;
  std::string base, quote, name, dataset;
  double price, change;  // change in percent, over the last interval
  std::vector<float> history;  // recent closes, oldest first
};

// The rows of a market: one per edge that has candles, in the order the store lists them. At most `bars` closes each.
std::vector<QuoteRow> makeRows(const Snapshot & snapshot, MarketId market, size_t bars = 60);

// A number with `decimals` places, with a sign when `sign`.
std::string fixed(double v, int decimals, bool sign = false);

// The rows of a market as a table: base, quote, name, dataset, price, change and a trace of the closes.
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
      case 3: return i.dataset;
      case 4: return fixed(i.price, 2);
      case 5: return fixed(i.change, 2, true) + "%";
      default: return "";
    }
  }
  // The edge a row shows: its identity.
  const EdgeKey & edge(int r) const { return rows[static_cast<size_t>(r)].key; }
  std::vector<float> trace(int r, int) const override { return rows[static_cast<size_t>(r)].history; }
  std::optional<cellui::Color> color(int r, int c) const override {
    const QuoteRow & i = rows[static_cast<size_t>(r)];
    if (c != 5 && c != 6) return std::nullopt;
    bool up = c == 5 ? i.change >= 0 : i.history.back() >= i.history.front();
    return up ? cellui::Color(0x3f, 0xb9, 0x50) : cellui::Color(0xf8, 0x51, 0x49);
  }
};

