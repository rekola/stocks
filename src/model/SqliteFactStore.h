#pragma once

#include "model/Facts.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// Says what went wrong or was done about it ("the file was not a database and was moved aside"). Called on whatever thread
// was using the file.
using FactLog = std::function<void(const std::string &)>;

// The facts in a SQLite file: one table keyed by (node key, predicate), in WAL mode, a transaction per batch. The
// schema's version is in `PRAGMA user_version`.
//
// It never fails and never throws. A file that is not a database or is damaged is moved aside (next to
// it, with `.bad-<time>` added) and a new one made. A file of another schema (older or newer) is emptied in place and started over, with a line in `log`; where the file cannot be made at all (no directory, no room, no rights)
// the store keeps nothing and says so through `log`. SQLiteCpp is used here and in no header.
std::shared_ptr<FactStore> openSqliteFactStore(const std::filesystem::path & file, FactLog log = {});

// The user's own file: the facts of the portfolio nodes and their transactions (events), in `facts` and `events` tables, with a schema
// version of its own (it shares none with the cache). Writes are synchronous, each on the disk when it returns. It is the only copy of
// what the user typed, so it is never dropped in place: a file of another schema is moved aside (`.old-<time>`, with its WAL files) and
// a damaged one or one that is no database to `.bad-<time>`, a new one is started, and `log` says so. Where no file can be made, or the
// old one cannot be moved, nothing is kept and `log` says so. It never throws.
std::shared_ptr<FactStore> openSqlitePortfolioStore(const std::filesystem::path & file, FactLog log = {});

// Where the facts are kept unless the user says otherwise: `facts.sqlite` in the user's data directory (XDG_DATA_HOME or
// ~/.local/share/stocks on Linux and macOS, %LOCALAPPDATA%\stocks on Windows). Empty where there is none to be found.
std::filesystem::path defaultFactsFile();
// `portfolios.sqlite` beside it.
std::filesystem::path defaultPortfoliosFile();
