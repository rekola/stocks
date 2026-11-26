#pragma once

#include "history/HistoryLoader.h"
#include "model/Period.h"
#include "Positions.h"
#include "icons/IconLoader.h"
#include "model/Store.h"

#include <cellui/Application.h>
#include <cellui/Label.h>
#include <cellui/StatusLine.h>
#include <cellui/Table.h>
#include <cellui/TagBar.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

// The screen: tags on the top row, the table filling what is between them
// and the info and status lines, laid out again whenever the display changes size.
// A **context** is a view of the store with its own tags, columns and model: Markets (the presets of edges, `Quotes`), Portfolio (the
// portfolios by name, and what the chosen one holds, `Positions`) and News (the presets of news by its kind, newest first, `NewsTable`). Tab goes to the next context and Shift+Tab to the previous; the
// period tags stay and apply to both, and each context keeps its own tag, sort and scroll position while the program runs.
// The period tags at the right end of that row choose the period the Change and the trace columns show (the headings
// name it); it is one for every row, and kept while the session runs.
// Commands (named, run by a key or from the application's command palette) do what
// the keys and the mouse do: show a preset, choose a period, move the focus, quit. A click on a
// heading sorts by it. The status line is the echo area.
enum class Context { Markets, Portfolio, News };
inline constexpr size_t kContextCount = 3;

class Browser {
 public:
  // Shows what `store` holds: a preset is a market of that name, a row an edge in it.
  // `icons` loads the pictures of the icon column; without it the rows have none.
  // `history` loads the price history of the rows on screen; without it the rows have what the sources gave.
  Browser(cellui::Application & app, std::shared_ptr<Store> store, std::shared_ptr<IconLoader> icons = nullptr,
          std::shared_ptr<HistoryLoader> history = nullptr);

  cellui::Table & table() { return table_; }
  cellui::TagBar & tags() { return tags_; }
  cellui::TagBar & periodTags() { return periods_; }
  Period period() const { return period_; }
  Context context() const { return context_; }
  // The portfolio the Portfolio context shows (none where there is none).
  PortfolioId shownPortfolio() const { return portfolio_; }
  cellui::StatusLine & statusLine() { return status_; }
  // What today is, as a day number, for the date a trade starts with (the system's clock by default; a test sets its own).
  void setToday(std::function<int64_t()> today) { today_ = std::move(today); }
  // How many of the user's changes can be taken back, and made again.
  size_t undoDepth() const { return undo_.size(); }
  size_t redoDepth() const { return redo_.size(); }
  // How soon the table follows the store again: a source that writes a page a second, or a store of tens of thousands of
  // rows that changes all the time, is shown at most this often (default a quarter of a second).
  void setMinRefreshInterval(double seconds) { min_refresh_ = seconds; }

 private:
  // The rows of preset `i` from the store's current snapshot.
  void showPreset(int i);
  // The positions of the `i`th portfolio of the tags.
  void showPortfolio(int i);
  // The news of the `i`th news preset.
  void showNews(int i);
  void refreshNews(std::shared_ptr<const Snapshot>);
  // What a tag stands for in the context shown.
  void showTag(int i);
  void showContext(Context);
  void nextContext(int delta);
  // What a context's view was when it was left, and puts it back.
  void saveView();
  void restoreView();
  // Puts the tags and the model of the context shown in place.
  void enterContext();
  // The portfolios as tags, by name; true if they were not those shown. `prefer` is the one to keep selected, if it is still there.
  bool syncPortfolioTags(const Snapshot &, PortfolioId prefer);
  // The line that says there is nothing to show where there is none, over the table; hidden otherwise.
  void showEmptyHint();
  void refreshPortfolio(std::shared_ptr<const Snapshot>);
  // The key a row is known by across refreshes (an edge in Markets, a position in Portfolio).
  std::optional<EdgeKey> selectedKey() const;
  int rowOfKey(const EdgeKey &) const;
  // The same for the News context: an item is its dataset and its id.
  std::string selectedNewsKey() const;
  int rowOfNewsKey(const std::string &) const;
  // Tells the icon loader which rows are on screen and near it: those first, then the screens after and before.
  void requestIcons();
  // Shows `period` in the change and the trace columns, their headings and the period tags.
  void showPeriod(Period);
  void setColumns();
  // Tells the history loader which rows are on screen and near it, when that changed.
  void requestHistory();
  // Shows what the store holds now, keeping the sort, the scroll position and the selected row.
  void refresh();
  // Does so unless it did lately, in which case it does when it is time.
  void requestRefresh();
  void layout(int rows, int cols);
  // The info line's help: the keys as this display's keymap binds them.
  void showHelp();
  // The link text from the backend, drawn again when it changed.
  void showLink();
  // The attribute viewer for the selected row, from the store's snapshot now.
  void showInfo();
  // The transactions of the selected position (or the one the menu was opened on) in the portfolio shown, as a table: to see that
  // what was entered is right. Only the Portfolio context has positions.
  void showTransactions();
  // The selected row's trace of the period shown as a large chart over the dimmed screen: the line, its values and dates, and a cursor
  // to read it with. A click on a trace cell opens it for that row, and so does Enter on one.
  void showChart();
  // The chart of `row`, if `column` is a trace column.
  void openChartAt(int row, int column);
  // The menu of a row (the button at the end of it): info, a position's transactions, buy and sell, and the lists it can be put on or
  // taken off.
  void openMenu(int row);
  // Puts the selected row (or the one the menu was opened on) on a list of kLists, or takes it off; the user's change, which undo takes back.
  void changeList(size_t list, bool add);
  // What a row is called in the menu's title and in messages.
  std::string rowLabel(int row) const;
  void defineCommands();
  void bindKeys();

