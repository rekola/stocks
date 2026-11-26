#pragma once

#include "market/Config.h"
#include "market/Dataset.h"
#include "market/Store.h"

#include <memory>

// The two ways the market data is kept (see SqlStore.h): in a SQLite file, or in a MariaDB schema of its own.
namespace market {

enum class OpenMode {
  ReadWrite,  // makes the schema and the tables if they are not there, and starts over (drops its own tables) when they are another version
  ReadOnly    // only checks that the tables are there and of this version
};

// Opens what the config's `type` says, or throws std::runtime_error with what the database said. A reset of the tables is said to `log`.
std::unique_ptr<MarketStore> openStore(const DbConfig &, OpenMode, const Log & log = {});
// Whether a backend was built in ("sqlite", "mariadb").
bool hasBackend(const std::string & type);
// Drops what the config names (the SQLite file, the MariaDB schema): for the tests' scratch databases.
void dropStore(const DbConfig &);

}  // namespace market
