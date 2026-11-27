#include "model/SqliteBarStore.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cstdio>
#include <mutex>

namespace {

constexpr int kBarsVersion = 1;

int64_t asInt(uint64_t v) { return static_cast<int64_t>(v); }

class SqliteBarStore : public BarStore {
 public:
  SqliteBarStore(std::filesystem::path file, FactLog log) : file_(std::move(file)), log_(std::move(log)) {
    try {
      std::error_code ec;
      if (file_.has_parent_path()) std::filesystem::create_directories(file_.parent_path(), ec);
      db_ = std::make_unique<SQLite::Database>(file_.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
      db_->setBusyTimeout(5000);
      const int version = db_->execAndGet("PRAGMA user_version").getInt();
      db_->exec("PRAGMA journal_mode = WAL");
      db_->exec("PRAGMA synchronous = NORMAL");
      if (version != kBarsVersion) {
        SQLite::Transaction transaction(*db_);
        db_->exec("DROP TABLE IF EXISTS bars");
        db_->exec("PRAGMA user_version = " + std::to_string(kBarsVersion));
        transaction.commit();
        if (version != 0) say("schema " + std::to_string(version) + ", not this program's; starting over");
      }
      db_->exec(
          "CREATE TABLE IF NOT EXISTS bars ("
          "base INTEGER NOT NULL, quote INTEGER NOT NULL, market INTEGER NOT NULL, dataset INTEGER NOT NULL, resolution INTEGER NOT NULL, "
          "time INTEGER NOT NULL, open REAL NOT NULL, high REAL NOT NULL, low REAL NOT NULL, close REAL NOT NULL, volume REAL, "
          "PRIMARY KEY (base, quote, market, dataset, resolution, time)) WITHOUT ROWID");
    } catch (const std::exception & e) {
      db_.reset();
      say(std::string("cannot open ") + file_.string() + ": " + e.what() + "; bars are not kept");
    }
  }

  void apply(const BarBatch & batch) override {
    if (!db_) return;
    std::lock_guard lock(mutex_);
    try {
      SQLite::Transaction transaction(*db_);
      SQLite::Statement put(*db_,
                            "INSERT INTO bars (base, quote, market, dataset, resolution, time, open, high, low, close, volume) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                            "ON CONFLICT (base, quote, market, dataset, resolution, time) DO UPDATE SET "
                            "open = excluded.open, high = excluded.high, low = excluded.low, close = excluded.close, volume = excluded.volume");
      for (const StoredBars & s : batch.series)
        for (const Candle & c : s.bars) {
          put.bind(1, asInt(s.edge.base.value));
          put.bind(2, asInt(s.edge.quote.value));
          put.bind(3, asInt(s.edge.market.value));
          put.bind(4, asInt(s.edge.dataset.value));
          put.bind(5, static_cast<int>(s.resolution));
          put.bind(6, c.time);
          put.bind(7, c.open);
          put.bind(8, c.high);
          put.bind(9, c.low);
          put.bind(10, c.close);
          if (isDefined(c.volume)) put.bind(11, c.volume);
          else put.bind(11);
          put.exec();
          put.reset();
        }
      transaction.commit();
    } catch (const std::exception & e) {
      say(std::string("cannot write ") + file_.string() + ": " + e.what());
      throw;
    }
  }

  void read(const EdgeKey & edge, Resolution resolution, int64_t first, int64_t last, const std::function<void(const Candle &)> & each) override {
    if (!db_) return;
    std::lock_guard lock(mutex_);
    SQLite::Statement q(*db_,
                        "SELECT time, open, high, low, close, volume FROM bars "
                        "WHERE base = ? AND quote = ? AND market = ? AND dataset = ? AND resolution = ? AND time BETWEEN ? AND ? ORDER BY time");
    q.bind(1, asInt(edge.base.value));
    q.bind(2, asInt(edge.quote.value));
    q.bind(3, asInt(edge.market.value));
    q.bind(4, asInt(edge.dataset.value));
    q.bind(5, static_cast<int>(resolution));
    q.bind(6, first);
    q.bind(7, last);
    while (q.executeStep()) {
      Candle c;
      c.time = q.getColumn(0).getInt64();
      c.open = q.getColumn(1).getDouble();
      c.high = q.getColumn(2).getDouble();
      c.low = q.getColumn(3).getDouble();
      c.close = q.getColumn(4).getDouble();
      c.volume = q.getColumn(5).isNull() ? kUndefined : q.getColumn(5).getDouble();
      each(c);
    }
  }

 private:
  void say(const std::string & text) const {
    if (log_) log_(text);
    else std::fprintf(stderr, "stocks: %s: %s\n", file_.string().c_str(), text.c_str());
  }

  std::filesystem::path file_;
  FactLog log_;
  std::unique_ptr<SQLite::Database> db_;
  std::mutex mutex_;
};

}  // namespace

std::shared_ptr<BarStore> openSqliteBarStore(const std::filesystem::path & file, FactLog log) {
  return std::make_shared<SqliteBarStore>(file, std::move(log));
}
