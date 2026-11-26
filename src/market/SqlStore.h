#pragma once

#include "market/Config.h"
#include "market/Dataset.h"
#include "market/SqlStores.h"
#include "market/Store.h"
#include "market/Tables.h"
#include "model/NodeKey.h"

#include <sqlpp11/sqlpp11.h>

#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// The market data in SQL, once, for SQLite and for MariaDB: sqlpp11 gives both the same typed interface, and every statement here is
// a prepared one with its values bound, so nothing is formatted into SQL text (no escaping, no locale in a number) and a result is read a
// row at a time. A backend is a class that says what differs (see SqliteStore.cpp and MariaDbStore.cpp): the connection, the end of
// a CREATE TABLE and how a schema is made and dropped.
//
// Each backend's translation unit instantiates `SqlStore<Backend>`; nothing else includes this file.
namespace market {


template <class Backend>
class SqlStore : public MarketStore {
  using Db = typename Backend::Connection;

 public:
  SqlStore(const DbConfig & config, OpenMode mode, const Log & log) : db_(Backend::connect(config, mode == OpenMode::ReadWrite)) {
    prepareSchema(config, mode, log);
  }

  uint64_t putDataset(const std::string & code, const std::string & name) override {
    const auto key = datasetKey(code);
    if (!key) throw std::runtime_error("a dataset code is one to ten letters or digits: " + code);
    std::lock_guard lock(mutex_);
    tables::Datasets d;
    auto find = db_->prepare(sqlpp::select(d.dataset).from(d).where(d.dataset == sqlpp::parameter(d.dataset)));
    find.params.dataset = static_cast<int64_t>(key->value);
    bool known = false;
    for (const auto & row : (*db_)(find)) known = (static_cast<void>(row), true);
    if (known) {
        auto update = db_->prepare(sqlpp::update(d).set(d.code = sqlpp::parameter(d.code), d.name = sqlpp::parameter(d.name))
                                     .where(d.dataset == sqlpp::parameter(d.dataset)));
      update.params.dataset = static_cast<int64_t>(key->value);
      update.params.code = code;
      update.params.name = name;
      (*db_)(update);
    } else {
      auto insert = db_->prepare(sqlpp::insert_into(d).set(d.dataset = sqlpp::parameter(d.dataset), d.code = sqlpp::parameter(d.code),
                                                           d.name = sqlpp::parameter(d.name)));
      insert.params.dataset = static_cast<int64_t>(key->value);
      insert.params.code = code;
      insert.params.name = name;
      (*db_)(insert);
    }
    return key->value;
  }

  uint64_t beginBatch(uint64_t dataset, const std::string & note) override {
    std::lock_guard lock(mutex_);
    Transaction transaction(*db_);
    const int64_t version = std::stoll(getMeta("batch").value_or("0")) + 1;
    putMeta("batch", std::to_string(version));
    tables::Batches b;
    auto insert = db_->prepare(sqlpp::insert_into(b).set(b.version = sqlpp::parameter(b.version), b.dataset = sqlpp::parameter(b.dataset),
                                                         b.startedAt = sqlpp::parameter(b.startedAt), b.note = sqlpp::parameter(b.note)));
    insert.params.version = version;
    insert.params.dataset = static_cast<int64_t>(dataset);
    insert.params.startedAt = static_cast<int64_t>(std::time(nullptr));
    insert.params.note = note.substr(0, 255);
    (*db_)(insert);
    transaction.commit();
    return static_cast<uint64_t>(version);
  }

