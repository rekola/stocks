#include "Browser.h"

#include "Quotes.h"
#include "source/Source.h"

#include <cellui/Layout.h>

#include <algorithm>
#include <cassert>

using namespace cellui;

namespace {

struct Preset {
  const char * tag;
  const char * command;
};
constexpr Preset kPresets[] = {{"Commodities", "show-commodities"}, {"Nasdaq Helsinki", "show-nasdaq-helsinki"}, {"Currencies", "show-currencies"},
                              {"Indices", "show-indices"}};

// Wide enough for "Link: Full speed, 480 kB/s, RTT 38 ms (pinned)".
constexpr int kLinkCols = 52;

}  // namespace

Browser::Browser(Application & app, std::shared_ptr<Store> store, std::shared_ptr<IconLoader> icons,
                 std::shared_ptr<HistoryLoader> history)
    : app_(app), store_(std::move(store)), icons_(std::move(icons)), history_(std::move(history)), tags_(app.root()), table_(app.root(), app.charts()), info_(app.root()), link_(app.root()), status_(app.root()) {
  tags_.attach(app.widgets());
  table_.attach(app.widgets());
  info_.attach(app.widgets());
  link_.attach(app.widgets());
  status_.attach(app.widgets());
  std::vector<std::string> tags;
  for (const Preset & preset : kPresets) tags.push_back(preset.tag);
  tags_.setTags(std::move(tags));
  info_.setColors(&StyleProvider::info_line_fg_color, &StyleProvider::info_line_bg_color);
  link_.setColors(&StyleProvider::info_line_fg_color, &StyleProvider::info_line_bg_color);
  link_.setStyle({TextFont::PROPORTIONAL, TextOverflow::ELLIPSIS, kPlainFace, TextAlign::RIGHT});
  table_.setColumns({{.title = "", .width = 2, .sortable = false, .icon = true},
                     {.title = "Base", .width = 8},
                     {.title = "Quote", .width = 6},
                     {.title = "Name", .width = 0},
                     {.title = "Dataset", .width = 12},
                     {.title = "Price", .width = 10, .align = Align::RIGHT},
                     {.title = "Change", .width = 9, .align = Align::RIGHT},
                     {.title = "60 days", .width = 22, .sortable = false, .trace = true}});
  defineCommands();
  bindKeys();
  showHelp();
  showLink();
  layout(app.rows(), app.cols());
  resized_ = app.resized.connect([this](int rows, int cols) { layout(rows, cols); });
  // The keymap profile (the help's key names) and the theme's colours come with the display.
  display_ = app.display_changed.connect([this] {
    showHelp();
    info_.render();
    showLink();
  });
  // A change of the link shows in the info line, so a frame is wanted.
  app.backend().setLinkHandler([this](const LinkStatus &) {
    showLink();
    app_.requestFrame();
  });
  tag_ = tags_.chosen.connect([this](int i) {
    if (i < 0) return;
    showPreset(i);
    table_.select(0);
    status_.setMessage(std::string("Showing ") + kPresets[i].tag);
  });
  changed_ = table_.on<StoreChanged>([this](const StoreChanged &, EventContext &) {
    refresh();
    app_.requestFrame();  // the data came from another thread, not from an input
    return Handled::no;
  });
  // An icon arrived on the loader's thread: draw the table again where it is (no new model, so the scroll position, the
  // selection and the sort stay).
  loaded_ = table_.on<IconsLoaded>([this](const IconsLoaded &, EventContext &) {
    table_.render();
    app_.requestFrame();
    return Handled::no;
  });
  said_ = table_.on<SourceMessage>([this](const SourceMessage & m, EventContext &) {
    status_.setMessage(m.text);
    app_.requestFrame();
    return Handled::no;
  });
  message_ = app.message.connect([this](const std::string & text) { status_.setMessage(text); });
  tags_.select(0);
  app.input().setFocus(&table_);
  // The table eases to a scroll position where the display scrolls smoothly: it moves with each frame, and asks for the next.
  frame_ = app.frame_started.connect([this](double seconds) {
    requestIcons();
    requestHistory();
    if (table_.tick(seconds)) app_.requestFrame();
  });
}

