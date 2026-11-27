#include "model/MariaDbStores.h"

#include "model/NodeKey.h"
#include "model/SummaryRecords.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>

namespace maria {

void createDatabase(const Config & config) {
  Config bare = config;
  bare.database.clear();
  Connection db(bare);
  std::string name;
  for (char c : config.database) name += c == '`' ? std::string("``") : std::string(1, c);
  db.exec("CREATE DATABASE IF NOT EXISTS `" + name + "` CHARACTER SET utf8mb4");
}

void dropDatabase(const Config & config) {
  Config bare = config;
  bare.database.clear();
  Connection db(bare);
  std::string name;
  for (char c : config.database) name += c == '`' ? std::string("``") : std::string(1, c);
  db.exec("DROP DATABASE IF EXISTS `" + name + "`");
}

}  // namespace maria

namespace {

int64_t asInt(uint64_t v) { return static_cast<int64_t>(v); }

// The schema's version of each store, kept in one small table: a store of another version drops its tables and starts over.
void ensureVersion(maria::Connection & db, const std::string & store, int version, const std::vector<std::string> & tables,
                   const std::function<void(const std::string &)> & say) {
  db.exec("CREATE TABLE IF NOT EXISTS store_versions (store VARCHAR(32) NOT NULL PRIMARY KEY, version INT NOT NULL) ENGINE=InnoDB");
  int found = 0;
  {
    maria::Statement q(db, "SELECT version FROM store_versions WHERE store = ?");
    q.bind(1, store);
    if (q.executeStep()) found = q.getColumn(0).getInt();
  }
  if (found == version) return;
  for (const std::string & table : tables) db.exec("DROP TABLE IF EXISTS " + table);
  maria::Statement put(db, "INSERT INTO store_versions (store, version) VALUES (?, ?) ON DUPLICATE KEY UPDATE version = VALUES(version)");
  put.bind(1, store);
  put.bind(2, version);
  put.exec();
  if (found != 0) say(store + ": schema " + std::to_string(found) + ", not this program's; starting over");
}

// What the three stores have in common: a connection for writes, and one for reads made when it is first asked for, a log, and the rule that
// nothing throws.
class Base {
 public:
  Base(maria::Config config, FactLog log, std::string name) : config_(std::move(config)), log_(std::move(log)), name_(std::move(name)) {}

 protected:
  void say(const std::string & text) const {
    if (log_) log_(name_ + ": " + text);
    else std::fprintf(stderr, "stocks: %s: %s\n", name_.c_str(), text.c_str());
  }
  // The connections are made when they are first wanted, and again after one that failed: a connection that has an error is dropped (what it had
  // open in a transaction is rolled back by the server) and the next call makes a new one, which is the only reconnect there is.
  maria::Connection & writer() {
    if (!db_) db_ = std::make_unique<maria::Connection>(config_);
    return *db_;
  }
  maria::Connection & reader() {
    if (!reader_) reader_ = std::make_unique<maria::Connection>(config_);
    return *reader_;
  }
  void dropWriter() { db_.reset(); }
  void dropReader() { reader_.reset(); }

  maria::Config config_;
  FactLog log_;
  std::string name_;
  std::unique_ptr<maria::Connection> db_, reader_;
  bool usable_ = false;  // the tables are there: what cannot be made stays out of use, with a line in the log
  std::mutex write_mutex_, read_mutex_;
};

// ---- the facts ----

constexpr int kFactsVersion = 1;
enum Kind : int { kNull = 0, kText = 1, kInteger = 2, kReal = 3, kBoolean = 4, kNode = 5 };

class MariaDbFactStore : public FactStore, private Base {
 public:
  MariaDbFactStore(const maria::Config & config, FactLog log) : Base(config, std::move(log), "facts") {
    try {
      ensureVersion(writer(), "facts", kFactsVersion, {"facts"}, [this](const std::string & t) { say(t); });
      writer().exec(
          "CREATE TABLE IF NOT EXISTS facts ("
          "node_key BIGINT NOT NULL, predicate VARCHAR(190) NOT NULL, kind TINYINT NOT NULL, text_value LONGTEXT NULL, "
          "int_value BIGINT NULL, real_value DOUBLE NULL, updated BIGINT NOT NULL, PRIMARY KEY (node_key, predicate)) ENGINE=InnoDB");
      usable_ = true;
    } catch (const std::exception & e) {
      db_.reset();
      usable_ = false;
      say(std::string("cannot open the database: ") + e.what() + "; facts are not kept");
    }
  }

