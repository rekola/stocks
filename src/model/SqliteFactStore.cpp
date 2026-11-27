#include "model/SqliteFactStore.h"

#include "model/LegacyNodeKey.h"
#include "model/NodeKey.h"

#include <nlohmann/json.hpp>

#include <SQLiteCpp/SQLiteCpp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>

namespace {

namespace fs = std::filesystem;

constexpr int kFactsVersion = 6;  // 6: the keys of codes are of eleven capitals or digits, base 37; 5: a value may be a link to a node (`tag` in place of `is_bool`); 4: private ISINs are a hash of the exact text
// What a fact's value is beyond what SQLite says of it: an integer that is a truth value, or one that is the key of a node.
constexpr int kPlain = 0, kBoolean = 1, kNode = 2;
// SQLite's result codes for a damaged database and for a file that is none (kCorrupt, kNotADatabase): SQLiteCpp does
// not show sqlite3.h, and nothing of SQLite itself is called from here.
constexpr int kCorrupt = 11, kNotADatabase = 26;

// The portfolio file's own schema, which shares no table and no version with the cache's: `facts` as the cache has it, and
// `events` for the transactions and `list_members` for the groups on the lists. 4: a list holds groups (market, base, quote), no dataset; 3: the lists (a file of 2 only gets the table); 2: the keys of codes are
// of eleven capitals or digits, base 37 (a file of 1 is upgraded, `upgradeV1`); 1: the first.
constexpr int kPortfoliosVersion = 4;

// A file that cannot be used as it is: damaged or not a database.
struct Unusable {
  std::string why;
};
// A file of another schema than this program's, which the user's own file is never emptied for.
struct OtherSchema {
  int version = 0;
};

// The two files this program keeps. The cache (`facts.sqlite`) is rebuilt by the sources, so a file of another schema is emptied in
// place. The portfolio file is the only copy of what the user typed: another schema is moved aside, never dropped.
enum class Kind { Cache, Portfolios };

class SqliteFactStore : public FactStore {
 public:
  SqliteFactStore(fs::path file, FactLog log, Kind kind = Kind::Cache) : kind_(kind), file_(std::move(file)), log_(std::move(log)) {
    std::error_code ec;
    if (file_.has_parent_path()) fs::create_directories(file_.parent_path(), ec);
    for (int attempt = 0; attempt < 2 && !db_; attempt++) {
      try {
        open();
      } catch (const Unusable & u) {
        db_.reset();
        if (!moveAside(".bad-", "it could not be used (" + u.why + ")")) return;
      } catch (const OtherSchema & other) {
        db_.reset();
        if (!moveAside(".old-", "it has schema " + std::to_string(other.version) + ", not this program's (" + std::to_string(schemaVersion()) + ")")) return;
      } catch (const SQLite::Exception & e) {
        db_.reset();
        if (e.getErrorCode() == kCorrupt || e.getErrorCode() == kNotADatabase) {
          if (!moveAside(".bad-", std::string("it could not be used (") + e.what() + ")")) return;
        } else {
          say(std::string("cannot open ") + file_.string() + ": " + e.what() + "; " + kept() + " are not kept");
          return;
        }
      } catch (const std::exception & e) {
        db_.reset();
        say(std::string("cannot open ") + file_.string() + ": " + e.what() + "; " + kept() + " are not kept");
        return;
      }
    }
  }

  std::vector<StoredFact> loadAll() override {
    std::vector<StoredFact> out;
    if (!db_) return out;
    std::lock_guard lock(mutex_);
    try {
      SQLite::Statement q(*db_, "SELECT node_key, predicate, value, tag, updated FROM facts");
      while (q.executeStep()) {
        StoredFact f;
        if (read(q, f)) out.push_back(std::move(f));
      }
    } catch (const std::exception & e) {
      say(std::string("cannot read ") + file_.string() + ": " + e.what());
      out.clear();
    }
    return out;
  }

  // One look-up of the primary key for each node: a restore by chunks reads only what it is about to show.
  std::vector<StoredFact> loadNodes(const std::vector<uint64_t> & nodes) override {
    std::vector<StoredFact> out;
    if (!db_) return out;
    std::lock_guard lock(mutex_);
    try {
      SQLite::Statement q(*db_, "SELECT node_key, predicate, value, tag, updated FROM facts WHERE node_key = ?");
      for (const uint64_t node : nodes) {
        q.bind(1, static_cast<int64_t>(node));
        while (q.executeStep()) {
          StoredFact f;
          if (read(q, f)) out.push_back(std::move(f));
        }
        q.reset();
      }
    } catch (const std::exception & e) {
      say(std::string("cannot read ") + file_.string() + ": " + e.what());
      out.clear();
    }
    return out;
  }

