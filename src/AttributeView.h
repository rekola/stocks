#pragma once

#include "model/Store.h"

#include <cellui/ModalDialog.h>
#include <cellui/ScrollBody.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// What is known of one table row, as sections of predicate and value, read from one snapshot.

// A value as the viewer shows it. `link` is the node a value refers to (a portfolio's currency is a currency node): the text is
// that node's name, and the viewer draws it as a link.
struct ShownValue {
  std::string text;
  std::optional<NodeRef> link;
};

// The value as shown: by its alternative (a visitor, so a new alternative of Value does not compile until it is handled
// here), and a whole number of a day predicate ("report-day", "first-trade-day") as a date. A node is shown as the name
// the snapshot says of it (else its symbol, else its key as text).
ShownValue showValue(const Snapshot & snapshot, const std::string & predicate, const Value & value);

// A real number with a point whatever the locale, no trailing zeros ("0.35", "4200000000000").
std::string realText(double v);
// "2024-05-03" of a day number.
std::string dateText(int64_t dayNumber);
// Whether a predicate says a day number.
bool isDayPredicate(const std::string & predicate);

// `text` as lines of at most `cols` cells: broken at whitespace, filled greedily, and a word wider than a line (a URL)
// broken between characters, so all of it shows and can be copied. Always at least one line.
std::vector<std::string> wrapText(const std::string & text, int cols);

struct InfoRow {
  std::string predicate;
  ShownValue value;
};
struct InfoSection {
  std::string title;
  std::vector<InfoRow> rows;
};

// The row's listing, its base instrument, its market (and quote currency, if it says anything) and the series the edge has.
// Reads only what the one row needs.
std::vector<InfoSection> buildInfo(const Snapshot & snapshot, const EdgeKey & edge);

// An item of news: what it says and where it is from, whom it is about (as links), and its text.
std::vector<InfoSection> buildNewsInfo(const Snapshot & snapshot, const NewsItem & item);

// A modal that shows the sections, scrolling; Escape closes it. It holds the snapshot the sections were read from.
class AttributeModal : public cellui::ModalDialog {
 public:
  AttributeModal(cellui::Widget & root, cellui::Plane & screen, cellui::EventLoop & loop, std::string title,
                 std::shared_ptr<const Snapshot> snapshot, std::vector<InfoSection> sections);

  // The lines the dialog scrolls through, as text (what a reader sees: headings, "predicate  value", the rest of a
  // wrapped value indented under it, blank lines), for the width of the last layout.
  std::vector<std::string> lines() const;

  // The display selects and copies the text, not the application.
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
  struct Line {
    enum Kind { HEADING, ROW, BLANK } kind = BLANK;
    std::string predicate, text;
    bool link = false;
    bool continued = false;  // the rest of the value above
  };
  // The dialog's lines for a value column `value_cols` wide: a row's value wrapped onto as many lines as it needs.
  void wrapLines(int value_cols);
  int bodyRows() const { return body_rows_; }
  void scrollBy(int delta);

  std::string title_;
  std::shared_ptr<const Snapshot> snapshot_;
  std::vector<Line> rows_;    // one for each heading, row and gap
  std::vector<Line> lines_;   // what is drawn: rows_ with the long values wrapped; `predicate` is empty on a continuation
  int value_cols_ = -1;       // the width lines_ was wrapped for
  int predicate_width_ = 0;
  int body_rows_ = 1;
  cellui::ScrollBody body_;
  std::unique_ptr<cellui::Plane> plane_;
};