  std::vector<StoredFact> loadAll() override { return load(std::nullopt); }

  std::vector<StoredFact> loadNodes(const std::vector<uint64_t> & nodes) override {
    std::vector<StoredFact> out;
    for (uint64_t node : nodes)
      for (StoredFact & f : load(node)) out.push_back(std::move(f));
    return out;
  }

  void apply(const FactBatch & batch) override {
    if (!usable_ || batch.empty()) return;
    std::lock_guard lock(write_mutex_);
    try {
      maria::Transaction transaction(writer());
      maria::Statement put(writer(),
                           "INSERT INTO facts (node_key, predicate, kind, text_value, int_value, real_value, updated) VALUES (?, ?, ?, ?, ?, ?, ?) "
                           "ON DUPLICATE KEY UPDATE kind = VALUES(kind), text_value = VALUES(text_value), int_value = VALUES(int_value), "
                           "real_value = VALUES(real_value), updated = VALUES(updated)");
      for (const auto * list : {&batch.changed, &batch.rechecked})
        for (const StoredFact & f : *list) {
          put.bind(1, asInt(f.node));
          put.bind(2, f.predicate);
          int kind = kNull;
          put.bind(4);
          put.bind(5);
          put.bind(6);
          if (const auto * t = std::get_if<std::string>(&f.value)) {
            kind = kText;
            put.bind(4, *t);
          } else if (const auto * i = std::get_if<int64_t>(&f.value)) {
            kind = kInteger;
            put.bind(5, *i);
          } else if (const auto * d = std::get_if<double>(&f.value)) {
            kind = kReal;
            put.bind(6, *d);
          } else if (const auto * b = std::get_if<bool>(&f.value)) {
            kind = kBoolean;
            put.bind(5, static_cast<int64_t>(*b ? 1 : 0));
          } else if (const auto * k = std::get_if<NodeKey>(&f.value)) {
            kind = kNode;
            put.bind(5, asInt(k->value));
          }
          put.bind(3, kind);
          put.bind(7, static_cast<int64_t>(f.updated));
          put.exec();
        }
      for (uint64_t node : batch.removed_nodes) {
        maria::Statement gone(writer(), "DELETE FROM facts WHERE node_key = ?");
        gone.bind(1, asInt(node));
        gone.exec();
      }
      for (const auto & [node, predicate] : batch.removed) {
        maria::Statement gone(writer(), "DELETE FROM facts WHERE node_key = ? AND predicate = ?");
        gone.bind(1, asInt(node));
        gone.bind(2, predicate);
        gone.exec();
      }
      transaction.commit();
    } catch (const std::exception & e) {
      dropWriter();  // the batch is not written, whole: the server rolls back what the connection had open when it ends
      say(std::string("cannot write: ") + e.what());
      throw;  // the writer that called counts it (Store::failedWrites), and the ingest says so
    }
  }

