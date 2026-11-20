#include "Browser.h"

#include "Quotes.h"

#include <algorithm>
#include <cassert>

using namespace cellui;

namespace {

struct Preset {
  const char * tag;
  const char * command;
};
constexpr Preset kPresets[] = {{"Commodities", "show-commodities"}, {"Nasdaq Helsinki", "show-nasdaq-helsinki"}, {"Currencies", "show-currencies"}};

struct SortColumn {
  const char * command;
  const char * description;
  int column;
};
constexpr SortColumn kSortColumns[] = {{"sort-by-base", "Sort by the base symbol, ascending", 0},
                                       {"sort-by-quote", "Sort by the quote symbol, ascending", 1},
                                       {"sort-by-name", "Sort by the instrument's name, ascending", 2},
                                       {"sort-by-dataset", "Sort by the name of the dataset the row comes from, ascending", 3},
                                       {"sort-by-price", "Sort by the price, ascending", 4},
                                       {"sort-by-change", "Sort by the change over the last interval, ascending", 5}};

}  // namespace

Browser::Browser(Application & app, std::shared_ptr<Store> store)
    : app_(app), store_(std::move(store)), tags_(app.root()), table_(app.root(), app.charts()), info_(app.root()), status_(app.root()) {
  tags_.attach(app.widgets());
  table_.attach(app.widgets());
  info_.attach(app.widgets());
  status_.attach(app.widgets());
  std::vector<std::string> tags;
  for (const Preset & preset : kPresets) tags.push_back(preset.tag);
  tags_.setTags(std::move(tags));
  table_.setColumns({{.title = "Base", .width = 8},
                     {.title = "Quote", .width = 6},
                     {.title = "Name", .width = 0},
                     {.title = "Dataset", .width = 12},
                     {.title = "Price", .width = 10, .align = Align::RIGHT},
                     {.title = "Change", .width = 9, .align = Align::RIGHT},
                     {.title = "60 days", .width = 22, .sortable = false, .trace = true}});
  defineCommands();
  bindKeys();
  layout(app.rows(), app.cols());
  resized_ = app.resized.connect([this](int rows, int cols) { layout(rows, cols); });
  // The theme's colours and the keymap profile (the help's key names) come with the display.
  display_ = app.display_changed.connect([this] { layout(app_.rows(), app_.cols()); });
  tag_ = tags_.chosen.connect([this](int i) {
    if (i < 0) return;
    auto snapshot = store_->snapshot();
    std::optional<Market> market = snapshot->findMarket(kPresets[i].tag);
    table_.setModel(std::make_shared<Quotes>(market ? makeRows(*snapshot, market->id) : std::vector<QuoteRow>{}));
    table_.select(0);
    status_.setMessage(std::string("Showing ") + kPresets[i].tag);
  });
  message_ = app.message.connect([this](const std::string & text) { status_.setMessage(text); });
  tags_.select(0);
  app.input().setFocus(&table_);
  // The table eases to a scroll position where the display scrolls smoothly: it moves with each frame, and asks for the next.
  frame_ = app.frame_started.connect([this](double seconds) {
    if (table_.tick(seconds)) app_.requestFrame();
  });
}

void Browser::layout(int rows, int cols) {
  tags_.resize(1, std::max(1, cols - 1));
  tags_.move(0, 1);
  table_.resize(std::max(2, rows - 4), cols);
  table_.move(2, 0);
  info_.resize(1, cols);
  info_.move(rows - 2, 0);
  status_.resize(1, cols);
  status_.move(rows - 1, 0);
  const StyleProvider & styles = app_.root().getStyles();
  info_.setFgColor(styles.info_line_fg_color);
  info_.setBgColor(styles.info_line_bg_color);
  info_.fill();
  std::string help = " stocks - made-up data.  " + app_.bindingFor("command-palette") + ": commands, < > r: sort, Tab: switch focus, " +
                     app_.bindingFor("quit") + ": quit";
  info_.putstr(0, 0, Plane::cutToCells(help, cols, TextOverflow::ELLIPSIS));
}

void Browser::defineCommands() {
  CommandRegistry & commands = app_.commands();
  for (size_t i = 0; i < std::size(kPresets); i++) {
    commands.define(kPresets[i].command, std::string("Show the ") + kPresets[i].tag + " preset", [this, i] { tags_.select(static_cast<int>(i)); });
  }
  const int last = static_cast<int>(std::size(kPresets)) - 1;
  commands.define("next-preset", "Show the next preset", [this, last] { tags_.select(std::min(tags_.selected() + 1, last)); });
  commands.define("previous-preset", "Show the previous preset", [this] { tags_.select(std::max(tags_.selected() - 1, 0)); });
  for (const SortColumn & sort : kSortColumns) {
    commands.define(sort.command, sort.description, [this, column = sort.column] { table_.sortBy(column, SortOrder::ASCENDING); });
  }
  commands.define("reverse-sort", "Reverse the order of the sorted column", [this] {
    if (table_.sortColumn() < 0) {
      status_.setMessage("Not sorted");
      return;
    }
    table_.sortBy(table_.sortColumn(), table_.sortOrder() == SortOrder::ASCENDING ? SortOrder::DESCENDING : SortOrder::ASCENDING);
  });
  commands.define("clear-sort", "Show the rows in the order the source gave them", [this] { table_.sortBy(-1, SortOrder::NONE); });
  commands.define("focus-tags", "Move the keyboard to the preset tags", [this] { app_.input().setFocus(&tags_); });
  commands.define("focus-table", "Move the keyboard to the table", [this] { app_.input().setFocus(&table_); });
  commands.define("quit", "Leave stocks", [this] { app_.quit(); });
}

void Browser::bindKeys() {
  Keymap & keymap = app_.keymap();
  keymap.bind("q", "quit");
  keymap.bind(KeymapProfile::TERMINAL, "C-x C-c", "quit");
  keymap.bind(KeymapProfile::DESKTOP, "Ctrl+Q", "quit");
  assert(keymap.allBoundIn(app_.commands()));
}
