// The stock browser: cellui's Table, TagBar and trace chart over a store of
// candles that data sources fill on a thread of their own.
//   stocks                 on the display cellui picks, with Nordnet's data, or
//   Yahoo's if Nordnet gives nothing stocks --ui terminal   in the terminal;
//   --ui window  in a window stocks --source NAME   the data source (several,
//   comma-separated, run at once): auto (the default: nordnet, and yahoo if
//   nordnet gives nothing),
//                          nordnet, yahoo, suomenpankki (the Euribor rates), funds:SCHEMA (a fund company: a
//                          schema in data/funds, or a schema file) or synthetic
//   stocks --lists DIR     the instrument lists of the yahoo and synthetic
//   sources (default: data/lists) stocks --facts FILE    where what the sources
//   said of instruments, markets and datasets is kept between runs (default:
//   facts.sqlite in the
//                          user's data directory); --no-facts runs without it
//   stocks --market MIC    of those lists, load only that market, e.g. XHEL;
//   XXXX is the indices, spot prices and currency pairs (repeatable)
// Keys: arrows, PgUp/PgDn, Home/End move; Tab moves between the tags and the
// table; M-x (Ctrl+Shift+P)
// opens the command palette; q quits. Clicking a heading sorts, clicking a tag
// or row chooses it, the wheel scrolls.
#include "Browser.h"
#include "Synthetic.h"
#include "history/HistoryLoader.h"
#include "icons/IconLoader.h"
#include "model/SqliteFactStore.h"
#include "source/AutoSource.h"
#include "source/SourceRunner.h"
#include "source/NordnetSource.h"
#include "source/FundSource.h"
#include "source/SuomenPankkiSource.h"
#include "source/YahooSource.h"

#include <cellui/Application.h>
#include <cellui/ThreadPosts.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace {

// The adapters there are, by the name the user picks them with. One runs at a time: "auto" is Nordnet, and Yahoo if Nordnet
// gives nothing.
// `funds:NAME` is the fund adapter with the schema NAME (data/funds/NAME.json) or the schema file NAME; `error` says what is wrong with a schema.
std::unique_ptr<Source> makeSource(const std::string & name, const ListSpec & lists, std::string & error) {
  if (name.rfind("funds:", 0) == 0) {
    FundSchema schema;
    if (!FundSchema::load(name.substr(6), schema, error)) return nullptr;
    return std::make_unique<FundSource>(std::move(schema));
  }
  if (name == "auto") return std::make_unique<AutoSource>(std::make_unique<NordnetSource>(), std::make_unique<YahooSource>(lists));
  if (name == "yahoo") return std::make_unique<YahooSource>(lists);
  if (name == "nordnet") return std::make_unique<NordnetSource>();
  if (name == "suomenpankki") return std::make_unique<SuomenPankkiSource>();
  if (name == "synthetic") return std::make_unique<SyntheticSource>(lists);
  return nullptr;
}