 private:
  std::vector<StoredFact> load(std::optional<uint64_t> node) {
    std::vector<StoredFact> out;
    if (!usable_) return out;
    std::lock_guard lock(read_mutex_);
    try {
      maria::Statement q(reader(), node ? "SELECT node_key, predicate, kind, text_value, int_value, real_value, updated FROM facts WHERE node_key = ?"
                                        : "SELECT node_key, predicate, kind, text_value, int_value, real_value, updated FROM facts");
      if (node) q.bind(1, asInt(*node));
      while (q.executeStep()) {
        StoredFact f;
        f.node = static_cast<uint64_t>(q.getColumn(0).getInt64());
        if (f.node == 0) continue;
        f.predicate = q.getColumn(1).getString();
        switch (q.getColumn(2).getInt()) {
          case kText: f.value = q.getColumn(3).getString(); break;
          case kInteger: f.value = q.getColumn(4).getInt64(); break;
          case kReal: f.value = q.getColumn(5).getDouble(); break;
          case kBoolean: f.value = q.getColumn(4).getInt64() != 0; break;
          case kNode:
            if (q.getColumn(4).getInt64() <= 0) continue;
            f.value = NodeKey{static_cast<uint64_t>(q.getColumn(4).getInt64())};
            break;
          default: f.value = std::monostate{}; break;
        }
        f.updated = q.getColumn(6).getInt64();
        out.push_back(std::move(f));
      }
    } catch (const std::exception & e) {
      dropReader();
      say(std::string("cannot read: ") + e.what());
      out.clear();
    }
    return out;
  }
};

// ---- the summaries ----

constexpr int kSummariesVersion = 3;  // 3: a bar with no open says so in its record (SummaryRecords.h); 2: the high and the low over each period of an edge, and queues of the kind their writer says

class MariaDbSummaryStore : public SummaryStore, private Base {
 public:
  MariaDbSummaryStore(const maria::Config & config, DatasetId dataset, FactLog log, std::shared_ptr<std::mutex> setup)
      : Base(config, std::move(log), "summaries"), dataset_(dataset) {
    try {
      std::lock_guard lock(*setup);  // the stores of the datasets make the same tables
      ensureVersion(writer(), "summaries", kSummariesVersion, {"queues", "summary_edges"}, [this](const std::string & t) { say(t); });
      writer().exec(
          "CREATE TABLE IF NOT EXISTS summary_edges ("
          "id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY, dataset BIGINT NOT NULL, base BIGINT NOT NULL, quote BIGINT NOT NULL, market BIGINT NOT NULL, "
          "symbol VARCHAR(190) NOT NULL DEFAULT '', identifier VARCHAR(190) NOT NULL DEFAULT '', order_book VARCHAR(190) NOT NULL DEFAULT '', "
          "type VARCHAR(190) NOT NULL DEFAULT '', name VARCHAR(500) NOT NULL DEFAULT '', "
          "last_price DOUBLE NULL, bid DOUBLE NULL, ask DOUBLE NULL, quote_time BIGINT NULL, previous_close DOUBLE NULL, previous_close_day BIGINT NULL, "
          "change_day DOUBLE NULL, change_week DOUBLE NULL, change_month DOUBLE NULL, change_quarter DOUBLE NULL, change_year DOUBLE NULL, "
          "change_year3 DOUBLE NULL, change_year10 DOUBLE NULL, "
          "high_day DOUBLE NULL, high_week DOUBLE NULL, high_month DOUBLE NULL, high_quarter DOUBLE NULL, high_year DOUBLE NULL, high_year3 DOUBLE NULL, high_year10 DOUBLE NULL, "
          "low_day DOUBLE NULL, low_week DOUBLE NULL, low_month DOUBLE NULL, low_quarter DOUBLE NULL, low_year DOUBLE NULL, low_year3 DOUBLE NULL, low_year10 DOUBLE NULL, UNIQUE KEY by_edge (dataset, base, quote, market)) ENGINE=InnoDB");
      writer().exec(
          "CREATE TABLE IF NOT EXISTS queues ("
          "edge BIGINT NOT NULL, resolution INT NOT NULL, kind INT NOT NULL, fetched BIGINT NOT NULL, count BIGINT NOT NULL, "
          "first_key BIGINT NOT NULL, last_key BIGINT NOT NULL, last_hash BIGINT NOT NULL, data LONGBLOB NOT NULL, "
          "PRIMARY KEY (edge, resolution)) ENGINE=InnoDB");
      usable_ = true;
    } catch (const std::exception & e) {
      db_.reset();
      usable_ = false;
      say(std::string("cannot open the database: ") + e.what() + "; summaries are not kept");
    }
  }

