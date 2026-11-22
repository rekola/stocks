#include "model/SqliteFactStore.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {

namespace fs = std::filesystem;

constexpr int kSchemaVersion = 1;
// SQLite's result codes for a damaged database and for a file that is none (kCorrupt, kNotADatabase): SQLiteCpp does
// not show sqlite3.h, and nothing of SQLite itself is called from here.
constexpr int kCorrupt = 11, kNotADatabase = 26;

// A file that cannot be used as it is: damaged, not a database, or made by a newer program.
struct Unusable {
  std::string why;
};

class SqliteFactStore : public FactStore {
 public:
  SqliteFactStore(fs::path file, FactLog log) : file_(std::move(file)), log_(std::move(log)) {
    std::error_code ec;
    if (file_.has_parent_path()) fs::create_directories(file_.parent_path(), ec);
    for (int attempt = 0; attempt < 2 && !db_; attempt++) {
      try {
        open();
      } catch (const Unusable & u) {
        db_.reset();
        if (!moveAside(u.why)) return;
      } catch (const SQLite::Exception & e) {
        db_.reset();
        if (e.getErrorCode() == kCorrupt || e.getErrorCode() == kNotADatabase) {
          if (!moveAside(e.what())) return;
        } else {
          say(std::string("cannot open ") + file_.string() + ": " + e.what() + "; facts are not kept");
          return;
        }
      } catch (const std::exception & e) {
        db_.reset();
        say(std::string("cannot open ") + file_.string() + ": " + e.what() + "; facts are not kept");
        return;
      }
    }
  }

  std::vector<StoredFact> loadAll() override {
    std::vector<StoredFact> out;
    if (!db_) return out;
    try {
      SQLite::Statement q(*db_, "SELECT node_type, node_key, predicate, value, is_bool, updated FROM facts");
      while (q.executeStep()) {
        StoredFact f;
        const int type = q.getColumn(0).getInt();
        if (type < 0 || type > static_cast<int>(NodeType::Dataset)) continue;
        f.type = static_cast<NodeType>(type);
        f.node = q.getColumn(1).getString();
        f.predicate = q.getColumn(2).getString();
        const SQLite::Column value = q.getColumn(3);
        if (value.isNull())
          f.value = std::monostate{};
        else if (value.isText())
          f.value = value.getString();
        else if (value.isFloat())
          f.value = value.getDouble();
        else if (value.isInteger()) {
          if (q.getColumn(4).getInt() != 0)
            f.value = value.getInt64() != 0;
          else
            f.value = value.getInt64();
        } else
          continue;  // a blob: not ours
        f.updated = q.getColumn(5).getInt64();
        out.push_back(std::move(f));
      }
    } catch (const std::exception & e) {
      say(std::string("cannot read ") + file_.string() + ": " + e.what());
      out.clear();
    }
    return out;
  }

  void apply(const FactBatch & batch) override {
    if (!db_ || batch.empty()) return;
    try {
      SQLite::Transaction transaction(*db_);
      SQLite::Statement forget(*db_, "DELETE FROM facts WHERE node_type = ? AND node_key = ?");
      for (const StoredNode & n : batch.forget) {
        forget.bind(1, static_cast<int32_t>(n.type));
        forget.bind(2, n.key);
        forget.exec();
        forget.reset();
      }
      SQLite::Statement put(*db_,
                            "INSERT INTO facts (node_type, node_key, predicate, value, is_bool, updated) "
                            "VALUES (?, ?, ?, ?, ?, ?) "
                            "ON CONFLICT (node_type, node_key, predicate) DO UPDATE SET "
                            "value = excluded.value, is_bool = excluded.is_bool, updated = excluded.updated");
      for (const auto * list : {&batch.changed, &batch.rechecked})
        for (const StoredFact & f : *list) {
          put.bind(1, static_cast<int32_t>(f.type));
          put.bind(2, f.node);
          put.bind(3, f.predicate);
          int32_t is_bool = 0;
          if (const auto * t = std::get_if<std::string>(&f.value))
            put.bind(4, *t);
          else if (const auto * i = std::get_if<int64_t>(&f.value))
            put.bind(4, *i);
          else if (const auto * d = std::get_if<double>(&f.value))
            put.bind(4, *d);
          else if (const auto * b = std::get_if<bool>(&f.value)) {
            put.bind(4, static_cast<int32_t>(*b ? 1 : 0));
            is_bool = 1;
          } else
            put.bind(4);
          put.bind(5, is_bool);
          put.bind(6, static_cast<int64_t>(f.updated));
          put.exec();
          put.reset();
        }
      transaction.commit();
    } catch (const std::exception & e) {
      say(std::string("cannot write ") + file_.string() + ": " + e.what());
    }
  }

