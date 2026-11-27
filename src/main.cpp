// The stock browser: cellui's Table, TagBar and trace chart over a store of
// candles that data sources fill on a thread of their own.
//   stocks                 on the display cellui picks, with Nordnet's data, or
//   Yahoo's if Nordnet gives nothing, and the small loaders (spfb, ecb, every funds:SCHEMA of data/funds); --source replaces that set stocks --ui terminal   in the terminal;
//   --ui window  in a window stocks --source NAME   the data source (several,
//   comma-separated, run at once): auto (the default: nordnet, and yahoo if
//   nordnet gives nothing),
//                          nordnet, yahoo, spfb (the Bank of Finland: the Euribor rates), ecb (the €STR), goldprice (goldprice.dev: spot prices and
//                          daily bars of gold, silver and copper, in euros and dollars), gold-api (gold-api.com: spot
//                          prices of gold and other metals, in euros and dollars), physical (physical metals held in hand: a gold
//                          bar by the gram, a gold coin by the ounce, a platinum bar, estimated from the spot prices another
//                          source gives; it runs beside the default), sifter (the Sifter Fund's share classes), funds:SCHEMA (a fund company: a
//                          schema in data/funds, or a schema file) or synthetic
//   stocks --lists DIR     the instrument lists of the yahoo and synthetic
//   sources (default: data/lists) stocks --facts FILE    where what the sources
//   said of instruments, markets and datasets is kept between runs (default:
//   facts.sqlite in the
//                          user's data directory); --no-facts runs without it
//   stocks --portfolios FILE  where the user's own portfolios are kept (default: portfolios.sqlite beside the facts file);
//                          --no-portfolios keeps them for the session only
//   stocks --summaries DIR  where the summaries of the edges (the last price, bid and ask and the queues behind the sparklines) are kept
//                          between runs, one file for each dataset (default: summaries/ beside the facts file); --no-summaries runs without
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
#include "model/SqliteSummaryStore.h"
#include "source/AutoSource.h"
#include "source/SourceRunner.h"
#include "source/NordnetSource.h"
#include "source/FundSource.h"
#include "source/EcbSource.h"
#include "source/GoldApiSource.h"
#include "source/GoldPriceSource.h"
#include "source/PhysicalSource.h"
#include "source/SifterSource.h"
#include "source/SuomenPankkiSource.h"
#include "source/YahooSource.h"
#include "source/DataDirectory.h"

#include <cellui/Application.h>
#include <cellui/ThreadPosts.h>

#include <algorithm>
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
  if (name == "ecb") return std::make_unique<EcbSource>();
  if (name == "gold-api") return std::make_unique<GoldApiSource>();
  if (name == "goldprice") return std::make_unique<GoldPriceSource>();
  if (name == "physical") {
    PhysicalTable table;
    if (!loadPhysicalTable(table, error)) {
      error = "physical: " + error;
      return nullptr;
    }
    return std::make_unique<PhysicalSource>(std::move(table));
  }
  if (name == "sifter") return std::make_unique<SifterSource>();
  if (name == "spfb") return std::make_unique<SuomenPankkiSource>();
  if (name == "synthetic") return std::make_unique<SyntheticSource>(lists);
  return nullptr;
}

// What runs when no --source is given: the market data ("auto") and the small loaders that have no options of their own (the rates, the
// €STR, the spot prices of goldprice.dev and gold-api.com, the estimate of the physical metals and every fund company schema of data/funds), so they are there without being asked for.
std::string defaultSources() {
  std::string names = "auto,spfb,ecb,goldprice,gold-api,physical,sifter";  // physical estimates the metals held in hand from the spot prices the others give
  std::vector<std::string> funds;
  std::error_code ec;
  const std::filesystem::path dir = findDataDirectory("funds");
  if (!dir.empty())
    for (const auto & entry : std::filesystem::directory_iterator(dir, ec))
      if (entry.is_regular_file(ec) && entry.path().extension() == ".json") funds.push_back(entry.path().stem().string());
  std::sort(funds.begin(), funds.end());
  for (const std::string & name : funds) names += ",funds:" + name;
  return names;
}

constexpr const char * kSourceNames = "auto, nordnet, yahoo, spfb, ecb, goldprice, gold-api, physical, sifter, funds:SCHEMA, synthetic";

}  // namespace