  std::vector<StoredEvent> loadEvents() override {
    std::vector<StoredEvent> out;
    if (!db_ || kind_ != Kind::Portfolios) return out;
    std::lock_guard lock(mutex_);
    try {
      SQLite::Statement q(*db_, "SELECT portfolio, dataset, id, time, body FROM events");
      while (q.executeStep()) {
        StoredEvent e;
        e.portfolio = static_cast<uint64_t>(q.getColumn(0).getInt64());
        e.dataset = static_cast<uint64_t>(q.getColumn(1).getInt64());
        e.id = static_cast<uint64_t>(q.getColumn(2).getInt64());
        e.time = q.getColumn(3).getInt64();
        e.body = q.getColumn(4).getString();
        out.push_back(std::move(e));
      }
    } catch (const std::exception & ex) {
      say(std::string("cannot read ") + file_.string() + ": " + ex.what());
      out.clear();
    }
    return out;
  }

  std::vector<StoredMember> loadMembers() override {
    std::vector<StoredMember> out;
    if (!db_ || kind_ != Kind::Portfolios) return out;
    std::lock_guard lock(mutex_);
    try {
      SQLite::Statement q(*db_, "SELECT list, base, quote, market FROM list_members");
      while (q.executeStep()) {
        StoredMember m;
        m.list = q.getColumn(0).getString();
        m.base = static_cast<uint64_t>(q.getColumn(1).getInt64());
        m.quote = static_cast<uint64_t>(q.getColumn(2).getInt64());
        m.market = static_cast<uint64_t>(q.getColumn(3).getInt64());
        out.push_back(std::move(m));
      }
    } catch (const std::exception & ex) {
      say(std::string("cannot read ") + file_.string() + ": " + ex.what());
      out.clear();
    }
    return out;
  }

  void apply(const FactBatch & batch) override {
    if (!db_ || batch.empty()) return;
    std::lock_guard lock(mutex_);
    try {
      SQLite::Transaction transaction(*db_);
      if (kind_ == Kind::Portfolios) applyUsers(batch);
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
  // One row of `facts` (node_key, predicate, value, tag, updated) as a fact; false for one that is not ours.
  static bool read(SQLite::Statement & q, StoredFact & f) {
    f.node = static_cast<uint64_t>(q.getColumn(0).getInt64());
    if (f.node == 0) return false;
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
        if (value.getInt64() <= 0) return false;  // no key
        f.value = NodeKey{static_cast<uint64_t>(value.getInt64())};
      } else
        f.value = value.getInt64();
    } else
      return false;  // a blob: not ours
    f.updated = q.getColumn(4).getInt64();
    return true;
  }

  int schemaVersion() const { return kind_ == Kind::Portfolios ? kPortfoliosVersion : kFactsVersion; }
  const char * kept() const { return kind_ == Kind::Portfolios ? "portfolios" : "facts"; }

  // What is taken back goes first, then what is put (a portfolio taken back and said again in one batch is there).
  void applyUsers(const FactBatch & batch) {
    for (uint64_t node : batch.removed_nodes) {
      SQLite::Statement facts(*db_, "DELETE FROM facts WHERE node_key = ?");
      facts.bind(1, static_cast<int64_t>(node));
      facts.exec();
      SQLite::Statement events(*db_, "DELETE FROM events WHERE portfolio = ?");
      events.bind(1, static_cast<int64_t>(node));
      events.exec();
    }
    for (const auto & [node, predicate] : batch.removed) {
      SQLite::Statement q(*db_, "DELETE FROM facts WHERE node_key = ? AND predicate = ?");
      q.bind(1, static_cast<int64_t>(node));
      q.bind(2, predicate);
      q.exec();
    }
    for (const StoredEvent & e : batch.events_removed) {
      SQLite::Statement q(*db_, "DELETE FROM events WHERE portfolio = ? AND dataset = ? AND id = ?");
      q.bind(1, static_cast<int64_t>(e.portfolio));
      q.bind(2, static_cast<int64_t>(e.dataset));
      q.bind(3, static_cast<int64_t>(e.id));
      q.exec();
    }
    for (const StoredEvent & e : batch.events) {
      SQLite::Statement q(*db_,
                          "INSERT INTO events (portfolio, dataset, id, time, body) VALUES (?, ?, ?, ?, ?) "
                          "ON CONFLICT (portfolio, dataset, id) DO UPDATE SET time = excluded.time, body = excluded.body");
      q.bind(1, static_cast<int64_t>(e.portfolio));
      q.bind(2, static_cast<int64_t>(e.dataset));
      q.bind(3, static_cast<int64_t>(e.id));
      q.bind(4, e.time);
      q.bind(5, e.body);
      q.exec();
    }
    for (const StoredMember & m : batch.members_removed) {
      SQLite::Statement q(*db_, "DELETE FROM list_members WHERE list = ? AND base = ? AND quote = ? AND market = ?");
      bindMember(q, m);
      q.exec();
    }
    for (const StoredMember & m : batch.members) {
      SQLite::Statement q(*db_, "INSERT OR IGNORE INTO list_members (list, base, quote, market) VALUES (?, ?, ?, ?)");
      bindMember(q, m);
      q.exec();
    }
  }

