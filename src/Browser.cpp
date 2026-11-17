#include "Browser.h"

#include "CommandDescriptions.h"
#include "Quotes.h"

#include <cellui/InputDispatcher.h>
#include <cellui/KeyStroke.h>
#include <cellui/KeyStrokeNames.h>

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
  int column;
};
constexpr SortColumn kSortColumns[] = {{"sort-by-base", 0}, {"sort-by-quote", 1}, {"sort-by-name", 2}, {"sort-by-price", 3}, {"sort-by-change", 4}};

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
  table_.setColumns({{"Base", 8, Align::LEFT, true, false},
                     {"Quote", 6, Align::LEFT, true, false},
                     {"Name", 0, Align::LEFT, true, false},
                     {"Price", 10, Align::RIGHT, true, false},
                     {"Change", 9, Align::RIGHT, true, false},
                     {"60 days", 22, Align::LEFT, false, true}});
  defineCommands();
  bindKeys();
  layout(app.rows(), app.cols());
  resized_ = app.resized.connect([this](int rows, int cols) {
    layout(rows, cols);
    layoutPalette();
  });
  display_ = app.display_changed.connect([this] { layout(app_.rows(), app_.cols()); });
  tag_ = tags_.chosen.connect([this](int i) {
    if (i < 0) return;
    table_.setModel(std::make_shared<Quotes>(makeRows(i)));
    table_.select(0);
    status_.setMessage(std::string("Showing ") + kPresets[i].tag);
  });
  tags_.select(0);
  app.input().setFocus(&table_);
  // M-x opens the palette; the palette, and a prompt, take every key while they are open.
  status_.setCommandPaletteCallback([this] { openPalette(); });
  app.setBeforeRefresh([this] { palette_.raise(); });
  app.setInputHook([this](const Input & in) { return offer(in); });
}

std::vector<std::string> Browser::commandNames() const {
  std::vector<std::string> names = commands_.matching("");
  std::sort(names.begin(), names.end());
  return names;
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
  std::string palette = profile() == KeymapProfile::DESKTOP ? "Ctrl+Shift+P" : "M-x";
  info_.putstr(0, 0, Plane::cutToCells(" stocks - made-up data.  " + palette + ": commands, < > r: sort, Tab: switch focus, q: quit", cols, TextOverflow::ELLIPSIS));
}

void Browser::defineCommands() {
  for (size_t i = 0; i < std::size(kPresets); i++) {
    commands_.define(kPresets[i].command, [this, i] { tags_.select(static_cast<int>(i)); });
  }
  commands_.define("next-preset", [this] { tags_.select(std::min(tags_.selected() + 1, static_cast<int>(std::size(kPresets)) - 1)); });
  commands_.define("previous-preset", [this] { tags_.select(std::max(tags_.selected() - 1, 0)); });
  for (const SortColumn & sort : kSortColumns) {
    commands_.define(sort.command, [this, column = sort.column] { table_.sortBy(column, SortOrder::ASCENDING); });
  }
  commands_.define("reverse-sort", [this] {
    if (table_.sortColumn() < 0) {
      status_.setMessage("Not sorted");
      return;
    }
    table_.sortBy(table_.sortColumn(), table_.sortOrder() == SortOrder::ASCENDING ? SortOrder::DESCENDING : SortOrder::ASCENDING);
  });
  commands_.define("clear-sort", [this] { table_.sortBy(-1, SortOrder::NONE); });
  commands_.define("focus-tags", [this] { app_.input().setFocus(&tags_); });
  commands_.define("focus-table", [this] { app_.input().setFocus(&table_); });
  commands_.define("command-palette", [this] { openPalette(); });
  commands_.define("quit", [this] { app_.quit(); });
}

void Browser::bindKeys() {
  using KeyStroke::pack;
  const uint64_t ctrl_x = pack('x', true, false, false, false);
  keymap_.bind(pack('q', false, false, false, false), "quit");
  keymap_.bindPrefixed(KeymapProfile::TERMINAL, ctrl_x, pack('c', true, false, false, false), "quit");
  keymap_.bind(KeymapProfile::DESKTOP, pack('q', true, false, false, false), "quit");
  assert(keymap_.allBoundIn(commands_));
}

bool Browser::offer(const Input & in) {
  const int id = in.getId();
  const bool release = in.getKind() == Input::Kind::RELEASE;
  const bool pointer = id == Key::Motion || (id >= Key::Button1 && id <= Key::Button11);
  if (palette_.isOpen()) {
    // Modal: it takes everything while it is open.
    if (pointer) {
      if (release) return true;
      if (id == Key::Button1) handlePalette(palette_.click(in.getY(), in.getX()));
      else if (id == Key::Button4 || id == Key::Button5) handlePalette(palette_.wheel(id == Key::Button4));
    } else if (!release) {
      handlePalette(palette_.key(InputDispatcher::keyDownFor(in)));
    }
    return true;
  }
  if (pointer) return false;
  if (release) return status_.isReaderActive();
  KeyDown e = InputDispatcher::keyDownFor(in);
  return status_.keyDown(e) || dispatchCommand(e);
}

bool Browser::dispatchCommand(const KeyDown & e) {
  if (e.mods.alt || e.mods.meta) return false;
  const uint64_t stroke = KeyStroke::pack(e.key, e.mods);
  // A started two-key sequence takes the next key, bound or not, so a half-typed prefix never does something else.
  if (pending_prefix_) {
    const uint64_t prefix = *pending_prefix_;
    pending_prefix_.reset();
    if (const std::string * name = keymap_.lookupPrefixed(prefix, stroke, profile())) return commands_.execute(*name);
    status_.setMessage("Undefined key sequence");
    return true;
  }
  if (keymap_.isPrefix(stroke, profile())) {
    pending_prefix_ = stroke;
    status_.setMessage("C-x-");
    return true;
  }
  if (const std::string * name = keymap_.lookup(stroke, profile())) return commands_.execute(*name);
  return false;
}

void Browser::openPalette() {
  // Opening it aborts a prompt, as entering any prompt does.
  status_.cancelReader();
  std::vector<PaletteEntry> entries;
  for (const std::string & name : commandNames()) entries.push_back({name, commandDescription(name), bindingFor(name)});
  palette_.open(std::move(entries));
  layoutPalette();
}

void Browser::layoutPalette() {
  if (palette_.isOpen()) palette_.show(app_.root(), app_.rows(), app_.cols());
}

void Browser::handlePalette(CommandPalette::Result result) {
  if (result == CommandPalette::Result::NONE) {
    layoutPalette();  // the new query or selection
    return;
  }
  std::string name;
  if (result == CommandPalette::Result::CHOSEN && palette_.chosen()) name = palette_.chosen()->name;
  // Closed before the command runs, which may open something of its own.
  palette_.close();
  if (!name.empty() && !commands_.execute(name)) status_.setMessage("Invalid command");
}

std::string Browser::bindingFor(const std::string & name) const {
  if (name == "command-palette") return profile() == KeymapProfile::DESKTOP ? "Ctrl+Shift+P" : "M-x";
  auto sequences = keymap_.sequencesFor(name, profile());
  if (sequences.empty()) return "";
  // The shortest, then the first by value, so the choice is the same every run.
  auto best = std::min_element(sequences.begin(), sequences.end(), [](const auto & a, const auto & b) { return a.size() != b.size() ? a.size() < b.size() : a < b; });
  return KeyStroke::describe(*best, profile());
}
