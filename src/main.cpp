// The stock browser. For now: cellui's Table, TagBar and trace chart over
// made-up rows (there is no data model yet).
//   stocks           full screen in the terminal
//   stocks --gtk     in a GTK window (when cellui is built with it)
//   stocks --plain   one frame as text
// Keys: arrows, PgUp/PgDn, Home/End move; < and > pick the sort column, r
// reverses it; Tab moves between the tags and the table; q quits. Clicking a
// heading sorts, clicking a tag or row chooses it, the wheel scrolls.
#include <cellui/Backend.h>
#include <cellui/BufferPlane.h>
#include <cellui/InputDispatcher.h>
#include <cellui/Table.h>
#include <cellui/TagBar.h>
#ifdef CELLUI_HAS_GTK
#include <cellui/gtk/Window.h>
#endif
#include <cellui/term/TerminalSession.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace cellui;

struct Instrument {
  std::string symbol, name;
  double price, change;  // change in percent
  std::vector<float> history;
};

// A random walk per instrument, the same every run.
std::vector<Instrument> makeRows(int preset) {
  static const char * names[3][12] = {
    {"Gold", "Silver", "Brent Crude", "WTI Crude", "Copper", "Natural Gas", "Platinum", "Wheat", "Corn", "Coffee", "Sugar", "Cotton"},
    {"Nokia", "Fortum", "Sampo", "Kone", "UPM", "Neste", "Stora Enso", "Wartsila", "Elisa", "Orion", "Nordea", "Metso"},
    {"EUR/USD", "USD/JPY", "GBP/USD", "USD/CHF", "AUD/USD", "USD/CAD", "EUR/SEK", "EUR/GBP", "USD/NOK", "EUR/CHF", "NZD/USD", "USD/CNY"}};
  std::mt19937 rng(static_cast<unsigned>(preset) * 7919u + 1u);
  std::uniform_real_distribution<double> step(-0.03, 0.032), start(1.5, 300.0);
  std::vector<Instrument> rows;
  for (int i = 0; i < 12; i++) {
    Instrument r;
    r.name = names[preset][i];
    r.symbol = r.name.substr(0, 4);
    double p = start(rng);
    for (int d = 0; d < 60; d++) {
      p *= 1.0 + step(rng);
      r.history.push_back(static_cast<float>(p));
    }
    r.price = p;
    r.change = (r.history.back() / r.history[r.history.size() - 2] - 1.0) * 100.0;
    rows.push_back(std::move(r));
  }
  return rows;
}

std::string fixed(double v, int decimals, bool sign = false) {
  char buf[32];
  std::snprintf(buf, sizeof buf, sign ? "%+.*f" : "%.*f", decimals, v);
  return buf;
}

struct Quotes : TableModel {
  explicit Quotes(std::vector<Instrument> r) : rows(std::move(r)) {}
  std::vector<Instrument> rows;
  int rowCount() const override { return static_cast<int>(rows.size()); }
  std::string text(int r, int c) const override {
    const Instrument & i = rows[static_cast<size_t>(r)];
    switch (c) {
      case 0: return i.symbol;
      case 1: return i.name;
      case 2: return fixed(i.price, 2);
      case 3: return fixed(i.change, 2, true) + "%";
      default: return "";
    }
  }
  std::vector<float> trace(int r, int) const override { return rows[static_cast<size_t>(r)].history; }
  std::optional<Color> color(int r, int c) const override {
    const Instrument & i = rows[static_cast<size_t>(r)];
    if (c != 3 && c != 4) return std::nullopt;
    bool up = c == 3 ? i.change >= 0 : i.history.back() >= i.history.front();
    return up ? Color(0x3f, 0xb9, 0x50) : Color(0xf8, 0x51, 0x49);
  }
};

class Demo {
 public:
  Demo(const DisplayInfo & display, int rows, int cols, int preset, int sort_column, SortOrder sort_order)
      : display_(display), buffer_(rows, cols), root_plane_(buffer_, styles_), charts_(chartsFor(display)), dispatcher_(root_) {
    root_plane_.setProportionalText(display.proportional_text);
    if (display.pictures && display.cell_px_height > 0 && display.cell_px_width > 0) {
      root_plane_.setCellPixelSize(display.cell_px_height, display.cell_px_width);
      root_plane_.setDisplayScale(display.display_scale);
      root_plane_.setHdrPictures(display.hdr_pictures);
    }
    tags_ = std::make_unique<TagBar>(root_plane_);
    tags_->attach(root_);
    tags_->setTags({"Commodities", "Nasdaq Helsinki", "Currencies"});
    tags_->resize(1, cols);
    tags_->move(0, 1);
    table_ = std::make_unique<Table>(root_plane_, *charts_);
    table_->attach(root_);
    // A taller row gives the pictures room; in cells it is still one row of braille.
    table_->setRowHeight(display.pictures ? 2 : 1);
    table_->setColumns({{"Symbol", 8, Align::LEFT, true, false},
                        {"Name", 0, Align::LEFT, true, false},
                        {"Price", 10, Align::RIGHT, true, false},
                        {"Change", 9, Align::RIGHT, true, false},
                        {"60 days", 22, Align::LEFT, false, true}});
    table_->resize(std::max(2, rows - 3), cols);
    table_->move(2, 0);
    tag_connection_ = tags_->chosen.connect([this](int i) { showPreset(i); });
    tags_->select(preset);
    if (sort_column >= 0) table_->sortBy(sort_column, sort_order);
    dispatcher_.setFocus(table_.get());
    table_->render();
    tags_->render();
  }