  std::vector<StoredEdge> loadEdges() override {
    std::vector<StoredEdge> out;
    if (!usable_) return out;
    std::lock_guard lock(read_mutex_);
    try {
      maria::Statement q(reader(),
                         "SELECT e.id, e.base, e.quote, e.market, e.symbol, e.identifier, e.order_book, e.type, e.name, "
                         "e.last_price, e.bid, e.ask, e.quote_time, e.previous_close, e.previous_close_day, "
                         "e.change_day, e.change_week, e.change_month, e.change_quarter, e.change_year, e.change_year3, e.change_year10, "
                         "e.high_day, e.high_week, e.high_month, e.high_quarter, e.high_year, e.high_year3, e.high_year10, e.low_day, e.low_week, e.low_month, e.low_quarter, e.low_year, e.low_year3, e.low_year10, "
                         "q.resolution, q.kind, q.fetched, q.count, q.first_key, q.last_key, q.last_hash "
                         "FROM summary_edges e LEFT JOIN queues q ON q.edge = e.id WHERE e.dataset = ? ORDER BY e.id, q.resolution");
      q.bind(1, asInt(dataset_.value));
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
          e.listing.orderBook = q.getColumn(6).getString();
          e.listing.type = q.getColumn(7).getString();
          e.listing.name = q.getColumn(8).getString();
          e.quote.last = real(q.getColumn(9));
          e.quote.bid = real(q.getColumn(10));
          e.quote.ask = real(q.getColumn(11));
          e.quote.time = q.getColumn(12).isNull() ? 0 : q.getColumn(12).getInt64();
          e.previous_close = real(q.getColumn(13));
          e.previous_close_day = q.getColumn(14).isNull() ? 0 : q.getColumn(14).getInt64();
          for (size_t i = 0; i < kChangeCount; i++) {
            e.changes.pct[i] = real(q.getColumn(15 + static_cast<unsigned>(i)));
            e.ranges.high[i] = real(q.getColumn(22 + static_cast<unsigned>(i)));
            e.ranges.low[i] = real(q.getColumn(29 + static_cast<unsigned>(i)));
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
      dropReader();
      say(std::string("cannot read: ") + e.what());
      out.clear();
    }
    return out;
  }

  std::optional<StoredQueue> readQueue(const EdgeKey & edge, Resolution resolution) override {
    if (!usable_) return std::nullopt;
    std::lock_guard lock(read_mutex_);
    try {
      maria::Statement q(reader(),
                         "SELECT q.kind, q.fetched, q.data FROM queues q JOIN summary_edges e ON e.id = q.edge "
                         "WHERE e.dataset = ? AND e.base = ? AND e.quote = ? AND e.market = ? AND q.resolution = ?");
      q.bind(1, asInt(dataset_.value));
      q.bind(2, asInt(edge.base.value));
      q.bind(3, asInt(edge.quote.value));
      q.bind(4, asInt(edge.market.value));
      q.bind(5, static_cast<int>(resolution));
      if (!q.executeStep()) return std::nullopt;
      StoredQueue out;
      out.edge = edge;
      out.resolution = resolution;
      out.fetched = q.getColumn(1).getInt64();
      if (!summary::unpack(q.getColumn(2).getBlob(), out.kind, out.bars)) {
        say("a queue (" + std::string(name(resolution)) + ") is not one of this program's, so it is left out");
        return std::nullopt;
      }
      return out;
    } catch (const std::exception & e) {
      dropReader();
      say(std::string("cannot read: ") + e.what());
      return std::nullopt;
    }
  }

  void apply(const SummaryBatch & batch) override {
    if (!usable_ || batch.empty()) return;
    std::lock_guard lock(write_mutex_);
    try {
      maria::Transaction transaction(writer());
      for (const StoredEdge & e : batch.edges) putEdge(e);
      for (const StoredQueue & q : batch.queues) putQueue(q);
      transaction.commit();
    } catch (const std::exception & e) {
      dropWriter();  // the batch is not written, whole: the server rolls back what the connection had open when it ends
      say(std::string("cannot write: ") + e.what());
      throw;  // the writer that called counts it (Store::failedWrites), and the ingest says so
    }
  }

 private:
  static double real(const maria::Column & c) { return c.isNull() ? kUndefined : c.getDouble(); }
  static void bindReal(maria::Statement & s, int at, double v) {
    if (isDefined(v)) s.bind(at, v);
    else s.bind(at);
  }

  void putEdge(const StoredEdge & e) {
    maria::Statement put(writer(),
                         "INSERT INTO summary_edges (dataset, base, quote, market, symbol, identifier, order_book, type, name, last_price, bid, ask, quote_time, "
                         "previous_close, previous_close_day, change_day, change_week, change_month, change_quarter, change_year, change_year3, change_year10, "
                         "high_day, high_week, high_month, high_quarter, high_year, high_year3, high_year10, low_day, low_week, low_month, low_quarter, low_year, low_year3, low_year10) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                         "ON DUPLICATE KEY UPDATE symbol = VALUES(symbol), identifier = VALUES(identifier), order_book = VALUES(order_book), type = VALUES(type), "
                         "name = VALUES(name), last_price = VALUES(last_price), bid = VALUES(bid), ask = VALUES(ask), quote_time = VALUES(quote_time), "
                         "previous_close = VALUES(previous_close), previous_close_day = VALUES(previous_close_day), change_day = VALUES(change_day), "
                         "change_week = VALUES(change_week), change_month = VALUES(change_month), change_quarter = VALUES(change_quarter), "
                         "change_year = VALUES(change_year), change_year3 = VALUES(change_year3), change_year10 = VALUES(change_year10), "
                         "high_day = VALUES(high_day), high_week = VALUES(high_week), high_month = VALUES(high_month), high_quarter = VALUES(high_quarter), high_year = VALUES(high_year), high_year3 = VALUES(high_year3), high_year10 = VALUES(high_year10), "
                         "low_day = VALUES(low_day), low_week = VALUES(low_week), low_month = VALUES(low_month), low_quarter = VALUES(low_quarter), low_year = VALUES(low_year), low_year3 = VALUES(low_year3), low_year10 = VALUES(low_year10)");
    const EdgeKey & k = e.listing.key;
    put.bind(1, asInt(dataset_.value));
    put.bind(2, asInt(k.base.value));
    put.bind(3, asInt(k.quote.value));
    put.bind(4, asInt(k.market.value));
    put.bind(5, e.listing.symbol);
    put.bind(6, e.listing.identifier);
    put.bind(7, e.listing.orderBook);
    put.bind(8, e.listing.type);
    put.bind(9, e.listing.name);
    bindReal(put, 10, e.quote.last);
    bindReal(put, 11, e.quote.bid);
    bindReal(put, 12, e.quote.ask);
    if (e.quote.time != 0) put.bind(13, static_cast<int64_t>(e.quote.time));
    else put.bind(13);
    bindReal(put, 14, e.previous_close);
    if (isDefined(e.previous_close)) put.bind(15, e.previous_close_day);
    else put.bind(15);
    for (size_t i = 0; i < kChangeCount; i++) {
      bindReal(put, 16 + static_cast<int>(i), e.changes.pct[i]);
      bindReal(put, 23 + static_cast<int>(i), e.ranges.high[i]);
      bindReal(put, 30 + static_cast<int>(i), e.ranges.low[i]);
    }
    put.exec();
  }

  // The row of an edge, made bare where it has none.
  int64_t edgeId(const EdgeKey & k) {
    {
      maria::Statement q(writer(), "SELECT id FROM summary_edges WHERE dataset = ? AND base = ? AND quote = ? AND market = ?");
      q.bind(1, asInt(dataset_.value));
      q.bind(2, asInt(k.base.value));
      q.bind(3, asInt(k.quote.value));
      q.bind(4, asInt(k.market.value));
      if (q.executeStep()) return q.getColumn(0).getInt64();
    }
    maria::Statement make(writer(), "INSERT INTO summary_edges (dataset, base, quote, market) VALUES (?, ?, ?, ?)");
    make.bind(1, asInt(dataset_.value));
    make.bind(2, asInt(k.base.value));
    make.bind(3, asInt(k.quote.value));
    make.bind(4, asInt(k.market.value));
    make.exec();
    return static_cast<int64_t>(make.lastInsertId());
  }

  void putQueue(const StoredQueue & in) {
    if (in.merge && in.bars.empty()) {
      maria::Statement touch(writer(),
                             "UPDATE queues SET fetched = ? WHERE resolution = ? AND edge = "
                             "(SELECT id FROM summary_edges WHERE dataset = ? AND base = ? AND quote = ? AND market = ?)");
      touch.bind(1, static_cast<int64_t>(in.fetched));
      touch.bind(2, static_cast<int>(in.resolution));
      touch.bind(3, asInt(dataset_.value));
      touch.bind(4, asInt(in.edge.base.value));
      touch.bind(5, asInt(in.edge.quote.value));
      touch.bind(6, asInt(in.edge.market.value));
      touch.exec();
      return;
    }
    const int64_t edge = edgeId(in.edge);
    StoredQueue q = in;
    if (in.merge) {
      // Into what the database has: read here, on the thread that writes, so that the store never has to read it to say a bar.
      maria::Statement old(writer(), "SELECT data FROM queues WHERE edge = ? AND resolution = ?");
      old.bind(1, edge);
      old.bind(2, static_cast<int>(in.resolution));
      std::vector<Candle> bars;
      if (old.executeStep()) {
        QueueKind kind = in.kind;
        std::vector<Candle> kept;
        if (summary::unpack(old.getColumn(0).getBlob(), kind, kept) && kind == in.kind) bars = std::move(kept);  // a queue of the other kind is not mixed in
      }
      old.reset();
      for (const Candle & c : in.bars) summary::put(bars, c);
      summary::trim(bars, in.resolution, in.keep);
      q.bars = std::move(bars);
    }
    std::erase_if(q.bars, [](const Candle & c) { return !summary::fits(c); });
    if (q.bars.empty()) {
      maria::Statement gone(writer(), "DELETE FROM queues WHERE edge = ? AND resolution = ?");
      gone.bind(1, edge);
      gone.bind(2, static_cast<int>(in.resolution));
      gone.exec();
      return;
    }
    const std::string blob = summary::pack(q.kind, q.bars);
    maria::Statement put(writer(),
                         "INSERT INTO queues (edge, resolution, kind, fetched, count, first_key, last_key, last_hash, data) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
                         "ON DUPLICATE KEY UPDATE kind = VALUES(kind), fetched = VALUES(fetched), count = VALUES(count), first_key = VALUES(first_key), "
                         "last_key = VALUES(last_key), last_hash = VALUES(last_hash), data = VALUES(data)");
    put.bind(1, edge);
    put.bind(2, static_cast<int>(in.resolution));
    put.bind(3, static_cast<int>(q.kind));
    put.bind(4, static_cast<int64_t>(q.fetched));
    put.bind(5, static_cast<int64_t>(q.bars.size()));
    put.bind(6, q.bars.front().time);
    put.bind(7, q.bars.back().time);
    put.bind(8, static_cast<int64_t>(summary::hashOf(q.kind, summary::normalized(q.kind, q.bars.back()))));
    put.bind(9, blob.data(), blob.size());
    put.exec();
  }

  DatasetId dataset_;
};

class MariaDbSummaries : public SummaryStores {
 public:
  MariaDbSummaries(maria::Config config, FactLog log) : config_(std::move(config)), log_(std::move(log)), setup_(std::make_shared<std::mutex>()) {}

