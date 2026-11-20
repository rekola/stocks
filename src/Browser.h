#pragma once

#include <cellui/Application.h>
#include <cellui/Label.h>
#include <cellui/StatusLine.h>
#include <cellui/Table.h>
#include <cellui/TagBar.h>

// The screen: preset tags on the top row, the table filling what is between them
// and the info and status lines, laid out again whenever the display changes size.
// Commands (named, run by a key or from the application's command palette) do what
// the keys and the mouse do: show a preset, move the focus, quit. A click on a
// heading sorts by it. The status line is the echo area.
class Browser {
 public:
  explicit Browser(cellui::Application & app);

  cellui::Table & table() { return table_; }
  cellui::TagBar & tags() { return tags_; }
  cellui::StatusLine & statusLine() { return status_; }

 private:
  void layout(int rows, int cols);
  // The info line's help: the keys as this display's keymap binds them.
  void showHelp();
  void defineCommands();
  void bindKeys();

  cellui::Application & app_;
  cellui::TagBar tags_;
  cellui::Table table_;
  cellui::Label info_;
  cellui::StatusLine status_;
  cellui::Connection resized_, display_, tag_, message_, frame_;
};
