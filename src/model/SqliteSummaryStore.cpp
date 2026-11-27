#include "model/SqliteSummaryStore.h"

#include "model/NodeKey.h"
#include "model/SummaryRecords.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <map>
#include <mutex>

namespace {

namespace fs = std::filesystem;

constexpr int kSummariesVersion = 7;  // 7: the order book column is `nordnet_order_book_id`, for it is Nordnet's; 6: a bar with no open says so in its record (SummaryRecords.h); 5: the high and the low over each period of an edge, and queues of the kind their writer says; 4: the change over each period of an edge; 3: the keys of codes are of eleven capitals or digits, base 37; 2: a digest of each queue's last bar (`last_hash`); 1: the first
// SQLite's result codes for a damaged database and for a file that is none (as the facts store has them).
constexpr int kCorrupt = 11, kNotADatabase = 26;

struct Unusable {
  std::string why;
};

int64_t asInt(uint64_t v) { return static_cast<int64_t>(v); }

class SqliteSummaryStore : public SummaryStore {
 public:
  SqliteSummaryStore(fs::path file, DatasetId dataset, FactLog log) : file_(std::move(file)), dataset_(dataset), log_(std::move(log)) {
    std::error_code ec;
    if (file_.has_parent_path()) fs::create_directories(file_.parent_path(), ec);
    for (int attempt = 0; attempt < 2 && !db_; attempt++) {
      try {
        open();
      } catch (const Unusable & u) {
        db_.reset();
        if (!moveAside("it could not be used (" + u.why + ")")) return;
      } catch (const SQLite::Exception & e) {
        db_.reset();
        if (e.getErrorCode() == kCorrupt || e.getErrorCode() == kNotADatabase) {
          if (!moveAside(std::string("it could not be used (") + e.what() + ")")) return;
        } else {
          say(std::string("cannot open ") + file_.string() + ": " + e.what() + "; summaries are not kept");
          return;
        }
      } catch (const std::exception & e) {
        db_.reset();
        say(std::string("cannot open ") + file_.string() + ": " + e.what() + "; summaries are not kept");
        return;
      }
    }
  }

  std::vector<StoredEdge> loadEdges() override {
    std::vector<StoredEdge> out;
    if (!db_) return out;
    std::lock_guard lock(read_mutex_);
    try {
      SQLite::Database & db = reader();
      SQLite::Statement q(db,
                          "SELECT e.id, e.base, e.quote, e.market, e.symbol, e.identifier, e.nordnet_order_book_id, e.type, e.name, "
                          "e.last, e.bid, e.ask, e.quote_time, e.previous_close, e.previous_close_day, "
                          "e.change_day, e.change_week, e.change_month, e.change_quarter, e.change_year, e.change_year3, e.change_year10, "
                          "e.high_day, e.high_week, e.high_month, e.high_quarter, e.high_year, e.high_year3, e.high_year10, e.low_day, e.low_week, e.low_month, e.low_quarter, e.low_year, e.low_year3, e.low_year10, "
                          "q.resolution, q.kind, q.fetched, q.count, q.first_key, q.last_key, q.last_hash "
                          "FROM edges e LEFT JOIN queues q ON q.edge = e.id ORDER BY e.id, q.resolution");
      int64_t current = -1;
      while (q.executeStep()) {
        const int64_t id = q.getColumn(0).getInt64();
        if (id != current) {
          current = id;
          StoredEdge e;
          e.listing.key = EdgeKey{InstrumentId{static_cast<uint64_t>(q.getColumn(1).getInt64())}, InstrumentId{static_cast<uint64_t>(q.getColumn(2).getInt64())},
                                  MarketId{static_cast<uint64_t>(q.getColumn(3).getInt64())}, dataset_};
          e.listing.symbol = q.getColumn(4).getString();
          e.listing.identifier = q.getColumn(5).getString();
          e.listing.nordnetOrderBookId = q.getColumn(6).getString();
          e.listing.type = q.getColumn(7).getString();
          e.listing.name = q.getColumn(8).getString();
          e.quote.last = real(q.getColumn(9));
          e.quote.bid = real(q.getColumn(10));
          e.quote.ask = real(q.getColumn(11));
          e.quote.time = q.getColumn(12).isNull() ? 0 : q.getColumn(12).getInt64();
          e.previous_close = real(q.getColumn(13));
          e.previous_close_day = q.getColumn(14).isNull() ? 0 : q.getColumn(14).getInt64();
          for (size_t i = 0; i < kChangeCount; i++) {
            e.changes.pct[i] = real(q.getColumn(15 + static_cast<int>(i)));
            e.ranges.high[i] = real(q.getColumn(22 + static_cast<int>(i)));
            e.ranges.low[i] = real(q.getColumn(29 + static_cast<int>(i)));
          }
          out.push_back(std::move(e));
        }
        if (!q.getColumn(36).isNull()) {
          const int resolution = q.getColumn(36).getInt();
          if (resolution < 0 || resolution > static_cast<int>(Resolution::Quarter)) continue;
          QueueCoverage c;
          c.kind = q.getColumn(37).getInt() == static_cast<int>(QueueKind::Scalar) ? QueueKind::Scalar : QueueKind::Candle;
          c.fetched = q.getColumn(38).getInt64();
          c.count = static_cast<uint32_t>(q.getColumn(39).getInt64());
          c.first_key = q.getColumn(40).getInt64();
          c.last_key = q.getColumn(41).getInt64();
          c.last_hash = static_cast<uint64_t>(q.getColumn(42).getInt64());
          out.back().queues.emplace_back(static_cast<Resolution>(resolution), c);
        }
      }
    } catch (const std::exception & e) {
      say(std::string("cannot read ") + file_.string() + ": " + e.what());
      out.clear();
    }
    return out;
  }