 private:
  void say(const std::string & text) const {
    if (log_)
      log_(text);
    else
      std::fprintf(stderr, "stocks: %s\n", text.c_str());
  }

  void open() {
    db_ = std::make_unique<SQLite::Database>(file_.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db_->setBusyTimeout(2000);
    // The first thing read from a file that is no database throws; a newer schema is not ours to read or change.
    const int version = db_->execAndGet("PRAGMA user_version").getInt();
    if (version > kSchemaVersion) throw Unusable{"its schema (" + std::to_string(version) + ") is newer than this program's"};
    db_->exec("PRAGMA journal_mode = WAL");
    db_->exec("PRAGMA synchronous = NORMAL");
    if (db_->execAndGet("PRAGMA quick_check(1)").getString() != "ok") throw Unusable{"it is damaged"};
    // `value` has no declared type, so SQLite keeps what it is given (a real that is whole stays real); a truth value is an
    // integer with `is_bool` set.
    db_->exec(
        "CREATE TABLE IF NOT EXISTS facts ("
        "node_type INTEGER NOT NULL, node_key TEXT NOT NULL, predicate TEXT NOT NULL, "
        "value, is_bool INTEGER NOT NULL DEFAULT 0, updated INTEGER NOT NULL, "
        "PRIMARY KEY (node_type, node_key, predicate)) WITHOUT ROWID");
    if (version < kSchemaVersion) db_->exec("PRAGMA user_version = " + std::to_string(kSchemaVersion));
  }

  // Puts the file (and what WAL mode keeps beside it) out of the way; false where that cannot be done.
  bool moveAside(const std::string & why) {
    db_.reset();
    const std::string suffix =
        ".bad-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    bool moved = true;
    for (const char * extra : {"", "-wal", "-shm"}) {
      fs::path from = file_;
      from += extra;
      std::error_code ec;
      if (!fs::exists(from, ec)) continue;
      fs::path to = from;
      to += suffix;
      fs::rename(from, to, ec);
      if (ec) {
        fs::remove(from, ec);
        if (ec) moved = false;
      }
    }
    say(file_.string() + " could not be used (" + why + "); " + (moved ? "it was moved aside and a new one is started" : "it could not be moved aside, so facts are not kept"));
    return moved;
  }

  fs::path file_;
  FactLog log_;
  std::unique_ptr<SQLite::Database> db_;
};

}  // namespace

std::shared_ptr<FactStore> openSqliteFactStore(const std::filesystem::path & file, FactLog log) {
  return std::make_shared<SqliteFactStore>(file, std::move(log));
}

std::filesystem::path defaultFactsFile() {
#ifdef _WIN32
  if (const char * base = std::getenv("LOCALAPPDATA"); base && *base) return fs::path(base) / "stocks" / "facts.sqlite";
#else
  if (const char * base = std::getenv("XDG_DATA_HOME"); base && *base) return fs::path(base) / "stocks" / "facts.sqlite";
  if (const char * home = std::getenv("HOME"); home && *home) return fs::path(home) / ".local" / "share" / "stocks" / "facts.sqlite";
#endif
  return {};
}
