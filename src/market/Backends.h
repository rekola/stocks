#pragma once

#include "market/Config.h"
#include "model/SqliteFactStore.h"
#include "model/Store.h"

#include <string>

// Where the market-data programs keep what the Store writes: the node attributes (facts), the summaries of the edges (the quote and the queues
// the sparklines draw) and every bar. Each is the model's own store, and the two ways to keep them are the same stores the application has:
// SQLite files (a directory: facts.sqlite, bars.sqlite and summaries/<dataset>.sqlite) or one MariaDB schema (src/model/MariaDbStores.h). The
// store does not tell the difference.
namespace market {

// Whether this build has the kind of database ("sqlite", and "mariadb" where libmariadb was found).
bool hasBackend(const std::string & type);

// The options of a Store that keeps its facts, summaries and bars in the database the config names (the schema or the directory is made if
// there is none); nothing else is set. Throws std::runtime_error where it cannot be had.
StoreOptions storeOptionsOf(const DbConfig &, const FactLog & log);

// Removes what the config names (the directory, the schema), with everything in it: for the tests' scratch databases.
void dropBackends(const DbConfig &);

}  // namespace market
