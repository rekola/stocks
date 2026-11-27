#include "Browser.h"

#include "AttributeView.h"
#include "TransactionsView.h"
#include "News.h"
#include "Quotes.h"
#include "model/Dates.h"
#include "source/Source.h"

#include <cellui/ChartModal.h>
#include <cellui/PopupMenu.h>
#include <cellui/Layout.h>

#include <algorithm>
#include <cassert>
#include <cmath>

using namespace cellui;

namespace {

// A preset is a selection of edges: a market (by its MIC; the indices, spot
// prices and currency pairs are on no market, XXXX) and, where that holds
// several kinds, the kinds of the base. No mic is every market, of the kind where there is one. A preset with a `list` is none of
// that: it is the edges the user put on that list (kLists, below).
struct Preset {
  const char * tag;
  const char * command;
  const char *mic = nullptr;
  KindSet kinds;
  const char * list = nullptr;
};
constexpr Preset kPresets[] = {
    {"Commodities", "show-commodities", kNoMarket, InstrumentKind::Commodity},
    {"Nasdaq Helsinki", "show-nasdaq-helsinki", "XHEL"},
    {"Currencies", "show-currencies", kNoMarket, {InstrumentKind::Currency, InstrumentKind::Crypto}},  // the coins are money too
    {"Indices", "show-indices", kNoMarket, InstrumentKind::Index},
    {"Funds", "show-funds", nullptr, InstrumentKind::Fund},  // of every market, whoever lists them
    // What is held in hand and traded off exchange: the estimated gold bar and coin now, platinum and scrap gold when they come. Commodities on XOFF.
    {"Physical", "show-physical", kOffExchange, InstrumentKind::Commodity},
    {"All", "show-all"},
    {"Watchlist", "show-watchlist", nullptr, {}, "watchlist"}};

// The lists the user keeps, each a code (what the store keeps its edges under) and a name. A row is put on one from its menu, and the
// list is shown by the preset that names it (kPresets): a new list is a line here and a line there, and the menu, the commands
// (add-to-<code>, remove-from-<code>) and the undo follow.
struct UserList {
  const char * code;
  const char * name;
};
constexpr UserList kLists[] = {{"watchlist", "Watchlist"}};

// A news preset is a selection of the news by what it says under a predicate: its kind (press release, analysis, news), or, with
// `general`, the news that is about nothing in particular.
struct NewsPreset {
  const char * tag;
  const char * command;
  const char * kind = nullptr;
  bool general = false;
};
constexpr NewsPreset kNewsPresets[] = {{"All", "show-all-news"},
                                       {"Press releases", "show-press-releases", kPressRelease},
                                       {"Analyses", "show-analyses", kAnalysis},
                                       {"News", "show-news-stories", kNewsStory},
                                       {"General", "show-general-news", nullptr, true}};

std::function<bool(const NewsItem &)> newsKeep(const NewsPreset & preset) {
  if (preset.general) return [](const NewsItem & n) { return n.about.empty(); };
  if (!preset.kind) return {};
  const std::string kind = preset.kind;
  return [kind](const NewsItem & n) { return newsText(n, predicate::kind) == kind; };
}

// The one button at the end of a row opens the row's menu: the hamburger, three equal lines (U+2261, identical to, in Mathematical
// Operators with the circled plus and minus that were here before), which the monospace fonts terminals and the window use have, one
// cell wide. The trigram (U+2630) is in fewer of them and its lines came with margins of their own where it was not.
// From these many columns the markets table has all its columns (medium), and its roomiest widths (wide); below the first (a terminal of 80) it has
// no high and low columns and narrower ones.
constexpr int kMediumCols = 110, kWideCols = 140;
constexpr const char * kMenuGlyph = "\xE2\x89\xA1";

Quotes::Scope scopeOf(const Snapshot & snapshot, const Preset & preset) {
  if (!preset.mic)
    return {.all = true, .kinds = preset.kinds};
  return {.market = snapshot.findMarket(preset.mic), .kinds = preset.kinds};
}

// The edges of a preset: a list's (those that are rows), a market's, or everything.
std::vector<EdgeKey> edgesOf(const Snapshot & snapshot, const Preset & preset) {
  if (preset.list) {
    std::vector<EdgeKey> keys = snapshot.listMembers(preset.list);
    std::erase_if(keys, [&](const EdgeKey & k) { return !snapshot.listed(k); });
    return keys;
  }
  if (!preset.mic) {
    std::vector<EdgeKey> keys = allQuoteEdges(snapshot);
    std::erase_if(keys, [&](const EdgeKey & k) { return !preset.kinds.admits(snapshot.kind(k.base)); });
    return keys;
  }
  const std::optional<MarketId> market = snapshot.findMarket(preset.mic);
  return market ? quoteEdges(snapshot, *market, preset.kinds)
                : std::vector<EdgeKey>{};
}

// Whether two lists of edges have the same ones, in any order.
bool sameEdges(std::vector<EdgeKey> a, std::vector<EdgeKey> b) {
  if (a.size() != b.size()) return false;
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return a == b;
}

// The room the presets are given beside the period tags before those go: the tags wrap onto lines, and the period tags come only where
// what is left is wide enough to be worth wrapping in.
constexpr int kMinPresetCols = 40;
// The most lines the presets may take, so that the table keeps the screen.
constexpr int kMaxPresetLines = 4;

// Wide enough for "Link: Full speed, 480 kB/s, RTT 38 ms (pinned)".
constexpr int kLinkCols = 52;

}  // namespace

