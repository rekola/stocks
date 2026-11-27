#pragma once

#include "model/BarStore.h"
#include "model/SqliteFactStore.h"

#include <filesystem>
#include <memory>

// Every bar of every series in one SQLite file: a `bars` table with a row for each (edge, resolution, time), kept in the order of its
// primary key, so that a range of a series is one run of the file. WAL mode, a transaction per batch, the schema's version in
// `PRAGMA user_version`; a file of another schema is emptied and started over (`log` says so). Never throws: where the file cannot be made nothing is
// kept and `log` says so. SQLiteCpp is used in the .cpp and in no header.
std::shared_ptr<BarStore> openSqliteBarStore(const std::filesystem::path & file, FactLog log = {});