constexpr const char * kSourceNames = "auto, nordnet, yahoo, suomenpankki, funds:SCHEMA, synthetic";

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
  std::string source_name = "auto";
  std::string list_directory;
  std::vector<std::string> market_names;
  std::string facts_file;
  bool keep_facts = true;
  const auto & args = app->arguments();
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--source" && i + 1 < args.size()) source_name = args[++i];
    else if (args[i].rfind("--source=", 0) == 0) source_name = args[i].substr(9);
    else if (args[i] == "--market" && i + 1 < args.size()) market_names.push_back(args[++i]);
    else if (args[i].rfind("--market=", 0) == 0) market_names.push_back(args[i].substr(9));
    else if (args[i] == "--facts" && i + 1 < args.size()) facts_file = args[++i];
    else if (args[i].rfind("--facts=", 0) == 0) facts_file = args[i].substr(8);
    else if (args[i] == "--no-facts") keep_facts = false;
    else if (args[i] == "--lists" && i + 1 < args.size()) list_directory = args[++i];
    else if (args[i].rfind("--lists=", 0) == 0) list_directory = args[i].substr(8);
    else {
      return fail(
          2,
          std::string("usage: ") + argv[0] +
              " [--ui auto|terminal|window] [--theme original|dark|light] "
              "[--source " +
              kSourceNames +
              "] [--facts FILE | --no-facts] [--lists DIR] [--market MIC]...");
    }
  }
  // One source, for now: the instrument lists are the yahoo source's own (and the synthetic one follows them); each reads
  // them when it loads.
  ListSpec lists{list_directory, market_names};
  // --source takes one name or several, separated by commas ("nordnet,funds:NAME"): they run at once, each on a thread of its own.
  std::vector<std::unique_ptr<Source>> sources;
  for (size_t from = 0; from <= source_name.size();) {
    size_t comma = source_name.find(',', from);
    if (comma == std::string::npos) comma = source_name.size();
    const std::string one = source_name.substr(from, comma - from);
    std::string schema_error;
    std::unique_ptr<Source> source = makeSource(one, lists, schema_error);
    if (!source) return fail(2, "stocks: " + (schema_error.empty() ? "unknown --source " + one + " (there are " + std::string(kSourceNames) + ")" : schema_error));
    sources.push_back(std::move(source));
    from = comma + 1;
  }
  if (const std::string clash = datasetCodeClash(sources); !clash.empty()) return fail(2, "stocks: " + clash);

  // What the sources said of the nodes is read from the file before they run. What goes wrong with the file is a message on
  // the status line once the display is up.
  struct FactMessages {
    std::mutex mutex;
    std::vector<std::string> early;         // said before the display is up
    std::function<void(std::string)> post;  // once it is: to the status line, from any thread
    void say(std::string text) {
      std::lock_guard lock(mutex);
      if (post) post(std::move(text));
      else early.push_back(std::move(text));
    }
  };
  auto fact_messages = std::make_shared<FactMessages>();
  StoreOptions store_options;
  // What the store refuses (a MIC that two markets would have) is said once, on the status line.
  store_options.notice = [fact_messages](const std::string & text) { fact_messages->say("facts: " + text); };
  if (keep_facts) {
    const std::filesystem::path file = facts_file.empty() ? defaultFactsFile() : std::filesystem::path(facts_file);
    if (!file.empty()) store_options.facts = openSqliteFactStore(file, [fact_messages](const std::string & text) { fact_messages->say("facts: " + text); });
  }
  std::shared_ptr<Store> store = makeMemoryStore(std::move(store_options));
  // The loader and the sources run on threads of their own and tell the UI thread what happened.
  cellui::ThreadInbox inbox(app->backend().eventLoop(), app->widgets());
  {
    std::lock_guard lock(fact_messages->mutex);
    fact_messages->post = [&inbox](std::string text) { inbox.post(SourceMessage{std::move(text)}); };
    for (std::string & text : fact_messages->early) fact_messages->post(std::move(text));
    fact_messages->early.clear();
  }
  // Declared after the inbox, so gone before it: nothing says anything to a display that is gone.
  struct StopPosting {
    FactMessages & messages;
    ~StopPosting() {
      std::lock_guard lock(messages.mutex);
      messages.post = nullptr;
    }
  } stop_posting{*fact_messages};
  auto icons = std::make_shared<IconLoader>(makeHttp(), IconLoader::Options{.cache_dir = defaultIconCacheDir()}, [&inbox] { inbox.post(IconsLoaded{}); });
  auto history = std::make_shared<HistoryLoader>(
      store, makeHttp(), HistoryLoader::Options{}, [&inbox] { inbox.post(StoreChanged{}); },
      [&inbox](std::string text) { inbox.post(SourceMessage{std::move(text)}); });
  // A source that has the history of what it found, but not with the rest, hands the loader what loads it when it is wanted. This is
  // before any source has run: "auto" hands over one that follows its choice, so it is asked for once, here.
  for (const auto & s : sources) history->addProvider(s->historyProvider());
  Browser browser(*app, store, icons, history);
  SourceRunner runner(
      store, std::move(sources), [&inbox] { inbox.post(StoreChanged{}); },
      [&inbox](std::string text) { inbox.post(SourceMessage{std::move(text)}); });
  const int status = app->run();
  store->flush();  // what the sources last said is on disk before the program ends
  return status;
}