  static void bindMember(SQLite::Statement & q, const StoredMember & m) {
    q.bind(1, m.list);
    q.bind(2, static_cast<int64_t>(m.base));
    q.bind(3, static_cast<int64_t>(m.quote));
    q.bind(4, static_cast<int64_t>(m.market));
  }

  bool hasTables() {
    SQLite::Statement q(*db_, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'");
    return q.executeStep() && q.getColumn(0).getInt() > 0;
  }

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
    // The user's own file is written by hand and seldom: each write is on the disk when it returns.
    db_->exec(kind_ == Kind::Portfolios ? "PRAGMA synchronous = FULL" : "PRAGMA synchronous = NORMAL");
    if (db_->execAndGet("PRAGMA quick_check(1)").getString() != "ok") throw Unusable{"it is damaged"};
    if (version != schemaVersion()) {
      // A new file has no version and no table: it only gets its version. Another schema is emptied for the cache and moved aside for
      // the portfolios.
      if (kind_ == Kind::Portfolios && hasTables()) {
        if (version != 1 && version != 2) throw OtherSchema{version};
        if (version == 1) upgradeV1();
        // A file of 2 has what 3 has but the table of the lists, which `createTable` makes.
        createTable();
        db_->exec("PRAGMA user_version = " + std::to_string(schemaVersion()));
      } else {
        startOver(version);
      }
    }
    createTable();
  }