  void putInstrument(const InstrumentRecord & r) override {
    std::lock_guard lock(mutex_);
    tables::Instruments i;
    // (The number of rows an UPDATE changed is not trusted: SQLite's is that of the last statement that changed any.)
    auto find = db_->prepare(sqlpp::select(i.code).from(i).where(i.base == sqlpp::parameter(i.base) and i.quote == sqlpp::parameter(i.quote) and
                                                                 i.market == sqlpp::parameter(i.market) and i.dataset == sqlpp::parameter(i.dataset)));
    find.params.base = static_cast<int64_t>(r.edge.base);
    find.params.quote = static_cast<int64_t>(r.edge.quote);
    find.params.market = static_cast<int64_t>(r.edge.market);
    find.params.dataset = static_cast<int64_t>(r.edge.dataset);
    bool known = false;
    for (const auto & row : (*db_)(find)) known = (static_cast<void>(row), true);
    if (known) {
      auto update = db_->prepare(sqlpp::update(i)
                                     .set(i.code = sqlpp::parameter(i.code), i.name = sqlpp::parameter(i.name),
                                          i.assetClass = sqlpp::parameter(i.assetClass), i.firstPeriod = sqlpp::parameter(i.firstPeriod))
                                     .where(i.base == sqlpp::parameter(i.base) and i.quote == sqlpp::parameter(i.quote) and
                                            i.market == sqlpp::parameter(i.market) and i.dataset == sqlpp::parameter(i.dataset)));
      update.params.code = r.code;
      update.params.name = r.name;
      update.params.assetClass = r.assetClass;
      update.params.firstPeriod = r.firstPeriod;
      update.params.base = static_cast<int64_t>(r.edge.base);
      update.params.quote = static_cast<int64_t>(r.edge.quote);
      update.params.market = static_cast<int64_t>(r.edge.market);
      update.params.dataset = static_cast<int64_t>(r.edge.dataset);
      (*db_)(update);
      return;
    }
    auto insert = db_->prepare(sqlpp::insert_into(i).set(
        i.base = sqlpp::parameter(i.base), i.quote = sqlpp::parameter(i.quote), i.market = sqlpp::parameter(i.market),
        i.dataset = sqlpp::parameter(i.dataset), i.code = sqlpp::parameter(i.code), i.name = sqlpp::parameter(i.name),
        i.assetClass = sqlpp::parameter(i.assetClass), i.firstPeriod = sqlpp::parameter(i.firstPeriod)));
    insert.params.base = static_cast<int64_t>(r.edge.base);
    insert.params.quote = static_cast<int64_t>(r.edge.quote);
    insert.params.market = static_cast<int64_t>(r.edge.market);
    insert.params.dataset = static_cast<int64_t>(r.edge.dataset);
    insert.params.code = r.code;
    insert.params.name = r.name;
    insert.params.assetClass = r.assetClass;
    insert.params.firstPeriod = r.firstPeriod;
    (*db_)(insert);
  }

  void removeInstrument(const Edge & e) override {
    std::lock_guard lock(mutex_);
    Transaction transaction(*db_);
    deleteBars(e, std::nullopt);
    tables::Loads l;
    auto deleteLoads = db_->prepare(sqlpp::remove_from(l).where(l.base == sqlpp::parameter(l.base) and l.quote == sqlpp::parameter(l.quote) and
                                                                l.market == sqlpp::parameter(l.market) and l.dataset == sqlpp::parameter(l.dataset)));
    deleteLoads.params.base = static_cast<int64_t>(e.base);
    deleteLoads.params.quote = static_cast<int64_t>(e.quote);
    deleteLoads.params.market = static_cast<int64_t>(e.market);
    deleteLoads.params.dataset = static_cast<int64_t>(e.dataset);
    (*db_)(deleteLoads);
    tables::Instruments i;
    auto deleteInstrument = db_->prepare(sqlpp::remove_from(i).where(i.base == sqlpp::parameter(i.base) and i.quote == sqlpp::parameter(i.quote) and
                                                                      i.market == sqlpp::parameter(i.market) and i.dataset == sqlpp::parameter(i.dataset)));
    deleteInstrument.params.base = static_cast<int64_t>(e.base);
    deleteInstrument.params.quote = static_cast<int64_t>(e.quote);
    deleteInstrument.params.market = static_cast<int64_t>(e.market);
    deleteInstrument.params.dataset = static_cast<int64_t>(e.dataset);
    (*db_)(deleteInstrument);
    transaction.commit();
  }