  std::vector<std::string> datasets() override {
    std::vector<std::string> out;
    try {
      maria::Connection db(config_);
      db.exec("CREATE TABLE IF NOT EXISTS summary_datasets (dataset BIGINT NOT NULL PRIMARY KEY, code VARCHAR(16) NOT NULL) ENGINE=InnoDB");
      maria::Statement q(db, "SELECT code FROM summary_datasets ORDER BY code");
      while (q.executeStep()) out.push_back(q.getColumn(0).getString());
    } catch (const std::exception & e) {
      if (log_) log_(std::string("summaries: cannot list the datasets: ") + e.what());
    }
    return out;
  }

  std::shared_ptr<SummaryStore> open(const std::string & dataset) override {
    const auto key = datasetKey(dataset);
    if (!key) return nullptr;
    std::lock_guard lock(mutex_);
    auto & store = stores_[dataset];
    if (!store) {
      store = std::make_shared<MariaDbSummaryStore>(config_, DatasetId{key->value}, log_, setup_);
      try {
        maria::Connection db(config_);
        db.exec("CREATE TABLE IF NOT EXISTS summary_datasets (dataset BIGINT NOT NULL PRIMARY KEY, code VARCHAR(16) NOT NULL) ENGINE=InnoDB");
        maria::Statement put(db, "INSERT INTO summary_datasets (dataset, code) VALUES (?, ?) ON DUPLICATE KEY UPDATE code = VALUES(code)");
        put.bind(1, asInt(key->value));
        put.bind(2, dataset);
        put.exec();
      } catch (const std::exception & e) {
        if (log_) log_(std::string("summaries: cannot note the dataset: ") + e.what());
      }
    }
    return store;
  }