Browser::Browser(Application & app, std::shared_ptr<Store> store, std::shared_ptr<IconLoader> icons,
                 std::shared_ptr<HistoryLoader> history)
    : app_(app), store_(std::move(store)), icons_(std::move(icons)), history_(std::move(history)), tags_(app.root()), periods_(app.root()), table_(app.root(), app.charts()), info_(app.root()), hint_(app.root()), link_(app.root()), status_(app.root()) {
  tags_.attach(app.widgets());
  periods_.attach(app.widgets());
  table_.attach(app.widgets());
  info_.attach(app.widgets());
  hint_.attach(app.widgets());
  link_.attach(app.widgets());
  status_.attach(app.widgets());
  std::vector<std::string> tags;
  for (const Preset & preset : kPresets) tags.push_back(preset.tag);
  tags_.setTags(std::move(tags));
  std::vector<std::string> periods;
  for (size_t i = 0; i < kPeriodCount; i++) periods.push_back(info(static_cast<Period>(i)).label);
  periods_.setTags(std::move(periods));
  info_.setColors(&StyleProvider::info_line_fg_color, &StyleProvider::info_line_bg_color);
  hint_.setVisible(false);
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
  // The menu button opens the menu of the row it was clicked on; the selection stays where it was.
  button_ = table_.button_clicked.connect([this](int row, int, int) { openMenu(row); });
  // A click on a trace cell, or Enter on one, opens its chart.
  chart_click_ = table_.cell_clicked.connect([this](int row, int column) { openChartAt(row, column); });
  chart_enter_ = table_.cell_activated.connect([this](int row, int column) { openChartAt(row, column); });
  period_tag_ = periods_.chosen.connect([this](int i) {
    if (i >= 0) showPeriod(static_cast<Period>(i));
  });
  periods_.select(static_cast<int>(period_));
  tag_ = tags_.chosen.connect([this](int i) {
    if (i < 0 || restoring_) return;
    showTag(i);
    table_.select(0);
    if (context_ == Context::News) status_.setMessage(std::string("Showing ") + kNewsPresets[i].tag + " news");
    else if (context_ == Context::Markets) status_.setMessage(std::string("Showing ") + kPresets[i].tag);
    else if (const auto positions = std::dynamic_pointer_cast<Positions>(table_.model())) status_.setMessage(positions->summary());
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
  if (context_ == Context::News) {
    table_.setColumns({{.title = "Time (UTC)", .width = 16},
                       {.title = "Kind", .width = 14},
                       {.title = "Headline", .width = 0},
                       {.title = "About", .width = 26},
                       {.title = "Source", .width = 9}});
    return;
  }
  if (context_ == Context::Markets) {
    // Three steps by the width of the display (Fit). The longest market labels the app shows are 25 cells ("Spotlight Stock Market DK"; most are under 24) and
    // the dataset names 21 ("European Central Bank"; the rest 13 or less), so the wide ones are cut: no width fits them with a Name left over.
    //   narrow (80)   no High and Low columns (the table has 10): icon 2, Base 7, Quote 5, Market 8, Dataset 8, Price 9, Change 10, trace 10, menu 1 and 9 gaps
    //                 leave the Name 10 cells at 80 columns (the scrollbar has the last one);
    //   medium (110)  all 12: Base 8, Quote 6, Market 10, Dataset 9, Price 9, Change 11, High 9, Low 9, trace 12, menu 1 and 11 gaps leave the Name 12 at 110;
    //   wide (140)    Market 16, Dataset 13 and trace 16: the Name has 28 at 140.
    std::vector<cellui::TableColumn> columns = {{.title = "", .width = 2, .sortable = false, .icon = true, .icon_fallback = true},
                                                {.title = "Base", .width = fit_ == Fit::Narrow ? 7 : 8},
                                                {.title = "Quote", .width = fit_ == Fit::Narrow ? 5 : 6},
                                                {.title = "Name", .width = 0},
                                                {.title = "Market", .width = fit_ == Fit::Narrow ? 8 : fit_ == Fit::Medium ? 10 : 16},
                                                {.title = "Dataset", .width = fit_ == Fit::Narrow ? 8 : fit_ == Fit::Medium ? 9 : 13},
                                                {.title = "Price", .width = 9, .align = Align::RIGHT},
                                                {.title = changeTitle(period_), .width = fit_ == Fit::Narrow ? 10 : 11, .align = Align::RIGHT}};
    if (fit_ != Fit::Narrow) {
      columns.push_back({.title = highTitle(period_), .width = 9, .align = Align::RIGHT});
      columns.push_back({.title = lowTitle(period_), .width = 9, .align = Align::RIGHT});
    }
    columns.push_back({.title = info(period_).trace, .width = fit_ == Fit::Narrow ? 10 : fit_ == Fit::Medium ? 12 : 16, .sortable = false, .trace = true});
    columns.push_back({.title = "", .buttons = {kMenuGlyph}});
    table_.setColumns(std::move(columns));
    return;
  }
  const auto positions = std::dynamic_pointer_cast<Positions>(table_.model());
  table_.setColumns({{.title = "", .width = 2, .sortable = false, .icon = true, .icon_fallback = true},
                     {.title = "Symbol", .width = 7},
                     {.title = "Name", .width = 0},
                     {.title = "Quantity", .width = 8, .align = Align::RIGHT},
                     {.title = "Avg cost", .width = 8, .align = Align::RIGHT},
                     {.title = "Price", .width = 8, .align = Align::RIGHT},
                     {.title = positions ? positions->valueTitle() : "Value", .width = 10, .align = Align::RIGHT},
                     {.title = "Weight", .width = 6, .align = Align::RIGHT},
                     {.title = "P&L", .width = 8, .align = Align::RIGHT},
                     {.title = "P&L %", .width = 7, .align = Align::RIGHT},
                     {.title = changeTitle(period_), .width = 10, .align = Align::RIGHT},
                     {.title = info(period_).trace, .width = 12, .sortable = false, .trace = true},
                     {.title = "", .buttons = {kMenuGlyph}}});
}

void Browser::showPeriod(Period period) {
  if (period == period_) return;
  period_ = period;
  periods_.select(static_cast<int>(period_));  // the tag, where there are tags (its own signal finds the period shown)
  if (auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model())) quotes->setPeriod(period_);
  if (auto positions = std::dynamic_pointer_cast<Positions>(table_.model())) positions->setPeriod(period_);
  setColumns();  // the headings; the sort and the scroll position stay
  table_.modelChanged();
  status_.setMessage(std::string("Showing ") + info(period_).trace);
  app_.requestFrame();
}

void Browser::showPreset(int i) {
  auto snapshot = store_->snapshot();
  auto quotes = std::make_shared<Quotes>(snapshot, edgesOf(*snapshot, kPresets[i]), icons_);
  quotes->setPeriod(period_);
  quotes->setNarrow(fit_ == Fit::Narrow);
  table_.setModel(quotes);
  shown_version_ = snapshot->version();
  shown_preset_ = i;
  last_refresh_ = std::chrono::steady_clock::now();
  showEmptyHint();
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
  auto * positions = dynamic_cast<const Positions *>(table_.model().get());
  if (!icons_ || (!quotes && !positions)) return;
  const int count = table_.model()->rowCount(), first = table_.firstVisiblePosition(), view = std::max(1, table_.visibleRows());
  std::vector<std::string> urls;
  auto add = [&](int position) {
    const int row = table_.rowAt(position);
    if (row < 0) return;
    std::string url = quotes ? quotes->iconUrl(row) : positions->iconUrl(row);
    if (!url.empty()) urls.push_back(std::move(url));
  };
  const int margin = view;
  for (int p = first; p < std::min(count, first + view + margin); p++) add(p);  // on screen, then the screens after
  for (int p = first - 1; p >= std::max(0, first - margin); p--) add(p);        // then the ones before
  icons_->want(urls);
}

void Browser::requestHistory() {
  auto * quotes = dynamic_cast<const Quotes *>(table_.model().get());
  auto * positions = dynamic_cast<const Positions *>(table_.model().get());
  if (!history_ || (!quotes && !positions)) return;
  const int count = table_.model()->rowCount(), first = table_.firstVisiblePosition(), view = std::max(1, table_.visibleRows());
  const int64_t minute = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now().time_since_epoch()).count();
  // Listed every frame (three screens of rows, into a buffer that is kept) and compared with what was sent: a sort or a scroll
  // changes them, a frame that only draws does not.
  std::vector<EdgeKey> & edges = history_scratch_;
  edges.clear();
  auto add = [&](int position) {
    const int row = table_.rowAt(position);
    if (row < 0) return;
    if (quotes) edges.push_back(quotes->edge(row));
    else if (const auto edge = positions->priceEdge(row)) edges.push_back(*edge);  // the edges of the positions on screen
  };
  // Each is a request a second: what is on screen, then the screen after and the one before.
  for (int p = first; p < std::min(count, first + 2 * view); p++) add(p);
  for (int p = first - 1; p >= std::max(0, first - view); p--) add(p);
  if (history_period_ == period_ && minute == history_minute_ && edges == history_edges_) return;  // the same rows as the minute before: the loader has them
  history_period_ = period_;
  history_minute_ = minute;
  std::swap(history_edges_, history_scratch_);
  history_->want(history_edges_, period_);
}

