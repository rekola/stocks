// The stock browser. For now: cellui's Table, TagBar and trace chart over
// made-up rows (there is no data model yet).
//   stocks                 on the display cellui picks
//   stocks --ui terminal   in the terminal;  --ui window  in a window
// Keys: arrows, PgUp/PgDn, Home/End move; < and > pick the sort column, r
// reverses it; Tab moves between the tags and the table; q quits. Clicking a
// heading sorts, clicking a tag or row chooses it, the wheel scrolls.
#include <cellui/Application.h>
#include <cellui/Table.h>
#include <cellui/TagBar.h>

#include <cstdio>
#include <memory>
#include <optional>
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

// The screen: tags on the top row, the table filling what is between them and
// the status line, which are laid out again whenever the display changes size.
class Browser {
 public:
  explicit Browser(Application & app)
      : app_(app), tags_(app.root()), table_(app.root(), app.charts()), status_(app.root()) {
    tags_.attach(app.widgets());
    table_.attach(app.widgets());
    status_.attach(app.widgets());
    tags_.setTags({"Commodities", "Nasdaq Helsinki", "Currencies"});
    table_.setColumns({{"Symbol", 8, Align::LEFT, true, false},
                       {"Name", 0, Align::LEFT, true, false},
                       {"Price", 10, Align::RIGHT, true, false},
                       {"Change", 9, Align::RIGHT, true, false},
                       {"60 days", 22, Align::LEFT, false, true}});
    layout(app.rows(), app.cols());
    resized_ = app.resized.connect([this](int rows, int cols) { layout(rows, cols); });
    display_ = app.display_changed.connect([this] { layout(app_.rows(), app_.cols()); });
    tag_ = tags_.chosen.connect([this](int i) {
      if (i < 0) return;
      table_.setModel(std::make_shared<Quotes>(makeRows(i)));
      table_.select(0);
    });
    tags_.select(0);
    app.input().setFocus(&table_);
    app.setInputHook([this](const Input & in) {
      if (in.getKind() == Input::Kind::RELEASE || in.getId() != 'q' || in.hasCtrl() || in.hasAlt()) return false;
      app_.quit();
      return true;
    });
  }

 private:
  void layout(int rows, int cols) {
    tags_.resize(1, std::max(1, cols - 1));
    tags_.move(0, 1);
    table_.resize(std::max(2, rows - 3), cols);
    table_.move(2, 0);
    status_.resize(1, cols);
    status_.move(rows - 1, 0);
    const StyleProvider & styles = app_.root().getStyles();
    status_.setFgColor(styles.info_line_fg_color);
    status_.setBgColor(styles.info_line_bg_color);
    status_.fill();
    status_.putstr(0, 0, Plane::cutToCells(" stocks - made-up data.  Click a heading or press < > r to sort, Tab switches focus, q quits", cols, TextOverflow::ELLIPSIS));
  }

  Application & app_;
  TagBar tags_;
  Table table_;
  Widget status_;
  Connection resized_, display_, tag_;
};

}  // namespace

int main(int argc, char ** argv) {
  std::string error;
  auto app = cellui::Application::open(argc, argv, &error);
  if (!app) {
    std::fprintf(stderr, "stocks: %s\n", error.c_str());
    return 1;
  }
  if (!app->arguments().empty()) {
    std::fprintf(stderr, "usage: %s [--ui auto|terminal|window] [--theme original|dark|light]\n", argv[0]);
    return 2;
  }
  Browser browser(*app);
  return app->run();
}
