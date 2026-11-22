#pragma once

#include "history/HistoryLoader.h"
#include "icons/IconLoader.h"
#include "model/Store.h"

#include <cellui/Application.h>
#include <cellui/Label.h>
#include <cellui/StatusLine.h>
#include <cellui/Table.h>
#include <cellui/TagBar.h>

#include <chrono>
#include <memory>
#include <optional>

// The screen: preset tags on the top row, the table filling what is between them
// and the info and status lines, laid out again whenever the display changes size.
// Commands (named, run by a key or from the application's command palette) do what
// the keys and the mouse do: show a preset, move the focus, quit. A click on a
// heading sorts by it. The status line is the echo area.
class Browser {
 public:
  // Shows what `store` holds: a preset is a market of that name, a row an edge in it.
  // `icons` loads the pictures of the icon column; without it the rows have none.
  // `history` loads the price history of the rows on screen; without it the rows have what the sources gave.
  Browser(cellui::Application & app, std::shared_ptr<Store> store, std::shared_ptr<IconLoader> icons = nullptr,
          std::shared_ptr<HistoryLoader> history = nullptr);

  cellui::Table & table() { return table_; }
  cellui::TagBar & tags() { return tags_; }
  cellui::StatusLine & statusLine() { return status_; }
  // How soon the table follows the store again: a source that writes a page a second, or a store of tens of thousands of
  // rows that changes all the time, is shown at most this often (default a quarter of a second).
  void setMinRefreshInterval(double seconds) { min_refresh_ = seconds; }

 private:
  // The rows of preset `i` from the store's current snapshot.
  void showPreset(int i);
  // Tells the icon loader which rows are on screen and near it: those first, then the screens after and before.
  void requestIcons();
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
  void defineCommands();
  void bindKeys();

  cellui::Application & app_;
  std::shared_ptr<Store> store_;
  std::shared_ptr<IconLoader> icons_;
  std::shared_ptr<HistoryLoader> history_;
  cellui::TagBar tags_;
  cellui::Table table_;
  cellui::Label info_;
  cellui::Label link_;  // the display's link, at the right end of the info line
  cellui::StatusLine status_;
  uint64_t shown_version_ = 0;  // of the snapshot the table shows
  int shown_preset_ = -1;       // the preset the table shows
  struct HistoryView {
    const void * model = nullptr;
    int first = -1, rows = -1, count = -1;
    bool operator==(const HistoryView &) const = default;
  } history_view_;              // what the history loader was last told is wanted
  double min_refresh_ = 0.25;
  std::chrono::steady_clock::time_point last_refresh_{};
  std::optional<int> action_row_;  // the row a button was clicked on, while its command runs: not the selected one
  bool pending_ = false;        // the store changed and the table does not show it yet
  cellui::Connection button_, resized_, display_, tag_, message_, frame_, changed_, said_, loaded_;
};
