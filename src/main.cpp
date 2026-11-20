// The stock browser: cellui's Table, TagBar and trace chart over a store of
// candles (made-up ones for now).
//   stocks                 on the display cellui picks
//   stocks --ui terminal   in the terminal;  --ui window  in a window
// Keys: arrows, PgUp/PgDn, Home/End move; Tab moves between the tags and the
// table; M-x (Ctrl+Shift+P)
// opens the command palette; q quits. Clicking a heading sorts, clicking a tag
// or row chooses it, the wheel scrolls.
#include "Browser.h"
#include "Synthetic.h"

#include <cellui/Application.h>

#include <cstdio>
#include <memory>
#include <string>

int main(int argc, char ** argv) {
  std::string error;
  auto app = cellui::Application::open(argc, argv, &error);
  if (!app) {
    std::fprintf(stderr, "stocks: %s\n", error.c_str());
    return 1;
  }
  if (!app->arguments().empty()) {
    std::fprintf(stderr, "usage: %s [--ui auto|terminal|window] [--theme original|dark|light]\n", argv[0]);
    return 2;
  }
  std::shared_ptr<Store> store = makeMemoryStore();
  fillSyntheticData(*store);
  Browser browser(*app, store);
  return app->run();
}
