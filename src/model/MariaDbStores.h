#pragma once

#include "model/BarStore.h"
#include "model/Facts.h"
#include "model/MariaDb.h"
#include "model/SqliteFactStore.h"
#include "model/Summary.h"

#include <memory>

// The stores of the model in MariaDB, one for each of the three SQLite ones (SqliteFactStore, SqliteSummaryStore, SqliteBarStore) with the
// same contents and behaviour: they never throw (what cannot be done is said to `log`, and the store carries on, keeping nothing); a table
// of another schema version is emptied and started over; nothing is migrated. They share one MariaDB schema (a database of its own) and
// keep what each SQLite file keeps, with the dataset as a column where the SQLite summaries have a file for each. A server runs the store on
// these, and the application on the SQLite ones; the model does not tell the difference.
//
//   facts       node_key, predicate, kind, text_value / int_value / real_value, updated        the node attributes
//   summary_*   edges (one row per dataset's edge) and queues (the bars of a queue as one blob, see SummaryRecords.h)
//   bars        one row per (edge, resolution, time), all of the bars said
//
// The user's own data (a portfolio's events and lists) is not kept here: it lives in the user's own file, on the user's machine.
// Needs libmariadb.
namespace maria {

// Makes the schema (database) if it is not there. Throws Error.
void createDatabase(const Config &);
// Drops it, with everything in it: for tests.
void dropDatabase(const Config &);

}  // namespace maria

std::shared_ptr<FactStore> openMariaDbFactStore(const maria::Config &, FactLog log = {});
std::shared_ptr<SummaryStores> openMariaDbSummaries(const maria::Config &, FactLog log = {});
std::shared_ptr<BarStore> openMariaDbBarStore(const maria::Config &, FactLog log = {});