  // The user's changes to her portfolios (src/BrowserPortfolio.cpp). Each is a pair of store writes (the inverse and the write
  // again), kept for the session in a stack, so undo takes the last change back whatever it was: a row bought, a portfolio renamed
  // or deleted.
  struct Change {
    std::string what;
    std::function<void(Store &)> undo, redo;
  };
  void record(Change);
  void undo();
  void redo();
  void trade(portfolio::Side);
  // Asks a name and a currency and makes the portfolio, which becomes the one shown; none if it was cancelled.
  std::optional<PortfolioId> makePortfolio(const std::string & title);
  void renamePortfolio();
  void deletePortfolio();
  // The portfolio the commands act on: the one shown, or the last one shown.
  PortfolioId currentPortfolio() const;
  // Shows what a write changed now, bypassing the pause between refreshes: the user asked for it.
  void wrote();

  cellui::Application & app_;
  std::shared_ptr<Store> store_;
  std::shared_ptr<IconLoader> icons_;
  std::shared_ptr<HistoryLoader> history_;
  cellui::TagBar tags_, periods_;
  Period period_ = kDefaultPeriod;
  cellui::Table table_;
  cellui::Label info_;
  cellui::Label hint_;  // over the table's first rows where the Portfolio context has nothing to show
  cellui::Label link_;  // the display's link, at the right end of the info line
  cellui::StatusLine status_;
  uint64_t shown_version_ = 0;  // of the snapshot the table shows
  int shown_preset_ = -1;       // the preset the table shows
  int shown_news_ = -1;         // the news preset the table shows
  Context context_ = Context::Markets;
  PortfolioId portfolio_;                    // the portfolio the Portfolio context shows
  std::vector<PortfolioId> portfolio_ids_;   // the portfolios, as the tags of the Portfolio context list them
  bool restoring_ = false;                   // the tags are being put back: choosing one is not a user's choice
  // What each context was left as: its tag, sort and scroll position (and the row), kept while the program runs.
  struct View {
    int tag = 0;
    PortfolioId portfolio;
    int sort_column = -1;
    cellui::SortOrder sort_order = cellui::SortOrder::NONE;
    int first_position = 0;
    std::optional<EdgeKey> selected;
    std::string news;  // the item selected in News
  };
  View views_[kContextCount];
  std::vector<Change> undo_, redo_;
  std::function<int64_t()> today_;
  PortfolioId last_traded_;  // the portfolio the last trade went to: where the next is offered
  // What the history loader was last told is wanted: the edges, in the order sent, and the period. The edges themselves and
  // not where the view is, as a sort (a click on a heading, or a store change that sorts again) puts other edges at the
  // same positions.
  std::vector<EdgeKey> history_edges_, history_scratch_;
  std::optional<Period> history_period_;
  int64_t history_minute_ = 0;  // the wall minute it was told in: told again every minute, for the requests that have expired
  double min_refresh_ = 0.25;
  double refresh_cost_ = 0;  // seconds the last refresh took: the next waits longer than that many times over, so loading never keeps the UI busy
  std::chrono::steady_clock::time_point last_refresh_{};
  std::optional<int> action_row_;  // the row a button was clicked on, while its command runs: not the selected one
  bool pending_ = false;        // the store changed and the table does not show it yet
  cellui::Connection period_tag_, button_, chart_click_, chart_enter_, resized_, display_, tag_, message_, frame_, changed_, said_, loaded_;
};
