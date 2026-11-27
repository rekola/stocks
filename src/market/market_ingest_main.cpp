// market-ingest: loads an external dataset into the market-data database, through the application's own Store.
//
//   market-ingest histdata [codes...|all] [--level 5m|1h|1d|1w|all] [--refresh] [--config FILE]
//
// One run does everything for the dataset: finds its instruments, makes the instrument records that are missing, gets the files
// (from the raw cache when they are there), reads them, builds the levels and replaces what the database holds of each. Run it
// again whenever: nothing it does is not safe to repeat. The datasets there are, are listed here and nowhere else.
#include "market/Config.h"
#include "model/Store.h"
#include "market/Ingest.h"
#include "market/Backends.h"
#include "market/offline/HistData.h"
#include "source/Http.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char * kUsage =
    "usage: market-ingest DATASET [CODE...|all] [--level 5m|1h|1d|1w|all] [--refresh] [--config FILE]\n"
    "  datasets: histdata\n"
    "  --level    the one resolution of bars to build (default all: 5m for a day, 1h for 7 days, 1d for a year, 1w for 10 years)\n"
    "  --refresh  ask again for the months of the current year and for the list of pairs (past years' files are never fetched again)\n"
    "  --config   the settings file (default: $MARKET_CONFIG, else ./market.json; without one the built-in defaults)\n";

// The datasets, by the name the command line gives them.
std::unique_ptr<market::Dataset> makeDataset(const std::string & name, const market::Config & config, Http & http) {
  if (name == "histdata") return std::make_unique<histdata::HistData>(http, market::RawCache(config.histdataCacheDir));
  return nullptr;
}

}  // namespace

int main(int argc, char ** argv) {
  std::string datasetName, configPath;
  market::IngestOptions options;
  bool levelGiven = false, explicitConfig = false;
  std::vector<std::string> codes;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    auto value = [&](const char * flag, std::string & out) {
      const std::string prefix = std::string(flag) + "=";
      if (arg == flag && i + 1 < argc) {
        out = argv[++i];
        return true;
      }
      if (arg.rfind(prefix, 0) == 0) {
        out = arg.substr(prefix.size());
        return true;
      }
      return false;
    };
    std::string v;
    if (arg == "--help" || arg == "-h") {
      std::fputs(kUsage, stdout);
      return 0;
    } else if (arg == "--refresh") {
      options.refresh = true;
    } else if (value("--level", v)) {
      if (levelGiven) {
        std::fprintf(stderr, "market-ingest: --level is given once\n%s", kUsage);
        return 2;
      }
      levelGiven = true;
      if (v == "all") continue;
      const auto level = market::parseResolution(v);
      if (!level) {
        std::fprintf(stderr, "market-ingest: unknown level \"%s\" (5m, 1h, 1d, 1w or all)\n", v.c_str());
        return 2;
      }
      options.resolutions = {*level};
    } else if (value("--config", v)) {
      configPath = v;
      explicitConfig = true;
    } else if (arg.rfind("--", 0) == 0) {
      std::fprintf(stderr, "market-ingest: unknown option %s\n%s", arg.c_str(), kUsage);
      return 2;
    } else if (datasetName.empty()) {
      datasetName = arg;
    } else {
      codes.push_back(arg);
    }
  }
  if (datasetName.empty()) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  for (const std::string & code : codes) {
    if (code == "all") {
      options.codes.clear();
      codes.clear();
      break;
    }
  }
  options.codes = codes;

  if (configPath.empty()) {
    const char * env = std::getenv("MARKET_CONFIG");
    configPath = env ? env : "market.json";
    explicitConfig = env != nullptr;
  }
  market::Config config;
  std::string error;
  if (explicitConfig || std::filesystem::exists(configPath)) {
    if (!market::loadConfig(configPath, config, error)) {
      std::fprintf(stderr, "market-ingest: %s\n", error.c_str());
      return 2;
    }
  }

  auto http = makeHttp();
  auto dataset = makeDataset(datasetName, config, *http);
  if (!dataset) {
    std::fprintf(stderr, "market-ingest: unknown dataset \"%s\"\n%s", datasetName.c_str(), kUsage);
    return 2;
  }
  const market::Log log = [](const std::string & line) { std::fprintf(stderr, "%s\n", line.c_str()); };

  try {
    StoreOptions storeOptions = market::storeOptionsOf(config.database, log);
    auto store = makeMemoryStore(std::move(storeOptions));  // the application's own store, with its facts, summaries and bars kept in the database
    const market::IngestReport report = market::runIngest(*dataset, *store, options, log);
    std::fputs("\n", stdout);
    std::fputs(market::formatReport(report).c_str(), stdout);
    return report.complete() ? 0 : 1;
  } catch (const std::exception & e) {
    std::fprintf(stderr, "market-ingest: %s\n", e.what());
    return 1;
  }
}