void Browser::refresh() {
  auto snapshot = store_->snapshot();
  if (context_ == Context::Portfolio) {
    if (snapshot->version() != shown_version_) refreshPortfolio(std::move(snapshot));
    return;
  }
  if (context_ == Context::News) {
    if (snapshot->version() != shown_version_) refreshNews(std::move(snapshot));
    return;
  }
  const int preset = tags_.selected();
  if (preset < 0 || snapshot->version() == shown_version_) return;
  std::optional<EdgeKey> selected;
  auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
  if (quotes && table_.selectedRow() >= 0) selected = quotes->edge(table_.selectedRow());

  // A list is a few rows the user chose, so it is looked at whole: the same edges, and it is the same preset with new data.
  std::optional<std::vector<EdgeKey>> members;
  if (kPresets[preset].list) members = edgesOf(*snapshot, kPresets[preset]);
  if (quotes && preset == shown_preset_ && (!members || sameEdges(*members, quotes->edges()))) {
    // The same preset with new data: change the rows in place, which keeps the scroll position, the sort and the selection
    // (setModel would put the table back at the top).
    // Most changes are new bars for rows already listed: then the rows stay as they are, and with no sort the order too, so the
    // table is drawn again and nothing is sorted or put in order again.
    // The rows keep their places and a row that appears goes last: the store says which have, so nothing is listed or compared
    // again unless there are some.
    bool same_rows = true;
    if (members) quotes->setSnapshot(snapshot);
    else same_rows = !(quotes->hasNewRows(*snapshot) ? quotes->addRows(snapshot, scopeOf(*snapshot, kPresets[preset])) : (quotes->setSnapshot(snapshot), false));
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
  showEmptyHint();
}

// The same for the Portfolio context: the portfolios may have changed (one made, renamed or deleted, here or by an undo), and the
// positions are worked out again from the newer snapshot, which keeps the sort, the scroll position and the selected row.
void Browser::refreshPortfolio(std::shared_ptr<const Snapshot> snapshot) {
  const std::optional<EdgeKey> selected = selectedKey();
  const bool moved = syncPortfolioTags(*snapshot, portfolio_);
  auto positions = std::dynamic_pointer_cast<Positions>(table_.model());
  if (moved || !positions || positions->portfolio() != portfolio_) {
    const int column = table_.sortColumn();
    const SortOrder order = table_.sortOrder();
    showPortfolio(tags_.selected());
    if (column >= 0) table_.sortBy(column, order);
    positions = std::dynamic_pointer_cast<Positions>(table_.model());
  } else {
    positions->setSnapshot(snapshot);
    shown_version_ = snapshot->version();
    setColumns();  // the value's heading names the currency, and a star where the total is incomplete
    table_.modelChanged();
  }
  int row = positions && positions->rowCount() > 0 ? 0 : -1;
  if (selected) row = std::max(row, rowOfKey(*selected));
  if (row >= 0) table_.reselect(row);
  showEmptyHint();
}

// The same for the News context: the items again from the newer snapshot, which keeps the sort, the scroll position and the item
// selected.
void Browser::refreshNews(std::shared_ptr<const Snapshot> snapshot) {
  auto news = std::dynamic_pointer_cast<NewsTable>(table_.model());
  if (!news || tags_.selected() != shown_news_) {
    const int column = table_.sortColumn();
    const SortOrder order = table_.sortOrder();
    showNews(tags_.selected());
    if (column >= 0) table_.sortBy(column, order);
    return;
  }
  const std::string selected = selectedNewsKey();
  news->setSnapshot(snapshot);
  shown_version_ = snapshot->version();
  table_.modelChanged();
  int row = news->rowCount() > 0 ? 0 : -1;
  if (!selected.empty()) row = std::max(row, rowOfNewsKey(selected));
  if (row >= 0) table_.reselect(row);
  showEmptyHint();
}

void Browser::showNews(int i) {
  i = std::clamp(i, 0, static_cast<int>(std::size(kNewsPresets)) - 1);
  auto snapshot = store_->snapshot();
  table_.setModel(std::make_shared<NewsTable>(snapshot, newsKeep(kNewsPresets[i])));
  shown_version_ = snapshot->version();
  shown_news_ = i;
  last_refresh_ = std::chrono::steady_clock::now();
  showEmptyHint();
}

std::string Browser::selectedNewsKey() const {
  const auto * news = dynamic_cast<const NewsTable *>(table_.model().get());
  const int row = table_.selectedRow();
  return news && row >= 0 && row < news->rowCount() ? NewsTable::keyOf(news->item(row)) : std::string();
}

int Browser::rowOfNewsKey(const std::string & key) const {
  const auto * news = dynamic_cast<const NewsTable *>(table_.model().get());
  for (int r = 0; news && r < news->rowCount(); r++)
    if (NewsTable::keyOf(news->item(r)) == key) return r;
  return -1;
}

// The tags on the top rows (one cell in), a gap, the table, the info line and the status line. The presets go onto as many lines as they
// need, the period tags at the right end of the first.
void Browser::layout(int rows, int cols) {
  const Fit fit = cols < kMediumCols ? Fit::Narrow : cols < kWideCols ? Fit::Medium : Fit::Wide;
  if (fit != fit_) {
    auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
    const bool hide = fit == Fit::Narrow, moves = quotes && quotes->narrow() != hide;
    int column = table_.sortColumn();
    const SortOrder order = table_.sortOrder();
    if (moves) {
      // The columns the table has change: the sort follows its column, and one by a column that is hidden now is dropped.
      const int own = column < 0 ? -1 : quotes->narrow() && column >= Quotes::HIGH ? column + 2 : column;
      quotes->setNarrow(hide);
      column = own < 0 ? -1 : quotes->tableColumn(own);
    }
    fit_ = fit;
    setColumns();  // the markets table has its columns for the width; the scroll position stays
    if (moves) table_.sortBy(column, column >= 0 ? order : SortOrder::NONE);
    table_.modelChanged();
  }
  // The period tags have the right end of the top row, where there is room beside the presets: all of them on one line, or at
  // least what is worth wrapping in.
  const int periodCols = std::min(periods_.preferredWidth(), cols);
  const bool room = cols - 2 - periodCols >= std::min(tags_.preferredWidth(), kMinPresetCols);
  const int tag_cols = std::max(1, cols - 1 - (room ? periodCols : 1));
  const int lines = std::clamp(tags_.lines(tag_cols), 1, std::max(1, std::min(kMaxPresetLines, rows / 4)));
  layoutRows(0, 0, rows, cols, {{.size = lines}, {.size = 1}, {.widget = &table_, .fill = 1}, {.size = 1}, {.widget = &status_}});
  layoutColumns(0, 0, lines, cols, {{.size = 1}, {.widget = &tags_, .fill = 1}, {.widget = &periods_, .size = room ? periodCols : 1}});  // not 0: a bar of no width is not drawn again
  // Where there is no room the bar has no tags at all (the headings still name the period).
  if (room != !periods_.tags().empty()) {
    std::vector<std::string> labels;
    for (size_t i = 0; room && i < kPeriodCount; i++) labels.push_back(info(static_cast<Period>(i)).label);
    periods_.setTags(std::move(labels));
    if (room) periods_.select(static_cast<int>(period_));
  }
  layoutColumns(lines + 2, 0, 1, cols, {{.widget = &hint_, .fill = 1}});  // the first row under the heading
  layoutColumns(rows - 2, 0, 1, cols, {{.widget = &info_, .fill = 1}, {.widget = &link_, .size = std::min(kLinkCols, cols / 2)}});
}

void Browser::showLink() { link_.setText(linkStatusText(app_.backend().linkStatus()) + " "); }

void Browser::showHelp() {
  info_.setText(" stocks.  " + app_.bindingFor("command-palette") + ": commands, click a heading: sort, Tab: switch context, " +
                app_.bindingFor("quit") + ": quit");
}

namespace {

// When a bar is, for the chart's axis: a date for a daily or longer bar, else the date and the time in UTC (no market calendars yet).
std::string timeLabel(Timestamp at, bool dated) {
  const int64_t day = dayNumberOfInstant(at);
  const Date d = dateOfDayNumber(day);
  char buf[32];
  if (dated) {
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", d.year, d.month, d.day);
  } else {
    const int64_t second = at - (day - kUnixEpochDayNumber) * 86400;
    std::snprintf(buf, sizeof buf, "%02u-%02u %02d:%02d", d.month, d.day, static_cast<int>(second / 3600), static_cast<int>(second % 3600 / 60));
  }
  return buf;
}

// A volume as a short number: 1.23M, 45.6k, 789.
std::string volumeText(double v) {
  if (v >= 1e9) return fixed(v / 1e9, 2) + "G";
  if (v >= 1e6) return fixed(v / 1e6, 2) + "M";
  if (v >= 1e3) return fixed(v / 1e3, 1) + "k";
  return fixed(v, 0);
}

}  // namespace

void Browser::showChart() {
  const int row = action_row_.value_or(table_.selectedRow());
  auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
  auto positions = std::dynamic_pointer_cast<Positions>(table_.model());
  if ((!quotes && !positions) || row < 0) {
    status_.setMessage("No row selected");
    return;
  }
  const std::optional<EdgeKey> found = quotes ? std::optional<EdgeKey>(quotes->edge(row)) : positions->priceEdge(row);
  if (!found) {
    status_.setMessage("No price for this position, so nothing to chart");
    return;
  }
  auto snapshot = store_->snapshot();
  const PeriodView view = periodView(*snapshot, *found, period_, true);
  if (view.closes.size() < 2) {
    status_.setMessage("No history over this period to chart");
    return;
  }
  std::string label = snapshot->symbol(*found);
  if (label.empty()) label = snapshot->name(found->base);
  const bool rate = view.rate;
  cellui::ChartData data;
  data.title = label + "  " + info(period_).trace;
  data.values = view.closes;
  data.light = view.light;  // the volume traded at each point, as how bright the line is there, as in the table
  // The volume is each point's note: read after the value at the cursor.
  for (size_t i = 0; i < view.times.size(); i++) {
    data.labels.push_back(timeLabel(view.times[i], view.dated));
    const bool traded = i < view.volumes.size() && isDefined(view.volumes[i]) && view.volumes[i] > 0;
    data.notes.push_back(traded ? "vol " + volumeText(view.volumes[i]) : std::string());
  }
  // Decimals enough that the labels at the bottom, middle and top of the axis differ: a narrow range (a currency pair) needs more
  // than the usual two (three for a rate).
  int decimals = rate ? 3 : 2;
  if (const auto [low, high] = std::minmax_element(view.closes.begin(), view.closes.end()); low != view.closes.end() && *high > *low) {
    const double step = (static_cast<double>(*high) - static_cast<double>(*low)) / 4;
    decimals = std::clamp(std::max(decimals, static_cast<int>(std::ceil(-std::log10(step)))), 0, 8);
  }
  data.format = [rate, decimals](float v) { return rate ? fixed(v, decimals) + "%" : fixed(v, decimals); };
  // The line is the colour of the period's change, as the table's trace is.
  if (const std::optional<bool> up = trendUp(view.change())) data.color = *up ? cellui::Color(0x3f, 0xb9, 0x50) : cellui::Color(0xf8, 0x51, 0x49);
  cellui::ChartModal chart(app_.widgets(), app_.root(), app_.backend().eventLoop(), app_.charts(), std::move(data));
  // The input that got us here does not return until the chart is closed, so no frame follows it: ask for the one that shows the
  // new selection and the chart.
  app_.requestFrame();
  chart.showModal();
  app_.requestFrame();
}

void Browser::openChartAt(int row, int column) {
  const auto & columns = table_.columns();
  if (column < 0 || column >= static_cast<int>(columns.size()) || !columns[static_cast<size_t>(column)].trace) return;
  action_row_ = row;
  showChart();
  action_row_.reset();
}

void Browser::showInfo() {
  const int row = action_row_.value_or(table_.selectedRow());
  if (auto news = std::dynamic_pointer_cast<NewsTable>(table_.model())) {
    if (row < 0 || row >= news->rowCount()) {
      status_.setMessage("No row selected");
      return;
    }
    auto snapshot = store_->snapshot();
    const NewsItem item = news->item(row);
    AttributeModal modal(app_.widgets(), app_.root(), app_.backend().eventLoop(), "News: " + item.headline, snapshot, buildNewsInfo(*snapshot, item));
    app_.requestFrame();
    modal.showModal();
    app_.requestFrame();
    return;
  }
  auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
  auto positions = std::dynamic_pointer_cast<Positions>(table_.model());
  if ((!quotes && !positions) || row < 0) {
    status_.setMessage("No row selected");
    return;
  }
  // A position is shown by its price edge, the listing it is valued by.
  std::optional<EdgeKey> found = quotes ? std::optional<EdgeKey>(quotes->edge(row)) : positions->priceEdge(row);
  if (!found) {
    status_.setMessage("No price for this position, so nothing to show");
    return;
  }
  const EdgeKey edge = *found;
  auto snapshot = store_->snapshot();
  std::string label = snapshot->symbol(edge);
  if (label.empty()) label = snapshot->name(edge.base);
  std::string title = "Info: " + label;
  AttributeModal modal(app_.widgets(), app_.root(), app_.backend().eventLoop(), std::move(title), snapshot, buildInfo(*snapshot, edge));
  // The input that got us here does not return until the modal closes, so no frame follows it: ask for the one that shows
  // the new selection and the dialog.
  app_.requestFrame();
  // The terminal and the window select and copy the dialog's text themselves while it is up, so it gets no clicks.
  app_.backend().setNativeSelection(modal.wantsNativeSelection());
  modal.showModal();
  app_.backend().setNativeSelection(false);
  app_.requestFrame();
}

void Browser::showTransactions() {
  const int row = action_row_.value_or(table_.selectedRow());
  auto positions = std::dynamic_pointer_cast<Positions>(table_.model());
  if (!positions) {
    status_.setMessage("Only a position has transactions: choose one in the Portfolio context (Tab)");
    return;
  }
  if (row < 0 || row >= positions->rowCount()) {
    status_.setMessage("No row selected");
    return;
  }
  auto snapshot = store_->snapshot();
  const portfolio::Position & p = positions->row(row).position;
  TransactionTable table = buildTransactionTable(*snapshot, positions->portfolio(), p.instrument, p.market, p.quote);
  const std::string market = snapshot->name(NodeRef(p.market));
  const std::string title = "Transactions: " + rowLabel(row) + (market.empty() ? "" : " on " + market) + " in " + snapshot->name(NodeRef(positions->portfolio()));
  const size_t count = table.rows.size();
  const std::string summary = std::to_string(count) + (count == 1 ? " transaction" : " transactions") + ", prices in " + table.currency + ". Held " +
                              realText(p.quantity) + " at an average cost of " + fixed(p.average_cost, 2) + " " + table.currency + ", fees included";
  TransactionsModal modal(app_.widgets(), app_.root(), app_.backend().eventLoop(), title, std::move(table), summary);
  // The input that got us here does not return until the modal closes, so no frame follows it: ask for the one that shows it.
  app_.requestFrame();
  app_.backend().setNativeSelection(modal.wantsNativeSelection());
  modal.showModal();
  app_.backend().setNativeSelection(false);
  app_.requestFrame();
}

std::optional<EdgeKey> Browser::selectedKey() const {
  const int row = table_.selectedRow();
  if (row < 0) return std::nullopt;
  if (auto * quotes = dynamic_cast<const Quotes *>(table_.model().get())) return quotes->edge(row);
  if (auto * positions = dynamic_cast<const Positions *>(table_.model().get())) return positions->key(row);
  return std::nullopt;
}

int Browser::rowOfKey(const EdgeKey & key) const {
  if (auto * quotes = dynamic_cast<const Quotes *>(table_.model().get())) {
    for (int r = 0; r < quotes->rowCount(); r++)
      if (quotes->edge(r) == key) return r;
  } else if (auto * positions = dynamic_cast<const Positions *>(table_.model().get())) {
    for (int r = 0; r < positions->rowCount(); r++)
      if (positions->key(r) == key) return r;
  }
  return -1;
}

void Browser::showTag(int i) {
  if (context_ == Context::Markets) showPreset(i);
  else if (context_ == Context::News) showNews(i);
  else showPortfolio(i);
}

void Browser::showPortfolio(int i) {
  portfolio_ = i >= 0 && i < static_cast<int>(portfolio_ids_.size()) ? portfolio_ids_[static_cast<size_t>(i)] : PortfolioId{};
  auto snapshot = store_->snapshot();
  auto positions = std::make_shared<Positions>(snapshot, portfolio_, icons_);
  positions->setPeriod(period_);
  table_.setModel(positions);
  setColumns();  // the value's heading names the portfolio's currency
  shown_version_ = snapshot->version();
  last_refresh_ = std::chrono::steady_clock::now();
  showEmptyHint();
}

bool Browser::syncPortfolioTags(const Snapshot & snapshot, PortfolioId prefer) {
  const std::vector<PortfolioId> ids = snapshot.portfolios();
  std::vector<std::string> labels;
  for (PortfolioId id : ids) labels.push_back(snapshot.name(NodeRef(id)));
  const auto at = std::find(ids.begin(), ids.end(), prefer);
  int index = at != ids.end() ? static_cast<int>(at - ids.begin()) : ids.empty() ? -1 : 0;
  portfolio_ = index >= 0 ? ids[static_cast<size_t>(index)] : PortfolioId{};
  const bool changed = ids != portfolio_ids_ || labels != tags_.tags();
  portfolio_ids_ = ids;
  const bool was = restoring_;
  restoring_ = true;  // choosing the tag here is not the user's
  if (changed) {
    tags_.setTags(labels);
    layout(app_.rows(), app_.cols());  // they may take more or fewer lines
  }
  tags_.select(index);
  restoring_ = was;
  return changed || (index >= 0 && portfolio_ != prefer);
}

void Browser::showEmptyHint() {
  std::string text;
  if (context_ == Context::Portfolio) {
    if (portfolio_ids_.empty()) text = "No portfolio yet. Make one with new-portfolio (" + app_.bindingFor("command-palette") + " for the commands).";
    else if (auto positions = std::dynamic_pointer_cast<Positions>(table_.model()); positions && positions->rowCount() == 0)
      text = "Nothing held in this portfolio yet. Buy from a row in Markets (Tab): the menu button at the end of the row.";
  }
  if (context_ == Context::Markets && tags_.selected() >= 0) {
    const Preset & preset = kPresets[static_cast<size_t>(tags_.selected())];
    if (auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model()); preset.list && quotes && quotes->rowCount() == 0)
      text = std::string("Nothing on the ") + preset.tag + " yet. Put a row on it from the menu button at the end of the row.";
  }
  if (context_ == Context::News) {
    if (auto news = std::dynamic_pointer_cast<NewsTable>(table_.model()); news && news->rowCount() == 0)
      text = "No news yet: a source that has news loads it as it runs.";
  }
  hint_.setText(text);
  hint_.setVisible(!text.empty());
}

