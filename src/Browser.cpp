#include "Browser.h"

#include "AttributeView.h"
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
                              {"Indices", "show-indices"}, {"All", "show-all"}};

// What the buttons at the end of a row do: a glyph and the command that runs for the selected row. A new action is a new line.
struct RowAction {
  const char * glyph;
  const char * command;
};
constexpr RowAction kRowActions[] = {{kInfoGlyph, "show-info"}};

// The edges of a preset: a market's, or everything.
std::vector<EdgeKey> edgesOf(const Snapshot & snapshot, const Preset & preset) {
  if (std::string(preset.tag) == "All") return allQuoteEdges(snapshot);
  const std::optional<MarketId> market = snapshot.findMarket(preset.tag);
  return market ? quoteEdges(snapshot, *market) : std::vector<EdgeKey>{};
}

std::vector<ButtonFace> buttonGlyphs() {
  std::vector<ButtonFace> glyphs;
  for (const RowAction & action : kRowActions) glyphs.push_back(action.glyph);
  return glyphs;
}

// Wide enough for "Link: Full speed, 480 kB/s, RTT 38 ms (pinned)".
constexpr int kLinkCols = 52;

}  // namespace

Browser::Browser(Application & app, std::shared_ptr<Store> store, std::shared_ptr<IconLoader> icons,
                 std::shared_ptr<HistoryLoader> history)
    : app_(app), store_(std::move(store)), icons_(std::move(icons)), history_(std::move(history)), tags_(app.root()), periods_(app.root()), table_(app.root(), app.charts()), info_(app.root()), link_(app.root()), status_(app.root()) {
  tags_.attach(app.widgets());
  periods_.attach(app.widgets());
  table_.attach(app.widgets());
  info_.attach(app.widgets());
  link_.attach(app.widgets());
  status_.attach(app.widgets());
  std::vector<std::string> tags;
  for (const Preset & preset : kPresets) tags.push_back(preset.tag);
  tags_.setTags(std::move(tags));
  std::vector<std::string> periods;
  for (size_t i = 0; i < kPeriodCount; i++) periods.push_back(info(static_cast<Period>(i)).label);
  periods_.setTags(std::move(periods));
  info_.setColors(&StyleProvider::info_line_fg_color, &StyleProvider::info_line_bg_color);
  link_.setColors(&StyleProvider::info_line_fg_color, &StyleProvider::info_line_bg_color);
  link_.setStyle({TextFont::PROPORTIONAL, TextOverflow::ELLIPSIS, kPlainFace, TextAlign::RIGHT});
  setColumns();
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
  // A button's command acts on the row it was clicked on; the selection stays where it was.
  button_ = table_.button_clicked.connect([this](int row, int, int button) {
    if (button < 0 || button >= static_cast<int>(std::size(kRowActions))) return;
    action_row_ = row;
    app_.execute(kRowActions[button].command);
    action_row_.reset();
  });
  period_tag_ = periods_.chosen.connect([this](int i) {
    if (i >= 0) showPeriod(static_cast<Period>(i));
  });
  periods_.select(static_cast<int>(period_));
  tag_ = tags_.chosen.connect([this](int i) {
    if (i < 0) return;
    showPreset(i);
    table_.select(0);
    status_.setMessage(std::string("Showing ") + kPresets[i].tag);
  });
  changed_ = table_.on<StoreChanged>([this](const StoreChanged &, EventContext &) {
    requestRefresh();
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
    if (pending_) requestRefresh();
    if (table_.tick(seconds) || pending_) app_.requestFrame();
  });
}

void Browser::setColumns() {
  table_.setColumns({{.title = "", .width = 2, .sortable = false, .icon = true},
                     {.title = "Base", .width = 8},
                     {.title = "Quote", .width = 6},
                     {.title = "Name", .width = 0},
                     {.title = "Market", .width = 16},
                     {.title = "Dataset", .width = 10},
                     {.title = "Price", .width = 10, .align = Align::RIGHT},
                     {.title = changeTitle(period_), .width = 12, .align = Align::RIGHT},
                     {.title = info(period_).trace, .width = 22, .sortable = false, .trace = true},
                     {.title = "", .buttons = buttonGlyphs()}});
}

