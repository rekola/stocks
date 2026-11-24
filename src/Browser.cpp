#include "Browser.h"

#include "AttributeView.h"
#include "Quotes.h"
#include "RowActions.h"
#include "source/Source.h"

#include <cellui/Layout.h>

#include <algorithm>
#include <cassert>

using namespace cellui;

namespace {

// A preset is a selection of edges: a market (by its MIC; the indices, spot
// prices and currency pairs are on no market, XXXX) and, where that holds
// several kinds, the kind of the base. No mic is every market, of the kind where there is one.
struct Preset {
  const char * tag;
  const char * command;
  const char *mic = nullptr;
  std::optional<InstrumentKind> kind = std::nullopt;
};
constexpr Preset kPresets[] = {
    {"Commodities", "show-commodities", kNoMarket, InstrumentKind::Commodity},
    {"Nasdaq Helsinki", "show-nasdaq-helsinki", "XHEL"},
    {"Currencies", "show-currencies", kNoMarket, InstrumentKind::Currency},
    {"Indices", "show-indices", kNoMarket, InstrumentKind::Index},
    {"Funds", "show-funds", nullptr, InstrumentKind::Fund},  // of every market, whoever lists them
    {"All", "show-all"}};

// What the buttons at the end of a row do: a glyph and the command that runs for the selected row. A new action is a new line.
struct RowAction {
  const char * glyph;
  const char * command;
};
// The glyphs of buy and sell are the circled plus and minus (U+2295, U+2296): Mathematical Operators, which the monospace fonts
// terminals and the window use have, one cell wide, so they draw the same in both (the info glyph is U+1F6C8).
constexpr const char * kBuyGlyph = "\xE2\x8A\x95";
constexpr const char * kSellGlyph = "\xE2\x8A\x96";
constexpr RowAction kRowActions[] = {{kInfoGlyph, "show-info"}, {kBuyGlyph, "buy"}, {kSellGlyph, "sell"}};
static_assert(kInfoButton == 0 && kBuyButton == 1 && kSellButton == 2 && std::size(kRowActions) == 3, "RowActions.h numbers the buttons as they are here");

Quotes::Scope scopeOf(const Snapshot & snapshot, const Preset & preset) {
  if (!preset.mic)
    return {.all = true, .kind = preset.kind};
  return {.market = snapshot.findMarket(preset.mic), .kind = preset.kind};
}

// The edges of a preset: a market's, or everything.
std::vector<EdgeKey> edgesOf(const Snapshot & snapshot, const Preset & preset) {
  if (!preset.mic) {
    std::vector<EdgeKey> keys = allQuoteEdges(snapshot);
    if (preset.kind)
      std::erase_if(keys, [&](const EdgeKey & k) { return snapshot.kind(k.base) != *preset.kind; });
    return keys;
  }
  const std::optional<MarketId> market = snapshot.findMarket(preset.mic);
  return market ? quoteEdges(snapshot, *market, preset.kind)
                : std::vector<EdgeKey>{};
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
    if (i < 0 || restoring_) return;
    showTag(i);
    table_.select(0);
    if (context_ == Context::Markets) status_.setMessage(std::string("Showing ") + kPresets[i].tag);
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
  if (context_ == Context::Markets) {
    table_.setColumns({{.title = "", .width = 2, .sortable = false, .icon = true},
                       {.title = "Base", .width = 8},
                       {.title = "Quote", .width = 6},
                       {.title = "Name", .width = 0},
                       {.title = "Market", .width = 16},
                       {.title = "Dataset", .width = 9},
                       {.title = "Price", .width = 9, .align = Align::RIGHT},
                       {.title = changeTitle(period_), .width = 11, .align = Align::RIGHT},
                       {.title = info(period_).trace, .width = 16, .sortable = false, .trace = true},
                       {.title = "", .buttons = buttonGlyphs()}});
    return;
  }
  const auto positions = std::dynamic_pointer_cast<Positions>(table_.model());
  table_.setColumns({{.title = "", .width = 2, .sortable = false, .icon = true},
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
                     {.title = "", .buttons = buttonGlyphs()}});
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
  const int preset = tags_.selected();
  if (preset < 0 || snapshot->version() == shown_version_) return;
  std::optional<EdgeKey> selected;
  auto quotes = std::dynamic_pointer_cast<Quotes>(table_.model());
  if (quotes && table_.selectedRow() >= 0) selected = quotes->edge(table_.selectedRow());

  if (quotes && preset == shown_preset_) {
    // The same preset with new data: change the rows in place, which keeps the scroll position, the sort and the selection
    // (setModel would put the table back at the top).
    // Most changes are new bars for rows already listed: then the rows stay as they are, and with no sort the order too, so the
    // table is drawn again and nothing is sorted or put in order again.
    // The rows keep their places and a row that appears goes last: the store says which have, so nothing is listed or compared
    // again unless there are some.
    const bool same_rows = !(quotes->hasNewRows(*snapshot) ? quotes->addRows(snapshot, scopeOf(*snapshot, kPresets[preset])) : (quotes->setSnapshot(snapshot), false));
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
  layoutColumns(3, 0, 1, cols, {{.widget = &hint_, .fill = 1}});  // the first row under the heading
  layoutColumns(rows - 2, 0, 1, cols, {{.widget = &info_, .fill = 1}, {.widget = &link_, .size = std::min(kLinkCols, cols / 2)}});
}

void Browser::showLink() { link_.setText(linkStatusText(app_.backend().linkStatus()) + " "); }

void Browser::showHelp() {
  info_.setText(" stocks.  " + app_.bindingFor("command-palette") + ": commands, click a heading: sort, Tab: switch context, " +
                app_.bindingFor("quit") + ": quit");
}

void Browser::showInfo() {
  const int row = action_row_.value_or(table_.selectedRow());
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
  modal.showModal();
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
  if (changed) tags_.setTags(labels);
  tags_.select(index);
  restoring_ = was;
  return changed || (index >= 0 && portfolio_ != prefer);
}

void Browser::showEmptyHint() {
  std::string text;
  if (context_ == Context::Portfolio) {
    if (portfolio_ids_.empty()) text = "No portfolio yet. Make one with new-portfolio (" + app_.bindingFor("command-palette") + " for the commands).";
    else if (auto positions = std::dynamic_pointer_cast<Positions>(table_.model()); positions && positions->rowCount() == 0)
      text = "Nothing held in this portfolio yet. Buy from a row in Markets (Tab): the buy button at the end of the row.";
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
}

void Browser::restoreView() {
  const View & v = views_[static_cast<size_t>(context_)];
  if (v.sort_column >= 0 && v.sort_column < static_cast<int>(table_.columns().size())) table_.sortBy(v.sort_column, v.sort_order);
  const int count = table_.model() ? table_.model()->rowCount() : 0;
  int row = v.selected ? rowOfKey(*v.selected) : -1;
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
    const int tag = std::clamp(v.tag, 0, static_cast<int>(std::size(kPresets)) - 1);
    tags_.select(tag);
    setColumns();
    showPreset(tag);
  } else {
    syncPortfolioTags(*snapshot, v.portfolio);
    showPortfolio(tags_.selected());
  }
  restoring_ = was;
  restoreView();
  hint_.setVisible(false);
  showEmptyHint();
  if (context_ == Context::Markets) status_.setMessage(std::string("Showing Markets: ") + kPresets[static_cast<size_t>(tags_.selected())].tag);
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

void Browser::defineCommands() {
  CommandRegistry & commands = app_.commands();
  for (size_t i = 0; i < std::size(kPresets); i++) {
    // A preset is of the Markets context: from another, that is shown first.
    commands.define(kPresets[i].command, std::string("Show the ") + kPresets[i].tag + " preset", [this, i] {
      showContext(Context::Markets);
      tags_.select(static_cast<int>(i));
    });
  }
  commands.define("next-preset", "Show the next tag (preset, or portfolio)", [this] { tags_.select(std::min(tags_.selected() + 1, static_cast<int>(tags_.tags().size()) - 1)); });
  commands.define("previous-preset", "Show the previous tag (preset, or portfolio)", [this] { tags_.select(std::max(tags_.selected() - 1, 0)); });
  commands.define("next-context", "Switch to the next context (Markets, Portfolio)", [this] { nextContext(1); });
  commands.define("previous-context", "Switch to the previous context (Markets, Portfolio)", [this] { nextContext(-1); });
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