  void replaceBars(const Edge & e, Level level, const std::vector<Bar> & bars, Timestamp lastBarTime, uint64_t version) override {
    std::lock_guard lock(mutex_);
    Transaction transaction(*db_);
    deleteBars(e, level);
    tables::Bars b;
    auto insert = db_->prepare(sqlpp::insert_into(b).set(
        b.base = sqlpp::parameter(b.base), b.quote = sqlpp::parameter(b.quote), b.market = sqlpp::parameter(b.market),
        b.dataset = sqlpp::parameter(b.dataset), b.level = sqlpp::parameter(b.level), b.barKey = sqlpp::parameter(b.barKey),
        b.open = sqlpp::parameter(b.open), b.high = sqlpp::parameter(b.high), b.low = sqlpp::parameter(b.low),
        b.close = sqlpp::parameter(b.close), b.version = sqlpp::parameter(b.version)));
    insert.params.base = static_cast<int64_t>(e.base);
    insert.params.quote = static_cast<int64_t>(e.quote);
    insert.params.market = static_cast<int64_t>(e.market);
    insert.params.dataset = static_cast<int64_t>(e.dataset);
    insert.params.level = static_cast<int>(level);
    insert.params.version = static_cast<int64_t>(version);
    for (const Bar & bar : bars) {
      insert.params.barKey = bar.key;
      insert.params.open = bar.open;
      insert.params.high = bar.high;
      insert.params.low = bar.low;
      insert.params.close = bar.close;
      (*db_)(insert);
    }
    tables::Loads l;
    auto deleteLoad = db_->prepare(sqlpp::remove_from(l).where(l.base == sqlpp::parameter(l.base) and l.quote == sqlpp::parameter(l.quote) and
                                                               l.market == sqlpp::parameter(l.market) and l.dataset == sqlpp::parameter(l.dataset) and
                                                               l.level == sqlpp::parameter(l.level)));
    deleteLoad.params.base = static_cast<int64_t>(e.base);
    deleteLoad.params.quote = static_cast<int64_t>(e.quote);
    deleteLoad.params.market = static_cast<int64_t>(e.market);
    deleteLoad.params.dataset = static_cast<int64_t>(e.dataset);
    deleteLoad.params.level = static_cast<int>(level);
    (*db_)(deleteLoad);
    auto insertLoad = db_->prepare(sqlpp::insert_into(l).set(
        l.base = sqlpp::parameter(l.base), l.quote = sqlpp::parameter(l.quote), l.market = sqlpp::parameter(l.market),
        l.dataset = sqlpp::parameter(l.dataset), l.level = sqlpp::parameter(l.level), l.version = sqlpp::parameter(l.version),
        l.loadedAt = sqlpp::parameter(l.loadedAt), l.barCount = sqlpp::parameter(l.barCount), l.firstKey = sqlpp::parameter(l.firstKey),
        l.lastKey = sqlpp::parameter(l.lastKey), l.lastBarTime = sqlpp::parameter(l.lastBarTime)));
    insertLoad.params.base = static_cast<int64_t>(e.base);
    insertLoad.params.quote = static_cast<int64_t>(e.quote);
    insertLoad.params.market = static_cast<int64_t>(e.market);
    insertLoad.params.dataset = static_cast<int64_t>(e.dataset);
    insertLoad.params.level = static_cast<int>(level);
    insertLoad.params.version = static_cast<int64_t>(version);
    insertLoad.params.loadedAt = static_cast<int64_t>(std::time(nullptr));
    insertLoad.params.barCount = static_cast<int64_t>(bars.size());
    insertLoad.params.firstKey = bars.empty() ? 0 : bars.front().key;
    insertLoad.params.lastKey = bars.empty() ? 0 : bars.back().key;
    insertLoad.params.lastBarTime = lastBarTime;
    (*db_)(insertLoad);
    transaction.commit();
  }

  std::vector<InstrumentInfo> instruments() override {
    std::lock_guard lock(mutex_);
    std::vector<InstrumentInfo> out;
    tables::Instruments i;
    auto selectInstruments = db_->prepare(sqlpp::select(i.base, i.quote, i.market, i.dataset, i.code, i.name, i.assetClass, i.firstPeriod)
                                              .from(i)
                                              .unconditionally()
                                              .order_by(i.code.asc()));
    for (const auto & row : (*db_)(selectInstruments)) {
      InstrumentInfo info;
      info.instrument.edge = {static_cast<uint64_t>(static_cast<int64_t>(row.base)), static_cast<uint64_t>(static_cast<int64_t>(row.quote)),
                              static_cast<uint64_t>(static_cast<int64_t>(row.market)), static_cast<uint64_t>(static_cast<int64_t>(row.dataset))};
      info.instrument.code = std::string(row.code);
      info.instrument.name = std::string(row.name);
      info.instrument.assetClass = std::string(row.assetClass);
      info.instrument.firstPeriod = static_cast<int>(row.firstPeriod);
      out.push_back(std::move(info));
    }
    tables::Loads l;
    auto selectLoads = db_->prepare(sqlpp::select(l.base, l.quote, l.market, l.dataset, l.level, l.version, l.loadedAt, l.barCount, l.firstKey,
                                                  l.lastKey, l.lastBarTime)
                                        .from(l)
                                        .unconditionally()
                                        .order_by(l.level.asc()));
    for (const auto & row : (*db_)(selectLoads)) {
      const Edge edge{static_cast<uint64_t>(static_cast<int64_t>(row.base)), static_cast<uint64_t>(static_cast<int64_t>(row.quote)),
                      static_cast<uint64_t>(static_cast<int64_t>(row.market)), static_cast<uint64_t>(static_cast<int64_t>(row.dataset))};
      LevelState state;
      state.level = static_cast<Level>(static_cast<int>(row.level));
      state.version = static_cast<uint64_t>(static_cast<int64_t>(row.version));
      state.loadedAt = row.loadedAt;
      state.barCount = row.barCount;
      state.firstKey = row.firstKey;
      state.lastKey = row.lastKey;
      state.lastBarTime = row.lastBarTime;
      for (InstrumentInfo & info : out)
        if (info.instrument.edge == edge) info.levels.push_back(state);
    }
    return out;
  }