  std::optional<StoredQueue> readQueue(const EdgeKey & edge, Resolution resolution) override {
    if (!db_) return std::nullopt;
    std::lock_guard lock(read_mutex_);
    try {
      SQLite::Statement q(reader(),
                          "SELECT q.kind, q.fetched, q.data FROM queues q JOIN edges e ON e.id = q.edge "
                          "WHERE e.base = ? AND e.quote = ? AND e.market = ? AND q.resolution = ?");
      q.bind(1, asInt(edge.base.value));
      q.bind(2, asInt(edge.quote.value));
      q.bind(3, asInt(edge.market.value));
      q.bind(4, static_cast<int>(resolution));
      if (!q.executeStep()) return std::nullopt;
      StoredQueue out;
      out.edge = edge;
      out.resolution = resolution;
      out.fetched = q.getColumn(1).getInt64();
      const SQLite::Column data = q.getColumn(2);
      const std::string_view blob(static_cast<const char *>(data.getBlob()), static_cast<size_t>(data.getBytes()));
      if (!summary::unpack(blob, out.kind, out.bars)) {
        say(file_.string() + ": a queue (" + name(resolution) + ") is not one of this program's, so it is left out");
        return std::nullopt;
      }
      return out;
    } catch (const std::exception & e) {
      say(std::string("cannot read ") + file_.string() + ": " + e.what());
      return std::nullopt;
    }
  }

  void apply(const SummaryBatch & batch) override {
    if (!db_ || batch.empty()) return;
    std::lock_guard lock(write_mutex_);
    try {
      SQLite::Transaction transaction(*db_);
      for (const StoredEdge & e : batch.edges) putEdge(e);
      for (const StoredQueue & q : batch.queues) putQueue(q);
      transaction.commit();
    } catch (const std::exception & e) {
      say(std::string("cannot write ") + file_.string() + ": " + e.what());
    }
  }

 private:
  static double real(const SQLite::Column & c) { return c.isNull() ? kUndefined : c.getDouble(); }
  static void bindReal(SQLite::Statement & s, int at, double v) {
    if (isDefined(v)) s.bind(at, v);
    else s.bind(at);
  }

  void say(const std::string & text) const {
    if (log_) log_(text);
    else std::fprintf(stderr, "stocks: %s\n", text.c_str());
  }