  int preset() const { return tags_->selected(); }
  int sortColumn() const { return table_->sortColumn(); }
  SortOrder sortOrder() const { return table_->sortOrder(); }

  void showPreset(int i) {
    if (i < 0) return;
    table_->setModel(std::make_shared<Quotes>(makeRows(i)));
    table_->select(0);
  }

  // Returns false when the demo should end.
  bool offer(const Input & in) {
    if (in.getKind() != Input::Kind::RELEASE && in.getId() == 'q' && !in.hasCtrl()) return false;
    if (in.getKind() != Input::Kind::RELEASE && in.getId() == Key::Tab) dispatcher_.setFocus(dispatcher_.focused() == table_.get() ? static_cast<Widget *>(tags_.get()) : table_.get());
    else dispatcher_.offer(in);
    return true;
  }

  // The frame as cells with its pictures and what a display needs besides.
  void present(Backend & backend) {
    status();
    root_plane_.refresh();
    FrameInfo info;
    info.text_runs = root_plane_.takeTextRuns();
    info.borders = root_plane_.takeBorders();
    dispatcher_.setHitAreas(root_plane_.takeHitAreas());
    backend.present(buffer_, {}, root_plane_.images(), &info);
  }

  std::string text() {
    status();
    root_plane_.refresh();
    return buffer_.toPlainText();
  }

 private:
  void status() {
    std::string line = " stocks - made-up data.  ";
    line += "Click a heading or press < > r to sort, Tab switches focus, q quits";
    CellBuffer::Style bar;
    bar.fg = styles_.theme.info_line_fg;
    bar.bg = styles_.theme.info_line_bg;
    int rows = buffer_.rows(), cols = buffer_.cols();
    buffer_.write(rows - 1, 0, std::string(static_cast<size_t>(cols), ' '), bar);
    buffer_.write(rows - 1, 0, Plane::cutToCells(line, cols, TextOverflow::ELLIPSIS), bar);
  }

  DisplayInfo display_;
  StyleProvider styles_;
  CellBuffer buffer_;
  BufferPlane root_plane_;
  std::unique_ptr<Charts> charts_;
  Widget root_;
  InputDispatcher dispatcher_;
  std::unique_ptr<TagBar> tags_;
  std::unique_ptr<Table> table_;
  Connection tag_connection_;
};

// Drives a demo on a backend until it ends.
int run(Backend & backend) {
  auto make = [&](int preset, int sort_column, SortOrder order) {
    return std::make_unique<Demo>(backend.display(), backend.rows(), backend.cols(), preset, sort_column, order);
  };
  auto demo = make(0, -1, SortOrder::NONE);
  auto & loop = backend.eventLoop();
  backend.setFrameHandler([&](int64_t) { demo->present(backend); });
  loop.addSource({backend.inputFd(), backend.wakeFd()}, [&] {
    for (const BackendEvent & e : backend.poll()) {
      switch (e.type) {
        case BackendEvent::Type::CLOSE: loop.quit(); break;
        case BackendEvent::Type::INPUT:
          if (!demo->offer(e.input)) loop.quit();
          break;
        case BackendEvent::Type::RESIZE:
        case BackendEvent::Type::DISPLAY: demo = make(demo->preset(), demo->sortColumn(), demo->sortOrder()); break;
        default: break;
      }
    }
    backend.requestFrame();
  });
  backend.requestFrame();
  loop.run();
  return 0;
}

}  // namespace

int main(int argc, char ** argv) {
  bool window = false, plain = false;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--plain")) plain = true;
    else if (!std::strcmp(argv[i], "--gtk")) window = true;
    else {
      std::fprintf(stderr, "usage: %s [--gtk | --plain]\n", argv[0]);
      return 2;
    }
  }
  if (plain) {
    Demo demo(DisplayInfo{}, 20, 100, 1, 2, SortOrder::DESCENDING);
    std::fputs(demo.text().c_str(), stdout);
    return 0;
  }
  std::string error;
#ifdef CELLUI_HAS_GTK
  if (window) {
    auto w = gtk::Window::open({}, &error);
    if (!w) {
      std::fprintf(stderr, "stocks: %s\n", error.c_str());
      return 1;
    }
    return run(*w);
  }
#else
  if (window) {
    std::fprintf(stderr, "stocks: cellui was built without GTK\n");
    return 1;
  }
#endif
  auto session = term::TerminalSession::open(term::SessionOptions(), &error);
  if (!session) {
    std::fprintf(stderr, "stocks: %s\n", error.c_str());
    return 1;
  }
  return run(*session);
}