void Browser::saveView() {
  View & v = views_[static_cast<size_t>(context_)];
  v.tag = tags_.selected();
  v.portfolio = portfolio_;
  v.sort_column = table_.sortColumn();
  v.sort_order = table_.sortOrder();
  v.first_position = table_.firstVisiblePosition();
  v.selected = selectedKey();
  v.news = selectedNewsKey();
}

void Browser::restoreView() {
  const View & v = views_[static_cast<size_t>(context_)];
  if (v.sort_column >= 0 && v.sort_column < static_cast<int>(table_.columns().size())) table_.sortBy(v.sort_column, v.sort_order);
  const int count = table_.model() ? table_.model()->rowCount() : 0;
  int row = context_ == Context::News ? (v.news.empty() ? -1 : rowOfNewsKey(v.news)) : v.selected ? rowOfKey(*v.selected) : -1;
  if (row < 0 && count > 0) row = 0;
  if (row >= 0) table_.reselect(row);
  table_.scrollToPosition(v.first_position);
}

void Browser::enterContext() {
  auto snapshot = store_->snapshot();
  const View & v = views_[static_cast<size_t>(context_)];
  const bool was = restoring_;
  restoring_ = true;
  table_.sortBy(-1, SortOrder::NONE);  // the columns are another context's: so was the sort
  if (context_ == Context::Markets) {
    std::vector<std::string> tags;
    for (const Preset & preset : kPresets) tags.push_back(preset.tag);
    tags_.setTags(std::move(tags));
    layout(app_.rows(), app_.cols());
    const int tag = std::clamp(v.tag, 0, static_cast<int>(std::size(kPresets)) - 1);
    tags_.select(tag);
    setColumns();
    showPreset(tag);
  } else if (context_ == Context::News) {
    std::vector<std::string> tags;
    for (const NewsPreset & preset : kNewsPresets) tags.push_back(preset.tag);
    tags_.setTags(std::move(tags));
    layout(app_.rows(), app_.cols());
    const int tag = std::clamp(v.tag, 0, static_cast<int>(std::size(kNewsPresets)) - 1);
    tags_.select(tag);
    setColumns();
    showNews(tag);
  } else {
    syncPortfolioTags(*snapshot, v.portfolio);
    showPortfolio(tags_.selected());
  }
  restoring_ = was;
  restoreView();
  hint_.setVisible(false);
  showEmptyHint();
  if (context_ == Context::Markets) status_.setMessage(std::string("Showing Markets: ") + kPresets[static_cast<size_t>(tags_.selected())].tag);
  else if (context_ == Context::News) status_.setMessage(std::string("Showing News: ") + kNewsPresets[static_cast<size_t>(tags_.selected())].tag);
  else if (auto positions = std::dynamic_pointer_cast<Positions>(table_.model()); positions && portfolio_.valid()) status_.setMessage(positions->summary());
  else status_.setMessage("Showing Portfolio: none yet");
  app_.requestFrame();
}