  void forEachBar(const Edge & e, Level level, int64_t firstKey, int64_t lastKey, const std::function<void(const Bar &)> & each) override {
    std::lock_guard lock(mutex_);
    tables::Bars b;
    // The two ends of the range are parameters of one type; sqlpp11 names a parameter by a column, so the unused columns `version` and
    // `open` name them.
    auto select = db_->prepare(
        sqlpp::select(b.barKey, b.open, b.high, b.low, b.close)
            .from(b)
            .where(b.base == sqlpp::parameter(b.base) and b.quote == sqlpp::parameter(b.quote) and b.market == sqlpp::parameter(b.market) and
                   b.dataset == sqlpp::parameter(b.dataset) and b.level == sqlpp::parameter(b.level) and
                   b.barKey >= sqlpp::parameter(sqlpp::bigint(), b.version) and b.barKey <= sqlpp::parameter(sqlpp::bigint(), b.open))
            .order_by(b.barKey.asc()));
    select.params.base = static_cast<int64_t>(e.base);
    select.params.quote = static_cast<int64_t>(e.quote);
    select.params.market = static_cast<int64_t>(e.market);
    select.params.dataset = static_cast<int64_t>(e.dataset);
    select.params.level = static_cast<int>(level);
    select.params.version = firstKey;
    select.params.open = lastKey;
    for (const auto & row : (*db_)(select)) {
      const Bar bar{row.barKey, row.open, row.high, row.low, row.close};
      each(bar);
    }
  }

  bool healthy() override {
    try {
      std::lock_guard lock(mutex_);
      return getMeta("schema_version").has_value();
    } catch (const std::exception &) {
      return false;
    }
  }

 private:
  // A transaction that rolls back unless it is committed.
  struct Transaction {
    explicit Transaction(Db & db) : db_(db) { db_.start_transaction(); }
    ~Transaction() {
      if (open_) {
        try {
          db_.rollback_transaction(false);
        } catch (...) {
        }
      }
    }
    void commit() {
      db_.commit_transaction();
      open_ = false;
    }
    Db & db_;
    bool open_ = true;
  };

  std::optional<std::string> getMeta(const std::string & name) {
    tables::Meta m;
    auto select = db_->prepare(sqlpp::select(m.value).from(m).where(m.name == sqlpp::parameter(m.name)));
    select.params.name = name;
    std::optional<std::string> found;
    for (const auto & row : (*db_)(select)) found = std::string(row.value);  // read to the end: a result left open holds its statement
    return found;
  }

  void putMeta(const std::string & name, const std::string & value) {
    tables::Meta m;
    if (getMeta(name)) {
      auto update = db_->prepare(sqlpp::update(m).set(m.value = sqlpp::parameter(m.value)).where(m.name == sqlpp::parameter(m.name)));
      update.params.value = value;
      update.params.name = name;
      (*db_)(update);
      return;
    }
    auto insert = db_->prepare(sqlpp::insert_into(m).set(m.name = sqlpp::parameter(m.name), m.value = sqlpp::parameter(m.value)));
    insert.params.name = name;
    insert.params.value = value;
    (*db_)(insert);
  }

