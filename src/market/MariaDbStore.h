#pragma once

#include "market/Config.h"
#include "market/Dataset.h"
#include "market/Store.h"

#include <memory>

// The market data in MariaDB (the catalog's database in plans/server.md; the bars sit beside it for now, see "Bars" there). One
// schema (database) of its own, with these tables, no auto-increment key on any of them but the batch counter:
//
//   meta         name, value                    schema_version
//   datasets     dataset (key), code, name      what says the bars: a dataset's key is made from its code (NodeKey.h)
//   batches      version, dataset, started_at, note   one ingest run: the version its bars carry
//   instruments  the edge's four keys, code, name, asset_class, first_period
//   loads        the edge, resolution, version, loaded_at, bar_count, first_key, last_key, last_bar_time    what a level holds
//   bars         the edge, resolution, bar_key, open, high, low, close, version                              one row per bar
//
// The edge is base, quote, market, dataset (the keys of NodeKey.h; a currency pair's two nodes are currencies, a market XXXX is
// none). Times are UTC seconds, bar keys are those of market/Bars.h, the resolution is the model's `Resolution` code.
namespace market {

enum class OpenMode {
  ReadWrite,  // makes the schema and the tables if they are not there, and starts over (drops its own tables) when they are another version
  ReadOnly    // only checks that the tables are there and of this version
};

// Connects, or throws std::runtime_error with what the server said. A reset of the tables is said to `log`.
std::unique_ptr<MarketStore> openMariaDbStore(const DbConfig &, OpenMode, const Log & log = {});
// Drops the whole schema (database) of the config: for the tests' scratch databases.
void dropMariaDbSchema(const DbConfig &);

}  // namespace market
