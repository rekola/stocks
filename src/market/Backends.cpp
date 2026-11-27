#include "market/Backends.h"

#include "model/SqliteBarStore.h"
#include "model/SqliteSummaryStore.h"

#ifdef MARKET_WITH_MARIADB
#include "model/MariaDbStores.h"
#endif

#include <filesystem>
#include <stdexcept>

namespace market {

namespace {

#ifdef MARKET_WITH_MARIADB
maria::Config mariaOf(const DbConfig & db) {
  maria::Config c;
  c.host = db.host;
  c.port = db.port;
  c.socket = db.socket;
  c.user = db.user;
  c.password = db.password;
  c.database = db.name;
  return c;
}
#endif

}  // namespace

bool hasBackend(const std::string & type) {
  if (type == "sqlite") return true;
#ifdef MARKET_WITH_MARIADB
  if (type == "mariadb") return true;
#endif
  return false;
}

StoreOptions storeOptionsOf(const DbConfig & db, const FactLog & log) {
  StoreOptions options;
  options.invent_isins = false;  // as the application's: the adapters name the ISINs they supply (source/KnownIsins.h)
  if (db.type == "sqlite") {
    const std::filesystem::path directory = db.path;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) throw std::runtime_error("cannot make " + directory.string() + ": " + ec.message());
    options.facts = openSqliteFactStore(directory / "facts.sqlite", log);
    options.summaries = openSqliteSummaries(directory / "summaries", log);
    options.bars = openSqliteBarStore(directory / "bars.sqlite", log);
    return options;
  }
#ifdef MARKET_WITH_MARIADB
  if (db.type == "mariadb") {
    const maria::Config config = mariaOf(db);
    maria::createDatabase(config);
    options.facts = openMariaDbFactStore(config, log);
    options.summaries = openMariaDbSummaries(config, log);
    options.bars = openMariaDbBarStore(config, log);
    return options;
  }
#endif
  throw std::runtime_error("this build has no \"" + db.type + "\" database (\"mariadb\" needs libmariadb)");
}

void dropBackends(const DbConfig & db) {
  if (db.type == "sqlite") {
    std::error_code ec;
    std::filesystem::remove_all(db.path, ec);
    return;
  }
#ifdef MARKET_WITH_MARIADB
  if (db.type == "mariadb") maria::dropDatabase(mariaOf(db));
#endif
}

}  // namespace market
