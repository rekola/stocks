#pragma once

#include "model/Store.h"

#include <cellui/ModalDialog.h>
#include <cellui/ScrollBody.h>

#include <memory>
#include <string>
#include <vector>

// What a portfolio says was done with one position (an instrument on a market in a currency): its transactions as a table, to
// read and check. Nothing here writes.

// The transactions of one position of a portfolio, from the dataset its holdings are derived from (`portfolio::ledgerOf`), in
// the order they are applied (by day; on one day the buys first, then by id). Each row is a transaction: its date, side,
// quantity, price, fee, what it cost or brought in (`total`, the fee included), how many were held after it, and its note.
// A transaction the arithmetic leaves out (a sell of more than is held) says why, and changes nothing that is held.
struct TransactionTable {
  enum Column { DATE, SIDE, QUANTITY, PRICE, FEE, TOTAL, HELD, NOTE, COLUMN_COUNT };
  struct Row {
    std::string cells[COLUMN_COUNT];
    std::string refused;  // why it was left out; empty where it was applied
  };
  std::string currency;  // the code of the currency the price, the fee and the total are in
  std::vector<Row> rows;
  // The column's heading, and whether its cells are numbers (right-aligned).
  static const char * heading(Column);
  static bool numeric(Column);
};
TransactionTable buildTransactionTable(const Snapshot &, PortfolioId, InstrumentId instrument, MarketId market, InstrumentId quote);

// A modal that shows a TransactionTable: a heading row that stays, the rows scrolling under it, and a line under them (what is
// held, at what average cost). Escape closes it.
class TransactionsModal : public cellui::ModalDialog {
 public:
  TransactionsModal(cellui::Widget & root, cellui::Plane & screen, cellui::EventLoop & loop, std::string title, TransactionTable table,
                    std::string summary);

  // The heading and the rows as text, columns two spaces apart, as wide as the last layout made them.
  std::vector<std::string> lines() const;

  bool wantsNativeSelection() const override { return true; }

 protected:
  void layout(cellui::Plane & screen, int rows, int cols) override;
  void keyDown(const cellui::KeyDown & e) override;
  void wheel(const cellui::Wheel & e) override;
  void mouseDown(const cellui::MouseDown & e) override;
  void mouseMove(const cellui::MouseMove & e) override;
  void mouseUp(const cellui::MouseUp & e) override;
  bool animating() const override { return body_.animating(); }
  void close() override {
    body_.close();
    plane_.reset();
  }

 private:
  // The widths of the columns for a box `inner` cells wide: each as wide as its widest cell, the note taking what is left
  // (and at most what it needs).
  void fitColumns(int inner);
  void scrollBy(int delta);

  std::string title_, summary_;
  TransactionTable table_;
  int widths_[TransactionTable::COLUMN_COUNT] = {};
  int body_rows_ = 1;
  cellui::ScrollBody body_;
  std::unique_ptr<cellui::Plane> plane_;
};