void Browser::showPeriod(Period period) {
  if (period == period_) return;
  period_ = period;
  periods_.select(static_cast<int>(period_));  // the tag, where there are tags (its own signal finds the period shown)
  if (auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model())) quotes->setPeriod(period_);
  setColumns();  // the headings; the sort and the scroll position stay
  table_.modelChanged();
  status_.setMessage(std::string("Showing ") + info(period_).trace);
  app_.requestFrame();
}

void Browser::showPreset(int i) {
  auto snapshot = store_->snapshot();
  auto quotes = std::make_shared<Quotes>(snapshot, edgesOf(*snapshot, kPresets[i]), icons_);
  quotes->setPeriod(period_);
  table_.setModel(quotes);
  shown_version_ = snapshot->version();
  shown_preset_ = i;
  last_refresh_ = std::chrono::steady_clock::now();
}

void Browser::requestRefresh() {
  const auto now = std::chrono::steady_clock::now();
  // A refresh lists every row again and sorts them (tens of milliseconds at 30 000 rows) and data keeps arriving for minutes: wait
  // at least ten times what the last one cost, so it takes a tenth of the time at most. A test that sets no wait gets none.
  const double wait = min_refresh_ > 0 ? std::max(min_refresh_, 10 * refresh_cost_) : 0;
  if (std::chrono::duration<double>(now - last_refresh_).count() < wait) {
    pending_ = true;  // the next frame asks again
    return;
  }
  pending_ = false;
  last_refresh_ = now;
  refresh();
  refresh_cost_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - now).count();
}

void Browser::requestIcons() {
  auto * quotes = dynamic_cast<const Quotes *>(table_.model().get());
  if (!icons_ || !quotes) return;
  const int count = quotes->rowCount(), first = table_.firstVisiblePosition(), view = std::max(1, table_.visibleRows());
  std::vector<std::string> urls;
  auto add = [&](int position) {
    const int row = table_.rowAt(position);
    if (row < 0) return;
    std::string url = quotes->iconUrl(row);
    if (!url.empty()) urls.push_back(std::move(url));
  };
  const int margin = view;
  for (int p = first; p < std::min(count, first + view + margin); p++) add(p);  // on screen, then the screens after
  for (int p = first - 1; p >= std::max(0, first - margin); p--) add(p);        // then the ones before
  icons_->want(urls);
}

void Browser::requestHistory() {
  auto * quotes = dynamic_cast<const Quotes *>(table_.model().get());
  if (!history_ || !quotes) return;
  const int count = quotes->rowCount(), first = table_.firstVisiblePosition(), view = std::max(1, table_.visibleRows());
  const HistoryView now{quotes, first, view, count, period_, true};
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
  history_->want(edges, period_);
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
    // Most changes are new bars for rows already listed: then the rows stay as they are, and with no sort the order too, so the
    // table is drawn again and nothing is sorted or put in order again.
    // The rows keep their places whatever the order edgesOf() gives: a row that appears goes last.
    const bool same_rows = !quotes->follow(snapshot, edgesOf(*snapshot, kPresets[preset]));
    shown_version_ = snapshot->version();
    if (same_rows && table_.sortColumn() < 0) table_.render();
    else table_.modelChanged();
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
  if (row >= 0) table_.reselect(row);  // the view stays where it is
}

// The tags on the top row (one cell in), a gap, the table, the info line and the status line.
void Browser::layout(int rows, int cols) {
  layoutRows(0, 0, rows, cols, {{.size = 1}, {.size = 1}, {.widget = &table_, .fill = 1}, {.size = 1}, {.widget = &status_}});
  // The period tags have the right end of the top row, where there is room beside the presets.
  const int periodCols = std::min(periods_.preferredWidth(), cols);
  const bool room = 1 + tags_.preferredWidth() + 1 + periodCols <= cols;
  layoutColumns(0, 0, 1, cols, {{.size = 1}, {.widget = &tags_, .fill = 1}, {.widget = &periods_, .size = room ? periodCols : 1}});  // not 0: a bar of no width is not drawn again
  // Where there is no room the bar has no tags at all (the headings still name the period).
  if (room != !periods_.tags().empty()) {
    std::vector<std::string> labels;
    for (size_t i = 0; room && i < kPeriodCount; i++) labels.push_back(info(static_cast<Period>(i)).label);
    periods_.setTags(std::move(labels));
    if (room) periods_.select(static_cast<int>(period_));
  }
  layoutColumns(rows - 2, 0, 1, cols, {{.widget = &info_, .fill = 1}, {.widget = &link_, .size = std::min(kLinkCols, cols / 2)}});
}

