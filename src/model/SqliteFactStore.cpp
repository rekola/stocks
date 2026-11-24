#include "model/SqliteFactStore.h"

#include "model/NodeKey.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {

namespace fs = std::filesystem;

constexpr int kSchemaVersion = 5;  // 5: a value may be a link to a node (`tag` in place of `is_bool`); 4: private ISINs are a hash of the exact text
// What a fact's value is beyond what SQLite says of it: an integer that is a truth value, or one that is the key of a node.
constexpr int kPlain = 0, kBoolean = 1, kNode = 2;
// SQLite's result codes for a damaged database and for a file that is none (kCorrupt, kNotADatabase): SQLiteCpp does
// not show sqlite3.h, and nothing of SQLite itself is called from here.
constexpr int kCorrupt = 11, kNotADatabase = 26;

// A file that cannot be used as it is: damaged or not a database.
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
      SQLite::Statement q(*db_, "SELECT node_key, predicate, value, tag, updated FROM facts");
      while (q.executeStep()) {
        StoredFact f;
        f.node = static_cast<uint64_t>(q.getColumn(0).getInt64());
        if (f.node == 0) continue;
        f.predicate = q.getColumn(1).getString();
        const SQLite::Column value = q.getColumn(2);
        if (value.isNull())
          f.value = std::monostate{};
        else if (value.isText())
          f.value = value.getString();
        else if (value.isFloat())
          f.value = value.getDouble();
        else if (value.isInteger()) {
          const int tag = q.getColumn(3).getInt();
          if (tag == kBoolean)
            f.value = value.getInt64() != 0;
          else if (tag == kNode) {
            if (value.getInt64() <= 0) continue;  // no key
            f.value = NodeKey{static_cast<uint64_t>(value.getInt64())};
          } else
            f.value = value.getInt64();
        } else
          continue;  // a blob: not ours
        f.updated = q.getColumn(4).getInt64();
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
      SQLite::Statement put(*db_,
                            "INSERT INTO facts (node_key, predicate, value, tag, updated) "
                            "VALUES (?, ?, ?, ?, ?) "
                            "ON CONFLICT (node_key, predicate) DO UPDATE SET "
                            "value = excluded.value, tag = excluded.tag, updated = excluded.updated");
      for (const auto * list : {&batch.changed, &batch.rechecked})
        for (const StoredFact & f : *list) {
          put.bind(1, static_cast<int64_t>(f.node));
          put.bind(2, f.predicate);
          int32_t tag = kPlain;
          if (const auto * t = std::get_if<std::string>(&f.value))
            put.bind(3, *t);
          else if (const auto * i = std::get_if<int64_t>(&f.value))
            put.bind(3, *i);
          else if (const auto * d = std::get_if<double>(&f.value))
            put.bind(3, *d);
          else if (const auto * b = std::get_if<bool>(&f.value)) {
            put.bind(3, static_cast<int32_t>(*b ? 1 : 0));
            tag = kBoolean;
          } else if (const auto * k = std::get_if<NodeKey>(&f.value)) {
            put.bind(3, static_cast<int64_t>(k->value));
            tag = kNode;
          } else
            put.bind(3);
          put.bind(4, tag);
          put.bind(5, static_cast<int64_t>(f.updated));
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
    // The first thing read from a file that is no database throws.
    const int version = db_->execAndGet("PRAGMA user_version").getInt();
    db_->exec("PRAGMA journal_mode = WAL");
    db_->exec("PRAGMA synchronous = NORMAL");
    if (db_->execAndGet("PRAGMA quick_check(1)").getString() != "ok") throw Unusable{"it is damaged"};
    if (version != kSchemaVersion) startOver(version);
    createTable();
  }

  // A file of another schema, older or newer, is emptied in place: the sources say everything again.
  void startOver(int version) {
    SQLite::Transaction transaction(*db_);
    std::vector<std::string> tables;
    {
      SQLite::Statement q(*db_, "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'");
      while (q.executeStep()) tables.push_back(q.getColumn(0).getString());
    }
    for (const std::string & table : tables) db_->exec("DROP TABLE \"" + table + "\"");
    db_->exec("PRAGMA user_version = " + std::to_string(kSchemaVersion));
    transaction.commit();
    if (!tables.empty()) say(file_.string() + ": schema " + std::to_string(version) + ", not this program's; starting over");
  }

  // `value` has no declared type, so SQLite keeps what it is given (a real that is whole stays real); a truth value is an
  // integer with `tag` 1, and a link the integer of the key with `tag` 2. The key of the node is its number (NodeKey.h), whose top bits say what kind of node it is.
  void createTable() {
    db_->exec(
        "CREATE TABLE IF NOT EXISTS facts ("
        "node_key INTEGER NOT NULL, predicate TEXT NOT NULL, "
        "value, tag INTEGER NOT NULL DEFAULT 0, updated INTEGER NOT NULL, "
        "PRIMARY KEY (node_key, predicate)) WITHOUT ROWID");
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