void Browser::showContext(Context context) {
  if (context == context_) return;
  saveView();
  context_ = context;
  enterContext();
}

void Browser::nextContext(int delta) {
  const int count = static_cast<int>(kContextCount);
  showContext(static_cast<Context>((static_cast<int>(context_) + delta % count + count) % count));
}

// The name a row goes by in a title or a message: its symbol, else its name.
std::string Browser::rowLabel(int row) const {
  if (auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model())) {
    const auto snapshot = store_->snapshot();
    const EdgeKey & edge = quotes->edge(row);
    const std::string symbol = snapshot->symbol(edge);
    return symbol.empty() ? snapshot->name(NodeRef(edge.base)) : symbol;
  }
  if (auto positions = std::dynamic_pointer_cast<Positions>(table_.model())) return positions->text(row, Positions::SYMBOL);
  return {};
}

// Where the menu button of a model row is on the screen: its line in the table (the heading is the first line), and the table's last
// column but one (the button is the last cell of a row). A row out of view is taken to be on the first line.
std::pair<int, int> Browser::menuAnchor(int row) const {
  const auto [top, left] = table_.getPosition();
  const int first = table_.firstVisiblePosition(), count = table_.visibleRows();
  int line = 0;
  for (int p = first; p < first + count; p++)
    if (table_.rowAt(p) == row) line = p - first;
  return {top + 1 + line, left + table_.getDim().second - 2};
}

