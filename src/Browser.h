#pragma once

#include "model/Store.h"

#include <cellui/Application.h>
#include <cellui/StatusLine.h>
#include <cellui/Table.h>
#include <cellui/TagBar.h>

#include <memory>

// The screen: preset tags on the top row, the table filling what is between them
// and the info and status lines, laid out again whenever the display changes size.
// Commands (named, run by a key or from the application's command palette) do what
// the keys and the mouse do: show a preset, sort, move the focus, quit. The status
// line is the echo area.
class Browser {
 public:
  // Shows what `store` holds: a preset is a market of that name, a row an edge in it.
  Browser(cellui::Application & app, std::shared_ptr<Store> store);

  cellui::Table & table() { return table_; }
  cellui::TagBar & tags() { return tags_; }
  cellui::StatusLine & statusLine() { return status_; }

 private:
  // The rows of preset `i` from the store's current snapshot.
  void showPreset(int i);
  // Shows what the store holds now, keeping the sort and the selected row.
  void refresh();
  void layout(int rows, int cols);
  void defineCommands();
  void bindKeys();

  cellui::Application & app_;
  std::shared_ptr<Store> store_;
  cellui::TagBar tags_;
  cellui::Table table_;
  cellui::Widget info_;
  cellui::StatusLine status_;
  uint64_t shown_version_ = 0;  // of the snapshot the table shows
  cellui::Connection resized_, display_, tag_, message_, frame_, changed_, said_;
};
