#include "market/SqlStores.h"

#include <stdexcept>

namespace market {

std::unique_ptr<MarketStore> openSqliteStore(const DbConfig &, OpenMode, const Log &);
void dropSqliteStore(const DbConfig &);
#ifdef MARKET_WITH_MARIADB
std::unique_ptr<MarketStore> openMariaDbStore(const DbConfig &, OpenMode, const Log &);
void dropMariaDbStore(const DbConfig &);
#endif

bool hasBackend(const std::string & type) {
  if (type == "sqlite") return true;
#ifdef MARKET_WITH_MARIADB
  if (type == "mariadb") return true;
#endif
  return false;
}

std::unique_ptr<MarketStore> openStore(const DbConfig & config, OpenMode mode, const Log & log) {
  if (config.type == "sqlite") return openSqliteStore(config, mode, log);
#ifdef MARKET_WITH_MARIADB
  if (config.type == "mariadb") return openMariaDbStore(config, mode, log);
#endif
  throw std::runtime_error("this build has no \"" + config.type + "\" database (it needs libmariadb)");
}

void dropStore(const DbConfig & config) {
  if (config.type == "sqlite") return dropSqliteStore(config);
#ifdef MARKET_WITH_MARIADB
  if (config.type == "mariadb") return dropMariaDbStore(config);
#endif
}

}  // namespace market