  // The user's own file of schema 1, whose keys of codes (a currency, a market, a dataset) were packed another way: every key that is one of a
  // code is written as schema 2 has it, in the facts (the node, and the value of a link) and in the events (the dataset, and the keys in the
  // body). Nothing else changes. Two keys that were two codes of one case-folded code are one node now: the later fact stands.
  void upgradeV1() {
    SQLite::Transaction transaction(*db_);
    db_->exec("CREATE TEMP TABLE keymap (old INTEGER PRIMARY KEY, new INTEGER NOT NULL)");
    {
      SQLite::Statement find(*db_,
                             "SELECT node_key FROM facts UNION SELECT value FROM facts WHERE tag = 2 AND typeof(value) = 'integer' "
                             "UNION SELECT dataset FROM events");
      SQLite::Statement put(*db_, "INSERT OR IGNORE INTO keymap (old, new) VALUES (?, ?)");
      while (find.executeStep()) {
        const int64_t old = find.getColumn(0).getInt64();
        const int64_t next = static_cast<int64_t>(upgradeLegacyKey(static_cast<uint64_t>(old)));
        if (next == old) continue;
        put.bind(1, old);
        put.bind(2, next);
        put.exec();
        put.reset();
      }
    }
    db_->exec(
        "CREATE TABLE facts_v2 ("
        "node_key INTEGER NOT NULL, predicate TEXT NOT NULL, "
        "value, tag INTEGER NOT NULL DEFAULT 0, updated INTEGER NOT NULL, "
        "PRIMARY KEY (node_key, predicate)) WITHOUT ROWID");
    db_->exec(
        "INSERT OR REPLACE INTO facts_v2 (node_key, predicate, value, tag, updated) "
        "SELECT COALESCE((SELECT new FROM keymap WHERE old = f.node_key), f.node_key), f.predicate, "
        "CASE WHEN f.tag = 2 AND typeof(f.value) = 'integer' THEN COALESCE((SELECT new FROM keymap WHERE old = f.value), f.value) ELSE f.value END, "
        "f.tag, f.updated FROM facts f ORDER BY f.updated");
    db_->exec("DROP TABLE facts");
    db_->exec("ALTER TABLE facts_v2 RENAME TO facts");
    db_->exec(
        "CREATE TABLE events_v2 ("
        "portfolio INTEGER NOT NULL, dataset INTEGER NOT NULL, id INTEGER NOT NULL, time INTEGER NOT NULL, body TEXT NOT NULL, "
        "PRIMARY KEY (portfolio, dataset, id)) WITHOUT ROWID");
    {
      SQLite::Statement read(*db_, "SELECT portfolio, dataset, id, time, body FROM events");
      SQLite::Statement write(*db_, "INSERT OR REPLACE INTO events_v2 (portfolio, dataset, id, time, body) VALUES (?, ?, ?, ?, ?)");
      while (read.executeStep()) {
        std::string body = read.getColumn(4).getString();
        nlohmann::json j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
        if (j.is_object()) {
          for (const char * key : {"instrument", "market", "quote"})
            if (auto it = j.find(key); it != j.end() && it->is_number_unsigned()) *it = upgradeLegacyKey(it->get<uint64_t>());
          body = j.dump();
        }
        write.bind(1, read.getColumn(0).getInt64());
        write.bind(2, static_cast<int64_t>(upgradeLegacyKey(static_cast<uint64_t>(read.getColumn(1).getInt64()))));
        write.bind(3, read.getColumn(2).getInt64());
        write.bind(4, read.getColumn(3).getInt64());
        write.bind(5, body);
        write.exec();
        write.reset();
      }
    }
    db_->exec("DROP TABLE events");
    db_->exec("ALTER TABLE events_v2 RENAME TO events");
    db_->exec("DROP TABLE keymap");
    db_->exec("PRAGMA user_version = " + std::to_string(schemaVersion()));
    transaction.commit();
    say(file_.string() + ": schema 1 upgraded to " + std::to_string(schemaVersion()) + " (the keys of codes are packed another way)");
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
    db_->exec("PRAGMA user_version = " + std::to_string(schemaVersion()));
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
    if (kind_ == Kind::Portfolios) {
      db_->exec(
          "CREATE TABLE IF NOT EXISTS events ("
          "portfolio INTEGER NOT NULL, dataset INTEGER NOT NULL, id INTEGER NOT NULL, time INTEGER NOT NULL, body TEXT NOT NULL, "
          "PRIMARY KEY (portfolio, dataset, id)) WITHOUT ROWID");
      // The edges on the user's lists: a list is its code, an edge its four keys.
      db_->exec(
          "CREATE TABLE IF NOT EXISTS list_members ("
          "list TEXT NOT NULL, base INTEGER NOT NULL, quote INTEGER NOT NULL, market INTEGER NOT NULL, "
          "PRIMARY KEY (list, base, quote, market)) WITHOUT ROWID");
    }
  }

  // Puts the file (and what WAL mode keeps beside it) out of the way, with `prefix` and the time added: `.bad-` for one that is damaged,
  // `.old-` for one of another schema. False where that cannot be done.
  bool moveAside(const std::string & prefix, const std::string & why) {
    db_.reset();
    std::string suffix =
        prefix + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    {
      // One that was moved aside in the same millisecond, or by hand, is never written over: the user's own file may be in it.
      std::error_code ec;
      const std::string base = suffix;
      for (int n = 2; fs::exists(fs::path(file_.string() + suffix), ec); n++) suffix = base + "-" + std::to_string(n);
    }
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
        // The cache is rebuilt, so a copy that cannot be moved is deleted; the user's own file never is.
        if (kind_ == Kind::Portfolios) {
          moved = false;
          continue;
        }
        fs::remove(from, ec);
        if (ec) moved = false;
      }
    }
    say(file_.string() + ": " + why + "; " +
        (moved ? "it was moved aside (" + file_.filename().string() + suffix + ") and a new one is started"
               : std::string("it could not be moved aside, so ") + kept() + " are not kept"));
    return moved;
  }

  Kind kind_;
  fs::path file_;
  FactLog log_;
  std::unique_ptr<SQLite::Database> db_;
  std::mutex mutex_;  // the connection is used by the writer thread and by a restore
};

}  // namespace

std::shared_ptr<FactStore> openSqliteFactStore(const std::filesystem::path & file, FactLog log) {
  return std::make_shared<SqliteFactStore>(file, std::move(log));
}

std::shared_ptr<FactStore> openSqlitePortfolioStore(const std::filesystem::path & file, FactLog log) {
  return std::make_shared<SqliteFactStore>(file, std::move(log), Kind::Portfolios);
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

std::filesystem::path defaultPortfoliosFile() {
  const std::filesystem::path facts = defaultFactsFile();
  return facts.empty() ? facts : facts.parent_path() / "portfolios.sqlite";
}
