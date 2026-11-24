#pragma once

#include "model/SqliteFactStore.h"
#include "model/Summary.h"

#include <filesystem>
#include <memory>

// The summaries of one dataset in a SQLite file: an `edges` table (one row per edge: its listing, quote and close before the session) and
// a `queues` table (one row per edge and resolution: when it was fetched, how many bars, the keys of its first and last, and the bars as
// a blob, see SummaryRecords.h). WAL mode, a transaction per batch, the schema's version in `PRAGMA user_version`. Writes go through
// one connection and reads through another, so a read never waits for a batch to be written.
//
// It never throws. A file that is not a database or is damaged is moved aside (next to it, with `.bad-<time>` added) and a new one made;
// a file of another schema is emptied and started over (nothing is migrated, and `log` says so); where the file cannot be made at all
// (no directory, no room, no rights) nothing is kept and `log` says so. SQLiteCpp is used in the .cpp and in no header.
std::shared_ptr<SummaryStore> openSqliteSummaryStore(const std::filesystem::path & file, DatasetId dataset, FactLog log = {});

// A directory of them, `<dataset code>.sqlite` in each. An empty directory path keeps nothing: `open` gives none.
std::shared_ptr<SummaryStores> openSqliteSummaries(const std::filesystem::path & directory, FactLog log = {});

// Where the summaries are kept unless the user says otherwise: `summaries` beside the facts file. Empty where there is none to be found.
std::filesystem::path defaultSummariesDirectory();