void Browser::showPreset(int i) {
  auto snapshot = store_->snapshot();
  std::optional<Market> market = snapshot->findMarket(kPresets[i].tag);
  table_.setModel(std::make_shared<Quotes>(market ? makeRows(*snapshot, market->id) : std::vector<QuoteRow>{}, icons_));
  shown_version_ = snapshot->version();
  shown_preset_ = i;
}

void Browser::requestIcons() {
  auto * quotes = dynamic_cast<const Quotes *>(table_.model().get());
  if (!icons_ || !quotes) return;
  const int count = quotes->rowCount(), first = table_.firstVisiblePosition(), view = std::max(1, table_.visibleRows());
  std::vector<std::string> urls;
  auto add = [&](int position) {
    const int row = table_.rowAt(position);
    if (row >= 0 && !quotes->rows[static_cast<size_t>(row)].icon_url.empty()) urls.push_back(quotes->rows[static_cast<size_t>(row)].icon_url);
  };
  const int margin = 3 * view;
  for (int p = first; p < std::min(count, first + view + margin); p++) add(p);  // on screen, then the screens after
  for (int p = first - 1; p >= std::max(0, first - margin); p--) add(p);        // then the ones before
  icons_->want(urls);
}

void Browser::requestHistory() {
  auto * quotes = dynamic_cast<const Quotes *>(table_.model().get());
  if (!history_ || !quotes) return;
  const int count = quotes->rowCount(), first = table_.firstVisiblePosition(), view = std::max(1, table_.visibleRows());
  const HistoryView now{quotes, first, view, count};
  if (now == history_view_) return;  // the same rows as the last time: the loader has them
  history_view_ = now;
  std::vector<EdgeKey> edges;
  auto add = [&](int position) {
    const int row = table_.rowAt(position);
    if (row >= 0) edges.push_back(quotes->edge(row));
  };
  // Each is a request a second: what is on screen, then the screen after and the one before.
  for (int p = first; p < std::min(count, first + 2 * view); p++) add(p);
  for (int p = first - 1; p >= std::max(0, first - view); p--) add(p);
  history_->want(edges);
}

void Browser::refresh() {
  const int preset = tags_.selected();
  auto snapshot = store_->snapshot();
  if (preset < 0 || snapshot->version() == shown_version_) return;
  std::optional<EdgeKey> selected;
  auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
  if (quotes && table_.selectedRow() >= 0) selected = quotes->edge(table_.selectedRow());

  if (quotes && preset == shown_preset_) {
    // The same preset with new data: change the rows in place, which keeps the scroll position, the sort and the selection
    // (setModel would put the table back at the top).
    std::optional<Market> market = snapshot->findMarket(kPresets[preset].tag);
    quotes->rows = market ? makeRows(*snapshot, market->id) : std::vector<QuoteRow>{};
    shown_version_ = snapshot->version();
    table_.modelChanged();
  } else {
    const int column = table_.sortColumn();
    const SortOrder order = table_.sortOrder();
    showPreset(preset);
    if (column >= 0) table_.sortBy(column, order);
    quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
  }
  // The row that was selected is selected still, wherever it is now.
  int row = quotes->rowCount() > 0 ? 0 : -1;
  for (int r = 0; selected && r < quotes->rowCount(); r++)
    if (quotes->edge(r) == *selected) row = r;
  if (row >= 0) table_.select(row);
}

// The tags on the top row (one cell in), a gap, the table, the info line and the status line.
void Browser::layout(int rows, int cols) {
  layoutRows(0, 0, rows, cols,
             {{.widget = &tags_, .margin_start = 1}, {.size = 1}, {.widget = &table_, .fill = 1}, {.size = 1}, {.widget = &status_}});
  layoutColumns(rows - 2, 0, 1, cols, {{.widget = &info_, .fill = 1}, {.widget = &link_, .size = std::min(kLinkCols, cols / 2)}});
}

void Browser::showLink() { link_.setText(linkStatusText(app_.backend().linkStatus()) + " "); }

void Browser::showHelp() {
  info_.setText(" stocks.  " + app_.bindingFor("command-palette") + ": commands, click a heading: sort, Tab: switch focus, " +
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
