// The stock browser: cellui's Table, TagBar and trace chart over a store of
// candles that data sources fill on a thread of their own.
//   stocks                 on the display cellui picks, with prices from Yahoo
//   stocks --ui terminal   in the terminal;  --ui window  in a window
//   stocks --source NAME   pick the data source (yahoo, nordnet, synthetic); more than one may be given
//   stocks --lists DIR     the instrument lists of the yahoo and synthetic sources (default: data/yahoo)
//   stocks --market NAME   of those lists, load only that market, e.g. "Nasdaq Helsinki" (repeatable)
// Keys: arrows, PgUp/PgDn, Home/End move; Tab moves between the tags and the
// table; M-x (Ctrl+Shift+P)
// opens the command palette; q quits. Clicking a heading sorts, clicking a tag
// or row chooses it, the wheel scrolls.
#include "Browser.h"
#include "Synthetic.h"
#include "source/SourceRunner.h"
#include "source/NordnetSource.h"
#include "source/YahooSource.h"

#include <cellui/Application.h>
#include <cellui/ThreadPosts.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace {

// The adapters there are, by the name the user picks them with.
std::unique_ptr<Source> makeSource(const std::string & name, const ListSpec & lists) {
  if (name == "yahoo") return std::make_unique<YahooSource>(lists);
  if (name == "nordnet") return std::make_unique<NordnetSource>();
  if (name == "synthetic") return std::make_unique<SyntheticSource>(lists);
  return nullptr;
}

constexpr const char * kSourceNames = "yahoo, nordnet, synthetic";

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
  std::string list_directory;
  std::vector<std::string> market_names;
  const auto & args = app->arguments();
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--source" && i + 1 < args.size()) source_names.push_back(args[++i]);
    else if (args[i].rfind("--source=", 0) == 0) source_names.push_back(args[i].substr(9));
    else if (args[i] == "--market" && i + 1 < args.size()) market_names.push_back(args[++i]);
    else if (args[i].rfind("--market=", 0) == 0) market_names.push_back(args[i].substr(9));
    else if (args[i] == "--lists" && i + 1 < args.size()) list_directory = args[++i];
    else if (args[i].rfind("--lists=", 0) == 0) list_directory = args[i].substr(8);
    else {
      return fail(2, std::string("usage: ") + argv[0] + " [--ui auto|terminal|window] [--theme original|dark|light] [--source " +
                         kSourceNames + "]... [--lists DIR] [--market NAME]...");
    }
  }
  if (source_names.empty()) source_names.push_back("yahoo");

  // The instrument lists are the yahoo source's own (and the synthetic one follows them); each reads them when it loads.
  ListSpec lists{list_directory, market_names};
  std::vector<std::unique_ptr<Source>> sources;
  for (const std::string & name : source_names) {
    std::unique_ptr<Source> source = makeSource(name, lists);
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
