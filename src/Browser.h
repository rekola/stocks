#pragma once

#include <cellui/Application.h>
#include <cellui/CommandPalette.h>
#include <cellui/CommandRegistry.h>
#include <cellui/Keymap.h>
#include <cellui/StatusLine.h>
#include <cellui/Table.h>
#include <cellui/TagBar.h>

#include <optional>
#include <string>
#include <vector>

// The screen: preset tags on the top row, the table filling what is between them
// and the info and status lines, laid out again whenever the display changes size.
// Commands (named, run by a key or from the command palette) do what the keys
// and the mouse do: show a preset, sort, move the focus, quit. The status line
// is the echo area; M-x (Ctrl+Shift+P on the desktop keymap) opens the palette.
class Browser {
 public:
  explicit Browser(cellui::Application & app);

  cellui::Table & table() { return table_; }
  cellui::TagBar & tags() { return tags_; }
  cellui::StatusLine & statusLine() { return status_; }
  bool paletteOpen() const { return palette_.isOpen(); }
  // Every command's name.
  std::vector<std::string> commandNames() const;
  bool execute(const std::string & name) { return commands_.execute(name); }

 private:
  void layout(int rows, int cols);
  void defineCommands();
  void bindKeys();
  // The input the application got, before its own handling: true if taken.
  bool offer(const cellui::Input & input);
  bool dispatchCommand(const cellui::KeyDown & e);
  void openPalette();
  void layoutPalette();
  void handlePalette(cellui::CommandPalette::Result result);
  // How `name` is bound on this display, as the palette shows it; "" if it is not.
  std::string bindingFor(const std::string & name) const;
  cellui::KeymapProfile profile() const { return app_.root().keymapProfile(); }

  cellui::Application & app_;
  cellui::TagBar tags_;
  cellui::Table table_;
  cellui::Widget info_;
  cellui::StatusLine status_;
  cellui::CommandPalette palette_;
  cellui::CommandRegistry commands_;
  cellui::Keymap keymap_;
  // The first shortcut of a two-key sequence (C-x), while it waits for the second.
  std::optional<uint64_t> pending_prefix_;
  cellui::Connection resized_, display_, tag_, frame_;
};
