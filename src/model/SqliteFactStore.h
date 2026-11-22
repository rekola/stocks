#pragma once

#include "model/Facts.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// Says what went wrong or was done about it ("the file was not a database and was moved aside"). Called on whatever thread
// was using the file.
using FactLog = std::function<void(const std::string &)>;

// The facts in a SQLite file: one table keyed by (node type, node key, predicate), in WAL mode, a transaction per batch. The
// schema's version is in `PRAGMA user_version`.
//
// It never fails and never throws. A file that is not a database, is damaged or is of a newer schema is moved aside (next to
// it, with `.bad-<time>` added) and a new one made; where the file cannot be made at all (no directory, no room, no rights)
// the store keeps nothing and says so through `log`. SQLiteCpp is used here and in no header.
std::shared_ptr<FactStore> openSqliteFactStore(const std::filesystem::path & file, FactLog log = {});

// Where the facts are kept unless the user says otherwise: `facts.sqlite` in the user's data directory (XDG_DATA_HOME or
// ~/.local/share/stocks on Linux and macOS, %LOCALAPPDATA%\stocks on Windows). Empty where there is none to be found.
std::filesystem::path defaultFactsFile();