 private:
  maria::Config config_;
  FactLog log_;
  std::shared_ptr<std::mutex> setup_;
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<SummaryStore>> stores_;
};

// ---- the bars ----

constexpr int kBarsVersion = 1;

class MariaDbBarStore : public BarStore, private Base {
 public:
  MariaDbBarStore(const maria::Config & config, FactLog log) : Base(config, std::move(log), "bars") {
    try {
      ensureVersion(writer(), "bars", kBarsVersion, {"bars"}, [this](const std::string & t) { say(t); });
      writer().exec(
          "CREATE TABLE IF NOT EXISTS bars ("
          "base BIGINT NOT NULL, quote BIGINT NOT NULL, market BIGINT NOT NULL, dataset BIGINT NOT NULL, resolution INT NOT NULL, "
          "time BIGINT NOT NULL, open DOUBLE NOT NULL, high DOUBLE NOT NULL, low DOUBLE NOT NULL, close DOUBLE NOT NULL, volume DOUBLE NULL, "
          "PRIMARY KEY (base, quote, market, dataset, resolution, time)) ENGINE=InnoDB");
      usable_ = true;
    } catch (const std::exception & e) {
      db_.reset();
      usable_ = false;
      say(std::string("cannot open the database: ") + e.what() + "; bars are not kept");
    }
  }