  // The bars of the edge: of one level, or of all.
  void deleteBars(const Edge & e, std::optional<Level> level) {
    tables::Bars b;
    if (level) {
      auto del = db_->prepare(sqlpp::remove_from(b).where(b.base == sqlpp::parameter(b.base) and b.quote == sqlpp::parameter(b.quote) and
                                                          b.market == sqlpp::parameter(b.market) and b.dataset == sqlpp::parameter(b.dataset) and
                                                          b.level == sqlpp::parameter(b.level)));
      del.params.base = static_cast<int64_t>(e.base);
      del.params.quote = static_cast<int64_t>(e.quote);
      del.params.market = static_cast<int64_t>(e.market);
      del.params.dataset = static_cast<int64_t>(e.dataset);
      del.params.level = static_cast<int>(*level);
      (*db_)(del);
    } else {
      auto del = db_->prepare(sqlpp::remove_from(b).where(b.base == sqlpp::parameter(b.base) and b.quote == sqlpp::parameter(b.quote) and
                                                          b.market == sqlpp::parameter(b.market) and b.dataset == sqlpp::parameter(b.dataset)));
      del.params.base = static_cast<int64_t>(e.base);
      del.params.quote = static_cast<int64_t>(e.quote);
      del.params.market = static_cast<int64_t>(e.market);
      del.params.dataset = static_cast<int64_t>(e.dataset);
      (*db_)(del);
    }
  }

  void prepareSchema(const DbConfig & config, OpenMode mode, const Log & log) {
    int found = 0;
    bool anything = false;
    try {
      if (auto version = getMeta("schema_version")) found = std::stoi(*version);
      anything = true;
    } catch (const std::exception &) {
      // no meta table: nothing of ours is there
    }
    if (found == kSchemaVersion) return;
    const std::string where = Backend::describe(config);
    if (mode == OpenMode::ReadOnly)
      throw std::runtime_error(where + " is " + (found ? "schema version " + std::to_string(found) : std::string("empty")) +
                               ", this program reads version " + std::to_string(kSchemaVersion) + ": run market-ingest to make it");
    // Nothing is kept across a change of schema: the tables are made again, and what was in them is loaded again from the raw files.
    if (anything && log) log(where + " is schema version " + std::to_string(found) + ", starting over at version " + std::to_string(kSchemaVersion));
    for (const char * table : {"bars", "loads", "instruments", "batches", "datasets", "meta"}) db_->execute(std::string("DROP TABLE IF EXISTS ") + table);
    for (const std::string & sql : createStatements()) db_->execute(sql);
    putMeta("schema_version", std::to_string(kSchemaVersion));
    putMeta("batch", "0");
  }

  static std::vector<std::string> createStatements() {
    const std::string table = Backend::kTableTail, clustered = Backend::kClusteredTableTail;
    return {
        "CREATE TABLE meta (name VARCHAR(32) NOT NULL PRIMARY KEY, value VARCHAR(255) NOT NULL)" + table,
        "CREATE TABLE datasets (dataset BIGINT NOT NULL PRIMARY KEY, code VARCHAR(32) NOT NULL UNIQUE, name VARCHAR(100) NOT NULL)" + table,
        "CREATE TABLE batches (version BIGINT NOT NULL PRIMARY KEY, dataset BIGINT NOT NULL, started_at BIGINT NOT NULL, "
        "note VARCHAR(255) NOT NULL)" + table,
        "CREATE TABLE instruments (base BIGINT NOT NULL, quote BIGINT NOT NULL, market BIGINT NOT NULL, dataset BIGINT NOT NULL, "
        "code VARCHAR(32) NOT NULL, name VARCHAR(100) NOT NULL, asset_class VARCHAR(16) NOT NULL, first_period INT NOT NULL, "
        "PRIMARY KEY (base, quote, market, dataset), UNIQUE (dataset, code))" + table,
        "CREATE TABLE loads (base BIGINT NOT NULL, quote BIGINT NOT NULL, market BIGINT NOT NULL, dataset BIGINT NOT NULL, "
        "level INT NOT NULL, version BIGINT NOT NULL, loaded_at BIGINT NOT NULL, bar_count BIGINT NOT NULL, first_key BIGINT NOT NULL, "
        "last_key BIGINT NOT NULL, last_bar_time BIGINT NOT NULL, PRIMARY KEY (base, quote, market, dataset, level))" + clustered,
        "CREATE TABLE bars (base BIGINT NOT NULL, quote BIGINT NOT NULL, market BIGINT NOT NULL, dataset BIGINT NOT NULL, "
        "level INT NOT NULL, bar_key BIGINT NOT NULL, open DOUBLE NOT NULL, high DOUBLE NOT NULL, low DOUBLE NOT NULL, "
        "close DOUBLE NOT NULL, version BIGINT NOT NULL, PRIMARY KEY (base, quote, market, dataset, level, bar_key))" + clustered,
    };
  }

  std::mutex mutex_;
  std::unique_ptr<Db> db_;
};

}  // namespace market