void Browser::showLink() { link_.setText(linkStatusText(app_.backend().linkStatus()) + " "); }

void Browser::showHelp() {
  info_.setText(" stocks.  " + app_.bindingFor("command-palette") + ": commands, click a heading: sort, Tab: switch focus, " +
                app_.bindingFor("quit") + ": quit");
}

void Browser::showInfo() {
  auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
  const int row = action_row_.value_or(table_.selectedRow());
  if (!quotes || row < 0) {
    status_.setMessage("No row selected");
    return;
  }
  const EdgeKey edge = quotes->edge(row);
  auto snapshot = store_->snapshot();
  std::string label = snapshot->symbol(edge);
  if (label.empty()) label = snapshot->name(edge.base);
  std::string title = "Info: " + label;
  AttributeModal modal(app_.widgets(), app_.root(), app_.backend().eventLoop(), std::move(title), snapshot, buildInfo(*snapshot, edge));
  // The input that got us here does not return until the modal closes, so no frame follows it: ask for the one that shows
  // the new selection and the dialog.
  app_.requestFrame();
  modal.showModal();
  app_.requestFrame();
}

void Browser::defineCommands() {
  CommandRegistry & commands = app_.commands();
  for (size_t i = 0; i < std::size(kPresets); i++) {
    commands.define(kPresets[i].command, std::string("Show the ") + kPresets[i].tag + " preset", [this, i] { tags_.select(static_cast<int>(i)); });
  }
  const int last = static_cast<int>(std::size(kPresets)) - 1;
  commands.define("next-preset", "Show the next preset", [this, last] { tags_.select(std::min(tags_.selected() + 1, last)); });
  commands.define("previous-preset", "Show the previous preset", [this] { tags_.select(std::max(tags_.selected() - 1, 0)); });
  for (size_t i = 0; i < kPeriodCount; i++) {
    const PeriodInfo & p = info(static_cast<Period>(i));
    commands.define(std::string("show-period-") + p.id, std::string("Show the change and the trace over ") + p.trace, [this, i] { showPeriod(static_cast<Period>(i)); });
  }
  commands.define("next-period", "Show the next period in the change and the trace (1d, 1w, 1m, 3m, 1y, then round)",
                  [this] { showPeriod(nextPeriod(period_)); });
  commands.define("previous-period", "Show the previous period in the change and the trace", [this] { showPeriod(previousPeriod(period_)); });
  commands.define("focus-tags", "Move the keyboard to the preset tags", [this] { app_.input().setFocus(&tags_); });
  commands.define("focus-table", "Move the keyboard to the table", [this] { app_.input().setFocus(&table_); });
  commands.define("show-info", "Show everything known about the selected row", [this] { showInfo(); });
  commands.define("quit", "Leave stocks", [this] { app_.quit(); });
}

void Browser::bindKeys() {
  Keymap & keymap = app_.keymap();
  keymap.bind("q", "quit");
  keymap.bind("i", "show-info");
  keymap.bind(KeymapProfile::TERMINAL, "C-x =", "show-info");  // where the point is, as in Emacs
  keymap.bind(KeymapProfile::TERMINAL, "C-x C-c", "quit");
  // The period: ] and [ step through them; a direct choice is C-c and a letter (d w m q y: day, week, month, quarter, year) in the
  // terminal (C-c and a letter is what Emacs leaves to the user), Alt and a digit on the desktop.
  keymap.bind("]", "next-period");
  keymap.bind("[", "previous-period");
  constexpr const char * kLetters = "dwmqy";
  for (size_t i = 0; i < kPeriodCount; i++) {
    const std::string command = std::string("show-period-") + info(static_cast<Period>(i)).id;
    keymap.bind(KeymapProfile::TERMINAL, std::string("C-c ") + kLetters[i], command);
    keymap.bind(KeymapProfile::DESKTOP, "Alt+" + std::to_string(i + 1), command);
  }
  keymap.bind(KeymapProfile::DESKTOP, "Ctrl+Q", "quit");
  assert(keymap.allBoundIn(app_.commands()));
}