  // The second connection, for reads: made when the first is asked of it, which is after the file is.
  SQLite::Database & reader() {
    if (!reader_) {
      reader_ = std::make_unique<SQLite::Database>(file_.string(), SQLite::OPEN_READONLY);
      reader_->setBusyTimeout(2000);
    }
    return *reader_;
  }

  void putEdge(const StoredEdge & e) {
    SQLite::Statement put(*db_,
                          "INSERT INTO edges (base, quote, market, symbol, identifier, nordnet_order_book_id, type, name, last, bid, ask, quote_time, "
                          "previous_close, previous_close_day, change_day, change_week, change_month, change_quarter, change_year, change_year3, change_year10, "
                          "high_day, high_week, high_month, high_quarter, high_year, high_year3, high_year10, low_day, low_week, low_month, low_quarter, low_year, low_year3, low_year10) "
                          "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                          "ON CONFLICT (base, quote, market) DO UPDATE SET symbol = excluded.symbol, identifier = excluded.identifier, "
                          "nordnet_order_book_id = excluded.nordnet_order_book_id, type = excluded.type, name = excluded.name, last = excluded.last, "
                          "bid = excluded.bid, ask = excluded.ask, quote_time = excluded.quote_time, "
                          "previous_close = excluded.previous_close, previous_close_day = excluded.previous_close_day, "
                          "change_day = excluded.change_day, change_week = excluded.change_week, change_month = excluded.change_month, "
                          "change_quarter = excluded.change_quarter, change_year = excluded.change_year, change_year3 = excluded.change_year3, "
                          "change_year10 = excluded.change_year10, high_day = excluded.high_day, high_week = excluded.high_week, high_month = excluded.high_month, high_quarter = excluded.high_quarter, high_year = excluded.high_year, high_year3 = excluded.high_year3, high_year10 = excluded.high_year10, "
                          "low_day = excluded.low_day, low_week = excluded.low_week, low_month = excluded.low_month, low_quarter = excluded.low_quarter, low_year = excluded.low_year, low_year3 = excluded.low_year3, low_year10 = excluded.low_year10");
    const EdgeKey & k = e.listing.key;
    put.bind(1, asInt(k.base.value));
    put.bind(2, asInt(k.quote.value));
    put.bind(3, asInt(k.market.value));
    put.bind(4, e.listing.symbol);
    put.bind(5, e.listing.identifier);
    put.bind(6, e.listing.nordnetOrderBookId);
    put.bind(7, e.listing.type);
    put.bind(8, e.listing.name);
    bindReal(put, 9, e.quote.last);
    bindReal(put, 10, e.quote.bid);
    bindReal(put, 11, e.quote.ask);
    if (e.quote.time != 0) put.bind(12, static_cast<int64_t>(e.quote.time));
    else put.bind(12);
    bindReal(put, 13, e.previous_close);
    if (isDefined(e.previous_close)) put.bind(14, e.previous_close_day);
    else put.bind(14);
    for (size_t i = 0; i < kChangeCount; i++) {
      bindReal(put, 15 + static_cast<int>(i), e.changes.pct[i]);
      bindReal(put, 22 + static_cast<int>(i), e.ranges.high[i]);
      bindReal(put, 29 + static_cast<int>(i), e.ranges.low[i]);
    }
    put.exec();
  }

  // The row of an edge, made bare where it has none.
  int64_t edgeId(const EdgeKey & k) {
    {
      SQLite::Statement q(*db_, "SELECT id FROM edges WHERE base = ? AND quote = ? AND market = ?");
      q.bind(1, asInt(k.base.value));
      q.bind(2, asInt(k.quote.value));
      q.bind(3, asInt(k.market.value));
      if (q.executeStep()) return q.getColumn(0).getInt64();
    }
    SQLite::Statement make(*db_, "INSERT INTO edges (base, quote, market) VALUES (?, ?, ?)");
    make.bind(1, asInt(k.base.value));
    make.bind(2, asInt(k.quote.value));
    make.bind(3, asInt(k.market.value));
    make.exec();
    return db_->getLastInsertRowid();
  }

