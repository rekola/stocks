#include "market/SqlStore.h"

#include <sqlpp11/sqlite3/sqlite3.h>

#include <filesystem>

namespace market {

namespace {

struct SqliteBackend {
  using Connection = sqlpp::sqlite3::connection;
  static constexpr const char * kTableTail = "";
  static constexpr const char * kClusteredTableTail = " WITHOUT ROWID";  // the rows are kept in the order of the primary key

  static std::unique_ptr<Connection> connect(const DbConfig & config, bool write) {
    sqlpp::sqlite3::connection_config c;
    c.path_to_database = config.path;
    c.flags = write ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READONLY;
    auto db = std::make_unique<Connection>(c);
    db->execute("PRAGMA busy_timeout = 10000");  // the server reads while an ingest writes
    if (write) db->execute("PRAGMA journal_mode = WAL");
    return db;
  }
  static std::string describe(const DbConfig & config) { return "the SQLite database " + config.path; }
};

}  // namespace

std::unique_ptr<MarketStore> openSqliteStore(const DbConfig & config, OpenMode mode, const Log & log) {
  return std::make_unique<SqlStore<SqliteBackend>>(config, mode, log);
}

void dropSqliteStore(const DbConfig & config) {
  if (config.path == ":memory:") return;
  std::error_code ec;
  for (const char * suffix : {"", "-wal", "-shm"}) std::filesystem::remove(config.path + suffix, ec);
}

}  // namespace market
