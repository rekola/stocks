// The stock browser: cellui's Table, TagBar and trace chart over a store of
// candles that data sources fill on a thread of their own.
//   stocks                 on the display cellui picks, with prices from Yahoo
//   stocks --ui terminal   in the terminal;  --ui window  in a window
//   stocks --source NAME   pick the data source (yahoo, synthetic); more than one may be given
// Keys: arrows, PgUp/PgDn, Home/End move; Tab moves between the tags and the
// table; M-x (Ctrl+Shift+P)
// opens the command palette; q quits. Clicking a heading sorts, clicking a tag
// or row chooses it, the wheel scrolls.
#include "Browser.h"
#include "Synthetic.h"
#include "source/SourceRunner.h"
#include "source/YahooSource.h"

#include <cellui/Application.h>
#include <cellui/ThreadPosts.h>

#include <cstdio>
#include <memory>
#include <string>

namespace {

// The adapters there are, by the name the user picks them with.
std::unique_ptr<Source> makeSource(const std::string & name) {
  if (name == "yahoo") return std::make_unique<YahooSource>();
  if (name == "synthetic") return std::make_unique<SyntheticSource>();
  return nullptr;
}

constexpr const char * kSourceNames = "yahoo, synthetic";

}  // namespace

int main(int argc, char ** argv) {
  std::string error;
  auto app = cellui::Application::open(argc, argv, &error);
  if (!app) {
    std::fprintf(stderr, "stocks: %s\n", error.c_str());
    return 1;
  }
  std::vector<std::unique_ptr<Source>> sources;
  const auto & args = app->arguments();
  for (size_t i = 0; i < args.size(); i++) {
    std::string name;
    if (args[i] == "--source" && i + 1 < args.size()) name = args[++i];
    else if (args[i].rfind("--source=", 0) == 0) name = args[i].substr(9);
    else {
      std::fprintf(stderr, "usage: %s [--ui auto|terminal|window] [--theme original|dark|light] [--source %s]...\n", argv[0], kSourceNames);
      return 2;
    }
    std::unique_ptr<Source> source = makeSource(name);
    if (!source) {
      std::fprintf(stderr, "stocks: unknown --source %s (there are %s)\n", name.c_str(), kSourceNames);
      return 2;
    }
    sources.push_back(std::move(source));
  }
  if (sources.empty()) sources.push_back(makeSource("yahoo"));

  std::shared_ptr<Store> store = makeMemoryStore();
  Browser browser(*app, store);
  // The sources run on a thread of their own and tell the UI thread what happened.
  cellui::ThreadInbox inbox(app->backend().eventLoop(), app->widgets());
  SourceRunner runner(
      store, std::move(sources), [&inbox] { inbox.post(StoreChanged{}); },
      [&inbox](std::string text) { inbox.post(SourceMessage{std::move(text)}); });
  return app->run();
}