  void putQueue(const StoredQueue & in) {
    if (in.merge && in.bars.empty()) {
      // Said again and the same: only the time it was fetched moves.
      SQLite::Statement touch(*db_, "UPDATE queues SET fetched = ? WHERE resolution = ? AND edge = (SELECT id FROM edges WHERE base = ? AND quote = ? AND market = ?)");
      touch.bind(1, static_cast<int64_t>(in.fetched));
      touch.bind(2, static_cast<int>(in.resolution));
      touch.bind(3, asInt(in.edge.base.value));
      touch.bind(4, asInt(in.edge.quote.value));
      touch.bind(5, asInt(in.edge.market.value));
      touch.exec();
      return;
    }
    const int64_t edge = edgeId(in.edge);
    StoredQueue q = in;
    if (in.merge) {
      // Into what the file has: read here, on the thread that writes, so that the store never has to read it to say a bar.
      SQLite::Statement old(*db_, "SELECT data FROM queues WHERE edge = ? AND resolution = ?");
      old.bind(1, edge);
      old.bind(2, static_cast<int>(in.resolution));
      StoredQueue base;
      base.edge = in.edge;
      base.resolution = in.resolution;
      base.kind = in.kind;
      if (old.executeStep()) {
        const SQLite::Column data = old.getColumn(0);
        QueueKind kind = in.kind;
        std::vector<Candle> bars;
        if (summary::unpack(std::string_view(static_cast<const char *>(data.getBlob()), static_cast<size_t>(data.getBytes())), kind, bars) && kind == in.kind)
          base.bars = std::move(bars);  // a queue of the other kind is not mixed in
      }
      std::vector<Candle> bars = std::move(base.bars);
      for (const Candle & c : in.bars) summary::put(bars, c);
      summary::trim(bars, in.resolution, in.keep);
      q.bars = std::move(bars);
    }
    std::erase_if(q.bars, [](const Candle & c) { return !summary::fits(c); });
    if (q.bars.empty()) {
      SQLite::Statement gone(*db_, "DELETE FROM queues WHERE edge = ? AND resolution = ?");
      gone.bind(1, edge);
      gone.bind(2, static_cast<int>(in.resolution));
      gone.exec();
      return;
    }
    const std::string blob = summary::pack(q.kind, q.bars);
    SQLite::Statement put(*db_,
                          "INSERT INTO queues (edge, resolution, kind, fetched, count, first_key, last_key, last_hash, data) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
                          "ON CONFLICT (edge, resolution) DO UPDATE SET kind = excluded.kind, fetched = excluded.fetched, count = excluded.count, "
                          "first_key = excluded.first_key, last_key = excluded.last_key, last_hash = excluded.last_hash, data = excluded.data");
    put.bind(1, edge);
    put.bind(2, static_cast<int>(in.resolution));
    put.bind(3, static_cast<int>(q.kind));
    put.bind(4, static_cast<int64_t>(q.fetched));
    put.bind(5, static_cast<int64_t>(q.bars.size()));
    put.bind(6, q.bars.front().time);
    put.bind(7, q.bars.back().time);
    put.bind(8, static_cast<int64_t>(summary::hashOf(q.kind, summary::normalized(q.kind, q.bars.back()))));
    put.bind(9, blob.data(), static_cast<int>(blob.size()));
    put.exec();
  }

