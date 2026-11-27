#include "TransactionsView.h"

#include "AttributeView.h"
#include "Quotes.h"
#include "portfolio/Portfolio.h"

#include <cellui/Input.h>
#include <cellui/Utf8.h>

#include <algorithm>
#include <tuple>

using namespace cellui;

namespace {

constexpr int kGap = 2;
constexpr int kMinNote = 8;
constexpr const char * kHint = "Esc: close  Up/Down, PgUp/PgDn, Home/End: scroll";
const Color kRefusedColor(0xf8, 0x51, 0x49);  // the red of a loss in the Portfolio context

}  // namespace

const char * TransactionTable::heading(Column c) {
  switch (c) {
    case DATE: return "Date";
    case SIDE: return "Side";
    case QUANTITY: return "Quantity";
    case PRICE: return "Price";
    case FEE: return "Fee";
    case TOTAL: return "Total";
    case HELD: return "Held";
    case NOTE: return "Note";
    case COLUMN_COUNT: break;
  }
  return "";
}

bool TransactionTable::numeric(Column c) { return c == QUANTITY || c == PRICE || c == FEE || c == TOTAL || c == HELD; }

TransactionTable buildTransactionTable(const Snapshot & snapshot, PortfolioId id, InstrumentId instrument, MarketId market, InstrumentId quote) {
  TransactionTable out;
  out.currency = snapshot.symbol(quote);
  if (out.currency.empty()) out.currency = snapshot.name(NodeRef(quote));
  std::vector<portfolio::Transaction> mine;
  for (const portfolio::Transaction & t : snapshot.transactions({id, portfolio::ledgerOf(snapshot, id)}))
    if (t.instrument == instrument && t.market == market && t.quote == quote) mine.push_back(t);
  // In the order derivePositions applies them, and left out where it leaves them out: a position's arithmetic is its own.
  std::stable_sort(mine.begin(), mine.end(), [](const portfolio::Transaction & a, const portfolio::Transaction & b) {
    return std::make_tuple(a.day, a.side == portfolio::Side::Sell, a.id) < std::make_tuple(b.day, b.side == portfolio::Side::Sell, b.id);
  });
  const portfolio::Derived derived = portfolio::derivePositions(mine);
  double held = 0;
  for (const portfolio::Transaction & t : mine) {
    const bool buy = t.side == portfolio::Side::Buy;
    const auto refusal = std::find_if(derived.refused.begin(), derived.refused.end(), [&](const portfolio::Refusal & r) { return r.id == t.id; });
    if (refusal == derived.refused.end()) held = std::max(0.0, held + (buy ? t.quantity : -t.quantity));
    TransactionTable::Row row;
    row.cells[TransactionTable::DATE] = dateText(t.day);
    row.cells[TransactionTable::SIDE] = buy ? "Buy" : "Sell";
    row.cells[TransactionTable::QUANTITY] = realText(t.quantity);
    row.cells[TransactionTable::PRICE] = realText(t.price);
    row.cells[TransactionTable::FEE] = t.fee == 0 ? "" : realText(t.fee);
    row.cells[TransactionTable::TOTAL] = fixed(t.quantity * t.price + (buy ? t.fee : -t.fee), 2);
    row.cells[TransactionTable::HELD] = realText(held);
    if (refusal != derived.refused.end()) {
      row.refused = refusal->why;
      row.cells[TransactionTable::HELD] = "-";
      row.cells[TransactionTable::NOTE] = "Left out: " + refusal->why + (t.note.empty() ? "" : " · " + t.note);
    } else {
      row.cells[TransactionTable::NOTE] = t.note;
    }
    out.rows.push_back(std::move(row));
  }
  return out;
}

TransactionsModal::TransactionsModal(Widget & root, Plane & screen, EventLoop & loop, std::string title, TransactionTable table, std::string summary)
    : ModalDialog(root, screen, loop), title_(std::move(title)), summary_(std::move(summary)), table_(std::move(table)) {
  fitColumns(1000);
}

void TransactionsModal::fitColumns(int inner) {
  int fixed_width = 0;
  for (int c = 0; c < TransactionTable::COLUMN_COUNT; c++) {
    const auto column = static_cast<TransactionTable::Column>(c);
    int w = utf8::displayWidth(TransactionTable::heading(column));
    for (const TransactionTable::Row & row : table_.rows) w = std::max(w, utf8::displayWidth(row.cells[c]));
    widths_[c] = w;
    if (column != TransactionTable::NOTE) fixed_width += w + kGap;
  }
  widths_[TransactionTable::NOTE] = std::clamp(inner - fixed_width, std::min(kMinNote, widths_[TransactionTable::NOTE]), widths_[TransactionTable::NOTE]);
}

