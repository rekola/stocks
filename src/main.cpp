// The stock browser: cellui's Table, TagBar and trace chart over a store of
// candles that data sources fill on a thread of their own.
//   stocks                 on the display cellui picks, with prices from Yahoo
//   stocks --ui terminal   in the terminal;  --ui window  in a window
//   stocks --source NAME   pick the data source (yahoo, synthetic); more than one may be given
//   stocks --universe DIR  the instrument lists (default: data/universe)
//   stocks --market NAME   load only the instruments of that market, e.g. "Nasdaq Helsinki" (repeatable)
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
#include <filesystem>
#include <memory>
#include <string>

namespace {

// The adapters there are, by the name the user picks them with.
std::unique_ptr<Source> makeSource(const std::string & name, const Universe & universe) {
  if (name == "yahoo") return std::make_unique<YahooSource>(universe);
  if (name == "synthetic") return std::make_unique<SyntheticSource>(universe);
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
  // The display is closed before a message is printed, or the terminal's alternate screen would swallow it.
  auto fail = [&app](int status, const std::string & message) {
    app.reset();
    std::fprintf(stderr, "%s\n", message.c_str());
    return status;
  };
  std::vector<std::string> source_names;
  std::string universe_directory;
  std::vector<std::string> market_names;
  const auto & args = app->arguments();
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--source" && i + 1 < args.size()) source_names.push_back(args[++i]);
    else if (args[i].rfind("--source=", 0) == 0) source_names.push_back(args[i].substr(9));
    else if (args[i] == "--market" && i + 1 < args.size()) market_names.push_back(args[++i]);
    else if (args[i].rfind("--market=", 0) == 0) market_names.push_back(args[i].substr(9));
    else if (args[i] == "--universe" && i + 1 < args.size()) universe_directory = args[++i];
    else if (args[i].rfind("--universe=", 0) == 0) universe_directory = args[i].substr(11);
    else {
      return fail(2, std::string("usage: ") + argv[0] + " [--ui auto|terminal|window] [--theme original|dark|light] [--source " +
                         kSourceNames + "]... [--universe DIR] [--market NAME]...");
    }
  }
  if (source_names.empty()) source_names.push_back("yahoo");

  // The instruments the presets show: lists on disk (data/universe), one file per market.
  std::filesystem::path directory = findUniverseDirectory(universe_directory);
  Universe universe = directory.empty() ? Universe{} : loadUniverse(directory, error);
  if (universe.empty()) {
    return fail(1, std::string("stocks: ") + (directory.empty() ? "no instrument lists (data/universe); --universe DIR names them" : error));
  }
  universe = onlyMarkets(universe, market_names);
  if (universe.empty()) {
    return fail(1, "stocks: no instruments in the --market given (the lists' markets are named in their files)");
  }
  std::vector<std::unique_ptr<Source>> sources;
  for (const std::string & name : source_names) {
    std::unique_ptr<Source> source = makeSource(name, universe);
    if (!source) {
      return fail(2, "stocks: unknown --source " + name + " (there are " + kSourceNames + ")");
    }
    sources.push_back(std::move(source));
  }

  std::shared_ptr<Store> store = makeMemoryStore();
  Browser browser(*app, store);
  // The sources run on a thread of their own and tell the UI thread what happened.
  cellui::ThreadInbox inbox(app->backend().eventLoop(), app->widgets());
  SourceRunner runner(
      store, std::move(sources), [&inbox] { inbox.post(StoreChanged{}); },
      [&inbox](std::string text) { inbox.post(SourceMessage{std::move(text)}); });
  return app->run();
}