int main(int argc, char ** argv) {
  std::string error;
  cellui::ApplicationOptions app_options;
  app_options.title = "stocks";
  auto app = cellui::Application::open(argc, argv, std::move(app_options), &error);
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
  std::string source_name;  // empty: defaultSources()
  std::string list_directory;
  std::vector<std::string> market_names;
  std::string facts_file;
  bool keep_facts = true;
  std::string portfolios_file;
  bool keep_portfolios = true;
  std::string summaries_directory;
  bool keep_summaries = true;
  const auto & args = app->arguments();
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--source" && i + 1 < args.size()) source_name = args[++i];
    else if (args[i].rfind("--source=", 0) == 0) source_name = args[i].substr(9);
    else if (args[i] == "--market" && i + 1 < args.size()) market_names.push_back(args[++i]);
    else if (args[i].rfind("--market=", 0) == 0) market_names.push_back(args[i].substr(9));
    else if (args[i] == "--facts" && i + 1 < args.size()) facts_file = args[++i];
    else if (args[i].rfind("--facts=", 0) == 0) facts_file = args[i].substr(8);
    else if (args[i] == "--no-facts") keep_facts = false;
    else if (args[i] == "--portfolios" && i + 1 < args.size()) portfolios_file = args[++i];
    else if (args[i].rfind("--portfolios=", 0) == 0) portfolios_file = args[i].substr(13);
    else if (args[i] == "--no-portfolios") keep_portfolios = false;
    else if (args[i] == "--summaries" && i + 1 < args.size()) summaries_directory = args[++i];
    else if (args[i].rfind("--summaries=", 0) == 0) summaries_directory = args[i].substr(12);
    else if (args[i] == "--no-summaries") keep_summaries = false;
    else if (args[i] == "--lists" && i + 1 < args.size()) list_directory = args[++i];
    else if (args[i].rfind("--lists=", 0) == 0) list_directory = args[i].substr(8);
    else {
      return fail(
          2,
          std::string("usage: ") + argv[0] +
              " [--ui auto|terminal|window] [--theme original|dark|light] "
              "[--source " +
              kSourceNames +
              "] [--facts FILE | --no-facts] [--portfolios FILE | --no-portfolios] [--summaries DIR | --no-summaries] [--lists DIR] [--market MIC]...");
    }
  }
  // One source, for now: the instrument lists are the yahoo source's own (and the synthetic one follows them); each reads
  // them when it loads.
  if (source_name.empty()) source_name = defaultSources();
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
  // What the store's own thread says when it has read queues into the store (a trace to draw): to the UI thread, once there is one.
  struct Changed {
    std::mutex mutex;
    std::function<void()> post;
    void say() {
      std::lock_guard lock(mutex);
      if (post) post();
    }
  };
  auto store_changed = std::make_shared<Changed>();
  StoreOptions store_options;
  // The adapters give the ISINs of what their source gives none for (source/KnownIsins.h): an instrument that comes with none is not kept, and says so.
  store_options.invent_isins = false;
  // What the store refuses (a MIC that two markets would have) is said once, on the status line.
  store_options.changed = [store_changed] { store_changed->say(); };
  // The screen is up at once: the facts, the edges and their prices are read on a thread of the store's own, the portfolios' first.
  store_options.restore = StoreOptions::Restore::InBackground;
  store_options.notice = [fact_messages](const std::string & text) { fact_messages->say("facts: " + text); };
  if (keep_facts) {
    const std::filesystem::path file = facts_file.empty() ? defaultFactsFile() : std::filesystem::path(facts_file);
    if (!file.empty()) store_options.facts = openSqliteFactStore(file, [fact_messages](const std::string & text) { fact_messages->say("facts: " + text); });
  }
  // The user's own file, a second one: portfolios are never in the cache above, and no source can write one.
  if (keep_portfolios) {
    const std::filesystem::path file = portfolios_file.empty() ? defaultPortfoliosFile() : std::filesystem::path(portfolios_file);
    if (!file.empty())
      store_options.portfolios = openSqlitePortfolioStore(file, [fact_messages](const std::string & text) { fact_messages->say("portfolios: " + text); });
  }
  // The summaries of the edges (the quote and the queues behind the sparklines), a file for each dataset in a directory of their own.
  if (keep_summaries) {
    const std::filesystem::path directory = summaries_directory.empty() ? defaultSummariesDirectory() : std::filesystem::path(summaries_directory);
    if (!directory.empty()) store_options.summaries = openSqliteSummaries(directory, [fact_messages](const std::string & text) { fact_messages->say("summaries: " + text); });
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
  {
    std::lock_guard lock(store_changed->mutex);
    store_changed->post = [&inbox] { inbox.post(StoreChanged{}); };
  }
  // Declared after the inbox, so gone before it: nothing says anything to a display that is gone.
  struct StopPosting {
    FactMessages & messages;
    Changed & changed;
    ~StopPosting() {
      {
        std::lock_guard lock(messages.mutex);
        messages.post = nullptr;
      }
      std::lock_guard lock(changed.mutex);
      changed.post = nullptr;
    }
  } stop_posting{*fact_messages, *store_changed};
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