std::vector<std::string> TransactionsModal::lines() const {
  auto line = [&](auto cell) {
    std::string s;
    for (int c = 0; c < TransactionTable::COLUMN_COUNT; c++) {
      const auto column = static_cast<TransactionTable::Column>(c);
      const std::string text = utf8::truncateToWidth(cell(c), widths_[c]);
      const std::string pad(static_cast<size_t>(std::max(0, widths_[c] - utf8::displayWidth(text))), ' ');
      if (c > 0) s += std::string(kGap, ' ');
      s += TransactionTable::numeric(column) ? pad + text : text + pad;
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
  };
  std::vector<std::string> out{line([](int c) { return std::string(TransactionTable::heading(static_cast<TransactionTable::Column>(c))); })};
  for (const TransactionTable::Row & row : table_.rows) out.push_back(line([&](int c) { return row.cells[c]; }));
  return out;
}

void TransactionsModal::layout(Plane & screen, int rows, int cols) {
  int wanted = 0;
  fitColumns(1000);
  for (int w : widths_) wanted += w + kGap;
  wanted -= kGap;
  wanted = std::max({wanted, utf8::displayWidth(title_), utf8::displayWidth(summary_), utf8::displayWidth(kHint)});
  // A cell of padding on the left, and the scrollbar's column and one of padding on the right.
  const int width = std::min(wanted + 3, std::max(20, cols - 4));
  const int inner = width - 3;
  fitColumns(inner);
  // Padding, title, a gap, the heading, the rows, a gap, the summary, the hint, padding.
  constexpr int kChrome = 8;
  const int count = std::max(1, static_cast<int>(table_.rows.size()));
  const int height = std::min(count + kChrome, std::max(kChrome + 1, rows - 2));
  body_rows_ = std::max(1, height - kChrome);
  body_.setCount(static_cast<int>(table_.rows.size()));

  if (!plane_) {
    plane_ = screen.createChild();
    plane_->setMouseCursor(MouseCursor::DEFAULT);
    plane_->setElevation(screen.getStyles().theme.elevation.modal);
    plane_->setCornerRadius(screen.getStyles().theme.dialog_radius_px);
  }
  plane_->resize(height, width);
  const StyleProvider & styles = plane_->getStyles();
  const Color bg = styles.picker_bg_color, fg = styles.window_fg_color, strong = styles.window_accent_fg_color, quiet = styles.theme.text_secondary;
  auto paint = [&](const Color & foreground) {
    plane_->setFgColor(foreground);
    plane_->setBgColor(bg);
  };
  paint(fg);
  for (int y = 0; y < height; y++) plane_->putstr(y, 0, std::string(static_cast<size_t>(width), ' '));
  paint(strong);
  plane_->putstr(1, 1, utf8::truncateToWidth(title_, width - 2), kBoldFace);

  // The cells of one line, each in its column: numbers to the right.
  auto drawCells = [&](Plane & p, int y, auto cell, const TextFace & face) {
    int x = 1;
    for (int c = 0; c < TransactionTable::COLUMN_COUNT && x <= inner; c++) {
      const auto column = static_cast<TransactionTable::Column>(c);
      const int w = std::min(widths_[c], inner + 1 - x);
      p.putText(y, x, w, cell(c), TextStyle{TextFont::PROPORTIONAL, TextOverflow::ELLIPSIS, face, TransactionTable::numeric(column) ? TextAlign::RIGHT : TextAlign::LEFT});
      x += widths_[c] + kGap;
    }
  };
  paint(quiet);
  drawCells(*plane_, 3, [](int c) { return std::string(TransactionTable::heading(static_cast<TransactionTable::Column>(c))); }, kBoldFace);
  plane_->putstr(height - 3, 1, utf8::truncateToWidth(summary_, width - 2));
  plane_->putstr(height - 2, 1, utf8::truncateToWidth(kHint, width - 2));

  const int top = (rows - height) / 2, left = (cols - width) / 2;
  plane_->move(top, left);
  body_.layout(screen, *plane_, top, left, 4, body_rows_, width - 1, bg, fg, quiet, strong);
  Plane & body = body_.plane();
  if (table_.rows.empty()) {
    body.setFgColor(quiet);
    body.putText(0, 1, inner, "No transactions", TextStyle{TextFont::PROPORTIONAL, TextOverflow::ELLIPSIS, kPlainFace, TextAlign::LEFT});
  }
  for (int i = 0; i < body_.drawnRows(); i++) {
    const size_t at = static_cast<size_t>(body_.firstLine() + i);
    if (at >= table_.rows.size()) break;
    const TransactionTable::Row & row = table_.rows[at];
    body.setFgColor(row.refused.empty() ? fg : kRefusedColor);
    drawCells(body, i, [&](int c) { return row.cells[c]; }, kPlainFace);
  }
  plane_->moveToTop();
  body.moveToTop();
}

void TransactionsModal::scrollBy(int delta) {
  body_.scrollBy(delta);
  relayout();
}

void TransactionsModal::keyDown(const KeyDown & e) {
  const int page = std::max(1, body_rows_ - 1);
  if (e.key == Key::Escape || e.key == 'q' || e.key == Key::Enter || (e.mods.ctrl && e.key == 'g')) done(0);
  else if (e.key == Key::Down || (e.mods.ctrl && e.key == 'n')) scrollBy(1);
  else if (e.key == Key::Up || (e.mods.ctrl && e.key == 'p')) scrollBy(-1);
  else if (e.key == Key::PageDown || (e.mods.ctrl && e.key == 'v')) scrollBy(page);
  else if (e.key == Key::PageUp || (e.mods.alt && e.key == 'v')) scrollBy(-page);
  else if (e.key == Key::Home) body_.home(), relayout();
  else if (e.key == Key::End) body_.end(), relayout();
}

void TransactionsModal::wheel(const Wheel & e) {
  if (e.dy != 0) scrollBy(-e.dy * 3);
}

void TransactionsModal::mouseDown(const MouseDown & e) {
  if (e.button == 1 && body_.press(e.row, e.col)) relayout();
}

void TransactionsModal::mouseMove(const MouseMove & e) {
  if ((e.buttons & 1u) && body_.drag(e.row, e.col)) relayout();
}

void TransactionsModal::mouseUp(const MouseUp & e) {
  if (e.button == 1 && body_.release(e.row, e.col)) relayout();
}