// The menu of a row: what it can be done with, as a list to choose from. What is offered depends on the row (buy and sell where it can be
// bought, a list to be put on or taken off where it is not on it or is), and each entry is a command, which runs for this row.
void Browser::openMenu(int row) {
  auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
  auto positions = std::dynamic_pointer_cast<Positions>(table_.model());
  if ((!quotes && !positions) || row < 0 || row >= table_.model()->rowCount()) {
    status_.setMessage("No row selected");
    return;
  }
  const auto snapshot = store_->snapshot();
  struct Entry {
    std::string label, command;
  };
  std::vector<Entry> entries{{"Show info", "show-info"}};
  if (positions) entries.push_back({"Show transactions", "show-transactions"});
  // A position is of something that is traded; an edge of Markets is, unless it is an index, a spot price, a currency pair or a rate.
  if (!quotes || portfolio::tradable(*snapshot, quotes->edge(row))) {
    entries.push_back({"Buy", "buy"});
    entries.push_back({"Sell", "sell"});
  }
  // The lists are of edges, which only Markets has: a position is of no dataset.
  if (quotes) {
    for (const UserList & list : kLists) {
      const bool on = snapshot->onList(list.code, quotes->edge(row));
      entries.push_back({std::string(on ? "Take off the " : "Put on the ") + list.name, std::string(on ? "remove-from-" : "add-to-") + list.code});
    }
  }
  std::vector<std::string> labels;
  for (const Entry & e : entries) labels.push_back(e.label);
  // A menu of the usual kind: it opens in place, under the button, and leaves the screen as it is.
  PopupMenu menu(app_.widgets(), app_.root(), app_.backend().eventLoop(), labels);
  const auto [anchor_row, anchor_col] = menuAnchor(row);
  menu.setAnchor(anchor_row, anchor_col);
  // The input that got us here does not return until the menu is closed, so no frame follows it: ask for the one that shows it.
  app_.requestFrame();
  const int chosen = menu.showModal();
  app_.requestFrame();
  if (chosen < 0 || chosen >= static_cast<int>(entries.size())) return;
  // The command acts on the row the menu was opened on; the selection stays where it was.
  action_row_ = row;
  app_.execute(entries[static_cast<size_t>(chosen)].command);
  action_row_.reset();
}