  void open() {
    db_ = std::make_unique<SQLite::Database>(file_.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db_->setBusyTimeout(2000);
    // The first thing read from a file that is no database throws.
    const int version = db_->execAndGet("PRAGMA user_version").getInt();
    db_->exec("PRAGMA journal_mode = WAL");
    db_->exec("PRAGMA synchronous = NORMAL");
    if (db_->execAndGet("PRAGMA quick_check(1)").getString() != "ok") throw Unusable{"it is damaged"};
    if (version != kSummariesVersion) startOver(version);
    createTables();
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
    db_->exec("PRAGMA user_version = " + std::to_string(kSummariesVersion));
    transaction.commit();
    if (!tables.empty()) say(file_.string() + ": schema " + std::to_string(version) + ", not this program's; starting over");
  }

  void createTables() {
    db_->exec(
        "CREATE TABLE IF NOT EXISTS edges ("
        "id INTEGER PRIMARY KEY, base INTEGER NOT NULL, quote INTEGER NOT NULL, market INTEGER NOT NULL, "
        "symbol TEXT NOT NULL DEFAULT '', identifier TEXT NOT NULL DEFAULT '', nordnet_order_book_id TEXT NOT NULL DEFAULT '', "
        "type TEXT NOT NULL DEFAULT '', name TEXT NOT NULL DEFAULT '', "
        "last REAL, bid REAL, ask REAL, quote_time INTEGER, previous_close REAL, previous_close_day INTEGER, "
        "change_day REAL, change_week REAL, change_month REAL, change_quarter REAL, change_year REAL, change_year3 REAL, change_year10 REAL, "
        "high_day REAL, high_week REAL, high_month REAL, high_quarter REAL, high_year REAL, high_year3 REAL, high_year10 REAL, "
        "low_day REAL, low_week REAL, low_month REAL, low_quarter REAL, low_year REAL, low_year3 REAL, low_year10 REAL, "
        "UNIQUE (base, quote, market))");
    db_->exec(
        "CREATE TABLE IF NOT EXISTS queues ("
        "edge INTEGER NOT NULL, resolution INTEGER NOT NULL, kind INTEGER NOT NULL, fetched INTEGER NOT NULL, count INTEGER NOT NULL, "
        "first_key INTEGER NOT NULL, last_key INTEGER NOT NULL, last_hash INTEGER NOT NULL, data BLOB NOT NULL, "
        "PRIMARY KEY (edge, resolution)) WITHOUT ROWID");
  }

  // Puts the file (and what WAL mode keeps beside it) out of the way, with `.bad-<time>` added. False where that cannot be done.
  bool moveAside(const std::string & why) {
    reader_.reset();
    db_.reset();
    std::string suffix = ".bad-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    {
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
        // A cache that cannot be moved is deleted: the sources say it all again.
        fs::remove(from, ec);
        if (ec) moved = false;
      }
    }
    say(file_.string() + ": " + why + "; " +
        (moved ? "it was moved aside (" + file_.filename().string() + suffix + ") and a new one is started" : std::string("it could not be moved aside, so summaries are not kept")));
    return moved;
  }

  fs::path file_;
  DatasetId dataset_;
  FactLog log_;
  std::unique_ptr<SQLite::Database> db_;
  std::unique_ptr<SQLite::Database> reader_;
  std::mutex write_mutex_, read_mutex_;
};

class SqliteSummaries : public SummaryStores {
 public:
  SqliteSummaries(fs::path directory, FactLog log) : directory_(std::move(directory)), log_(std::move(log)) {}

  std::vector<std::string> datasets() override {
    std::vector<std::string> out;
    if (directory_.empty()) return out;
    std::error_code ec;
    for (const auto & entry : fs::directory_iterator(directory_, ec))
      if (entry.is_regular_file(ec) && entry.path().extension() == ".sqlite" && datasetKey(entry.path().stem().string())) out.push_back(entry.path().stem().string());
    std::sort(out.begin(), out.end());
    return out;
  }

  std::shared_ptr<SummaryStore> open(const std::string & dataset) override {
    if (directory_.empty()) return nullptr;
    const auto key = datasetKey(dataset);
    if (!key) return nullptr;
    std::lock_guard lock(mutex_);
    auto & store = stores_[dataset];
    if (!store) store = openSqliteSummaryStore(directory_ / (dataset + ".sqlite"), DatasetId{key->value}, log_);
    return store;
  }

 private:
  fs::path directory_;
  FactLog log_;
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<SummaryStore>> stores_;
};

}  // namespace

std::shared_ptr<SummaryStore> openSqliteSummaryStore(const std::filesystem::path & file, DatasetId dataset, FactLog log) {
  return std::make_shared<SqliteSummaryStore>(file, dataset, std::move(log));
}

std::shared_ptr<SummaryStores> openSqliteSummaries(const std::filesystem::path & directory, FactLog log) {
  return std::make_shared<SqliteSummaries>(directory, std::move(log));
}

std::filesystem::path defaultSummariesDirectory() {
  const std::filesystem::path facts = defaultFactsFile();
  return facts.empty() ? facts : facts.parent_path() / "summaries";
}