  void apply(const BarBatch & batch) override {
    if (!usable_) return;
    std::lock_guard lock(write_mutex_);
    try {
      maria::Transaction transaction(writer());
      maria::Statement put(writer(),
                           "INSERT INTO bars (base, quote, market, dataset, resolution, time, open, high, low, close, volume) "
                           "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                           "ON DUPLICATE KEY UPDATE open = VALUES(open), high = VALUES(high), low = VALUES(low), close = VALUES(close), volume = VALUES(volume)");
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
        }
      transaction.commit();
    } catch (const std::exception & e) {
      dropWriter();
      say(std::string("cannot write: ") + e.what());
      throw;
    }
  }

  void read(const EdgeKey & edge, Resolution resolution, int64_t first, int64_t last, const std::function<void(const Candle &)> & each) override {
    if (!usable_) return;
    std::lock_guard lock(read_mutex_);
    try {
      maria::Statement q(reader(),
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
    } catch (const std::exception & e) {
      dropReader();
      say(std::string("cannot read: ") + e.what());
      throw;
    }
  }
};

}  // namespace

std::shared_ptr<FactStore> openMariaDbFactStore(const maria::Config & config, FactLog log) { return std::make_shared<MariaDbFactStore>(config, std::move(log)); }
std::shared_ptr<SummaryStores> openMariaDbSummaries(const maria::Config & config, FactLog log) { return std::make_shared<MariaDbSummaries>(config, std::move(log)); }
std::shared_ptr<BarStore> openMariaDbBarStore(const maria::Config & config, FactLog log) { return std::make_shared<MariaDbBarStore>(config, std::move(log)); }