// Puts the row the command is for on a list, or takes it off. A change of hers, so it can be taken back.
void Browser::changeList(size_t index, bool add) {
  const UserList & list = kLists[index];
  const int row = action_row_.value_or(table_.selectedRow());
  auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
  if (!quotes) {
    status_.setMessage(std::string("Only a row of Markets can be put on the ") + list.name + ": it is a list of edges");
    return;
  }
  if (row < 0 || row >= quotes->rowCount()) {
    status_.setMessage("No row selected");
    return;
  }
  const EdgeKey edge = quotes->edge(row);
  const std::string label = rowLabel(row), code = list.code, name = list.name;
  const bool on = store_->snapshot()->onList(code, edge);
  if (add == on) {
    status_.setMessage(label + (on ? " is on the " : " is not on the ") + name + (on ? " already" : ""));
    return;
  }
  const std::function<void(Store &)> put = [code, edge](Store & s) { s.addToList(code, {edge}); };
  const std::function<void(Store &)> take = [code, edge](Store & s) { s.removeFromList(code, {edge}); };
  try {
    (add ? put : take)(*store_);
  } catch (const std::invalid_argument & e) {
    status_.setMessage(std::string("Not changed: ") + e.what());
    return;
  }
  const std::string what = add ? "put " + label + " on the " + name : "took " + label + " off the " + name;
  record({what, add ? take : put, add ? put : take});
  wrote();
  status_.setMessage(std::string(add ? "Put " : "Took ") + label + (add ? " on the " : " off the ") + name + " (undo takes it back)");
}

