#include "Browser.h"

#include "Quotes.h"

#include <cellui/Layout.h>

#include <algorithm>
#include <cassert>

using namespace cellui;

namespace {

struct Preset {
  const char * tag;
  const char * command;
};
constexpr Preset kPresets[] = {{"Commodities", "show-commodities"}, {"Nasdaq Helsinki", "show-nasdaq-helsinki"}, {"Currencies", "show-currencies"}};

}  // namespace

Browser::Browser(Application & app)
    : app_(app), tags_(app.root()), table_(app.root(), app.charts()), info_(app.root()), status_(app.root()) {
  tags_.attach(app.widgets());
  table_.attach(app.widgets());
  info_.attach(app.widgets());
  status_.attach(app.widgets());
  std::vector<std::string> tags;
  for (const Preset & preset : kPresets) tags.push_back(preset.tag);
  tags_.setTags(std::move(tags));
  info_.setColors(&StyleProvider::info_line_fg_color, &StyleProvider::info_line_bg_color);
  table_.setColumns({{.title = "Base", .width = 8},
                     {.title = "Quote", .width = 6},
                     {.title = "Name", .width = 0},
                     {.title = "Price", .width = 10, .align = Align::RIGHT},
                     {.title = "Change", .width = 9, .align = Align::RIGHT},
                     {.title = "60 days", .width = 22, .sortable = false, .trace = true}});
  defineCommands();
  bindKeys();
  showHelp();
  layout(app.rows(), app.cols());
  resized_ = app.resized.connect([this](int rows, int cols) { layout(rows, cols); });
  // The keymap profile (the help's key names) and the theme's colours come with the display.
  display_ = app.display_changed.connect([this] {
    showHelp();
    info_.render();
  });
  tag_ = tags_.chosen.connect([this](int i) {
    if (i < 0) return;
    table_.setModel(std::make_shared<Quotes>(makeRows(i)));
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

// The tags on the top row (one cell in), a gap, the table, the info line and the status line.
void Browser::layout(int rows, int cols) {
  layoutRows(0, 0, rows, cols,
             {{.widget = &tags_, .margin_start = 1}, {.size = 1}, {.widget = &table_, .fill = 1}, {.widget = &info_}, {.widget = &status_}});
}

void Browser::showHelp() {
  info_.setText(" stocks - made-up data.  " + app_.bindingFor("command-palette") + ": commands, click a heading: sort, Tab: switch focus, " +
                app_.bindingFor("quit") + ": quit");
}

void Browser::defineCommands() {
  CommandRegistry & commands = app_.commands();
  for (size_t i = 0; i < std::size(kPresets); i++) {
    commands.define(kPresets[i].command, std::string("Show the ") + kPresets[i].tag + " preset", [this, i] { tags_.select(static_cast<int>(i)); });
  }
  const int last = static_cast<int>(std::size(kPresets)) - 1;
  commands.define("next-preset", "Show the next preset", [this, last] { tags_.select(std::min(tags_.selected() + 1, last)); });
  commands.define("previous-preset", "Show the previous preset", [this] { tags_.select(std::max(tags_.selected() - 1, 0)); });
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