void Browser::defineCommands() {
  CommandRegistry & commands = app_.commands();
  for (size_t i = 0; i < std::size(kPresets); i++) {
    // A preset is of the Markets context: from another, that is shown first.
    commands.define(kPresets[i].command, std::string("Show the ") + kPresets[i].tag + " preset", [this, i] {
      showContext(Context::Markets);
      tags_.select(static_cast<int>(i));
    });
  }
  for (size_t i = 0; i < std::size(kNewsPresets); i++) {
    commands.define(kNewsPresets[i].command, std::string("Show the ") + kNewsPresets[i].tag + " news, newest first", [this, i] {
      showContext(Context::News);
      tags_.select(static_cast<int>(i));
    });
  }
  for (size_t i = 0; i < std::size(kLists); i++) {
    const std::string name = kLists[i].name;
    commands.define(std::string("add-to-") + kLists[i].code, "Put the selected row on the " + name, [this, i] { changeList(i, true); });
    commands.define(std::string("remove-from-") + kLists[i].code, "Take the selected row off the " + name, [this, i] { changeList(i, false); });
  }
  commands.define("next-preset", "Show the next tag (preset, or portfolio)", [this] { tags_.select(std::min(tags_.selected() + 1, static_cast<int>(tags_.tags().size()) - 1)); });
  commands.define("previous-preset", "Show the previous tag (preset, or portfolio)", [this] { tags_.select(std::max(tags_.selected() - 1, 0)); });
  commands.define("next-context", "Switch to the next context (Markets, Portfolio, News)", [this] { nextContext(1); });
  commands.define("previous-context", "Switch to the previous context (Markets, Portfolio, News)", [this] { nextContext(-1); });
  for (size_t i = 0; i < kPeriodCount; i++) {
    const PeriodInfo & p = info(static_cast<Period>(i));
    commands.define(std::string("show-period-") + p.id, std::string("Show the change and the trace over ") + p.trace, [this, i] { showPeriod(static_cast<Period>(i)); });
  }
  commands.define("next-period", "Show the next period in the change and the trace (1d, 1w, 1m, 3m, 1y, 3y, 10y, then round)",
                  [this] { showPeriod(nextPeriod(period_)); });
  commands.define("previous-period", "Show the previous period in the change and the trace", [this] { showPeriod(previousPeriod(period_)); });
  commands.define("focus-tags", "Move the keyboard to the preset tags", [this] { app_.input().setFocus(&tags_); });
  commands.define("focus-table", "Move the keyboard to the table", [this] { app_.input().setFocus(&table_); });
  commands.define("show-info", "Show everything known about the selected row", [this] { showInfo(); });
  commands.define("show-transactions", "Show the transactions of the selected position in the portfolio shown, as a table",
                  [this] { showTransactions(); });
  commands.define("row-menu", "Open the menu of the selected row: its info, buy and sell, and the lists it can be put on", [this] { openMenu(table_.selectedRow()); });
  commands.define("show-chart", "Show the selected row's trace of the period as a large chart", [this] { showChart(); });
  commands.define("buy", "Buy the selected row in a portfolio: quantity, price, date, fee and a note", [this] { trade(portfolio::Side::Buy); });
  commands.define("sell", "Sell the selected row from a portfolio: quantity, price, date, fee and a note", [this] { trade(portfolio::Side::Sell); });
  commands.define("new-portfolio", "Make a portfolio: a name and the currency its value is shown in", [this] { makePortfolio("New portfolio"); });
  commands.define("rename-portfolio", "Rename the portfolio shown", [this] { renamePortfolio(); });
  commands.define("delete-portfolio", "Delete the portfolio shown, with everything said of it (undo brings it back)", [this] { deletePortfolio(); });
  commands.define("undo", "Take back the last change to the portfolios", [this] { undo(); });
  commands.define("redo", "Make the change that was taken back again", [this] { redo(); });
  commands.define("quit", "Leave stocks", [this] { app_.quit(); });
}

void Browser::bindKeys() {
  Keymap & keymap = app_.keymap();
  keymap.bind("q", "quit");
  // Tab and Shift+Tab switch the context in both profiles: what nothing takes walks the focus, so these are taken here.
  keymap.bind("Tab", "next-context");
  keymap.bind(KeymapProfile::TERMINAL, "S-Tab", "previous-context");
  keymap.bind(KeymapProfile::DESKTOP, "Shift+Tab", "previous-context");
  keymap.bind("i", "show-info");
  keymap.bind("c", "show-chart");
  keymap.bind(KeymapProfile::TERMINAL, "C-x =", "show-info");  // where the point is, as in Emacs
  keymap.bind(KeymapProfile::TERMINAL, "C-x C-c", "quit");
  // The period: ] and [ step through them; a direct choice is C-c and a letter (d w m q y 3 t: day, week, month, quarter, year, three years, ten) in the
  // terminal (C-c and a letter is what Emacs leaves to the user), Alt and a digit on the desktop.
  keymap.bind("]", "next-period");
  keymap.bind("[", "previous-period");
  constexpr const char * kLetters = "dwmqy3t";  // 3: three years, t: ten
  for (size_t i = 0; i < kPeriodCount; i++) {
    const std::string command = std::string("show-period-") + info(static_cast<Period>(i)).id;
    keymap.bind(KeymapProfile::TERMINAL, std::string("C-c ") + kLetters[i], command);
    keymap.bind(KeymapProfile::DESKTOP, "Alt+" + std::to_string(i + 1), command);
  }
  keymap.bind(KeymapProfile::DESKTOP, "Ctrl+Q", "quit");
  // Undo and redo of the user's changes, as each keymap has them. In the terminal: C-x u and C-/ (a terminal sends C-/ as C-_, which
  // Emacs also binds, so that is bound too), and C-M-_ for redo; on the desktop Ctrl+Z and Ctrl+Y. buy and sell have no key: the
  // buttons at the end of a row and the palette reach them.
  keymap.bind(KeymapProfile::TERMINAL, "C-x u", "undo");
  keymap.bind(KeymapProfile::TERMINAL, "C-/", "undo");
  keymap.bind(KeymapProfile::TERMINAL, "C-_", "undo");
  keymap.bind(KeymapProfile::TERMINAL, "C-M-_", "redo");
  keymap.bind(KeymapProfile::DESKTOP, "Ctrl+Z", "undo");
  keymap.bind(KeymapProfile::DESKTOP, "Ctrl+Y", "redo");
  assert(keymap.allBoundIn(app_.commands()));
}
