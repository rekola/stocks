#include "market/MariaDbStore.h"

#include "model/NodeKey.h"

#include <mysql.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <set>
#include <stdexcept>

namespace market {

namespace {

using Row = std::vector<std::string>;

// One connection, text protocol. Every number put into a statement is formatted here (an integer, or a double with 17 digits, which
// reads back as the same double), and every string is escaped, so nothing a provider or a user sent is ever part of the SQL.
class Connection {
 public:
  explicit Connection(const DbConfig & config, bool withSchema) : config_(config) { connect(withSchema); }
  ~Connection() {
    if (mysql_) mysql_close(mysql_);
  }
  Connection(const Connection &) = delete;
  Connection & operator=(const Connection &) = delete;

  void exec(const std::string & sql) {
    if (mysql_real_query(mysql_, sql.data(), static_cast<unsigned long>(sql.size())) != 0) fail("query failed", sql);
  }
  std::vector<Row> query(const std::string & sql) {
    exec(sql);
    MYSQL_RES * result = mysql_store_result(mysql_);
    if (!result) fail("no result", sql);
    std::vector<Row> rows;
    const unsigned columns = mysql_num_fields(result);
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
      Row out(columns);
      for (unsigned i = 0; i < columns; i++) out[i] = row[i] ? row[i] : "";
      rows.push_back(std::move(out));
    }
    mysql_free_result(result);
    return rows;
  }
  uint64_t insertId() { return mysql_insert_id(mysql_); }
  std::string quote(const std::string & text) {
    std::string out(text.size() * 2 + 1, '\0');
    const unsigned long n = mysql_real_escape_string(mysql_, out.data(), text.data(), static_cast<unsigned long>(text.size()));
    out.resize(n);
    return "'" + out + "'";
  }
  // Brings a connection that the server dropped (a timeout) back, or throws.
  void ensureAlive() {
    if (mysql_ping(mysql_) != 0) fail("the connection to the database is lost", "");
  }

 private:
  void connect(bool withSchema) {
    mysql_ = mysql_init(nullptr);
    if (!mysql_) throw std::runtime_error("mariadb: cannot start the client");
    mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    const bool reconnect = true;
    mysql_options(mysql_, MYSQL_OPT_RECONNECT, &reconnect);
    const bool local = config_.host == "localhost" || config_.host.empty();
    if (!mysql_real_connect(mysql_, config_.host.c_str(), config_.user.c_str(), config_.password.c_str(),
                            withSchema ? config_.name.c_str() : nullptr, static_cast<unsigned>(config_.port),
                            local && !config_.socket.empty() ? config_.socket.c_str() : nullptr, 0)) {
      const std::string message = mysql_error(mysql_);
      mysql_close(mysql_);
      mysql_ = nullptr;
      throw std::runtime_error("mariadb: cannot connect to " + config_.host + " as " + config_.user + ": " + message);
    }
  }
  [[noreturn]] void fail(const char * what, const std::string & sql) {
    std::string message = std::string("mariadb: ") + what + ": " + mysql_error(mysql_);
    if (!sql.empty()) message += " [" + sql.substr(0, 120) + (sql.size() > 120 ? "..." : "") + "]";
    throw std::runtime_error(message);
  }
  DbConfig config_;
  MYSQL * mysql_ = nullptr;
};

std::string u(uint64_t v) { return std::to_string(v); }
std::string i(int64_t v) { return std::to_string(v); }
std::string d(double v) {
  if (!std::isfinite(v)) throw std::runtime_error("mariadb: a bar holds a value that is not a number");
  char buffer[40];
  std::snprintf(buffer, sizeof buffer, "%.17g", v);
  return buffer;
}
uint64_t parseU(const std::string & s) { return std::strtoull(s.c_str(), nullptr, 10); }
int64_t parseI(const std::string & s) { return std::strtoll(s.c_str(), nullptr, 10); }
double parseD(const std::string & s) { return std::strtod(s.c_str(), nullptr); }

std::string where(const Edge & e) {
  return "base=" + u(e.base) + " AND quote=" + u(e.quote) + " AND market=" + u(e.market) + " AND dataset=" + u(e.dataset);
}

constexpr const char * kTables[] = {"bars", "loads", "instruments", "batches", "datasets", "meta"};

const char * const kCreate[] = {
    "CREATE TABLE meta (name VARCHAR(32) NOT NULL PRIMARY KEY, value VARCHAR(255) NOT NULL) ENGINE=InnoDB",
    "CREATE TABLE datasets (dataset BIGINT UNSIGNED NOT NULL PRIMARY KEY, code VARCHAR(32) NOT NULL UNIQUE, name VARCHAR(100) NOT NULL) "
    "ENGINE=InnoDB",
    "CREATE TABLE batches (version BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, dataset BIGINT UNSIGNED NOT NULL, "
    "started_at BIGINT NOT NULL, note VARCHAR(255) NOT NULL) ENGINE=InnoDB",
    "CREATE TABLE instruments (base BIGINT UNSIGNED NOT NULL, quote BIGINT UNSIGNED NOT NULL, market BIGINT UNSIGNED NOT NULL, "
    "dataset BIGINT UNSIGNED NOT NULL, code VARCHAR(32) NOT NULL, name VARCHAR(100) NOT NULL, asset_class VARCHAR(16) NOT NULL, "
    "first_period INT NOT NULL, PRIMARY KEY (base, quote, market, dataset), UNIQUE KEY by_code (dataset, code)) ENGINE=InnoDB",
    "CREATE TABLE loads (base BIGINT UNSIGNED NOT NULL, quote BIGINT UNSIGNED NOT NULL, market BIGINT UNSIGNED NOT NULL, "
    "dataset BIGINT UNSIGNED NOT NULL, resolution TINYINT UNSIGNED NOT NULL, version BIGINT UNSIGNED NOT NULL, loaded_at BIGINT NOT NULL, "
    "bar_count BIGINT NOT NULL, first_key BIGINT NOT NULL, last_key BIGINT NOT NULL, last_bar_time BIGINT NOT NULL, "
    "PRIMARY KEY (base, quote, market, dataset, resolution)) ENGINE=InnoDB",
    "CREATE TABLE bars (base BIGINT UNSIGNED NOT NULL, quote BIGINT UNSIGNED NOT NULL, market BIGINT UNSIGNED NOT NULL, "
    "dataset BIGINT UNSIGNED NOT NULL, resolution TINYINT UNSIGNED NOT NULL, bar_key BIGINT NOT NULL, open DOUBLE NOT NULL, "
    "high DOUBLE NOT NULL, low DOUBLE NOT NULL, close DOUBLE NOT NULL, version BIGINT UNSIGNED NOT NULL, "
    "PRIMARY KEY (base, quote, market, dataset, resolution, bar_key)) ENGINE=InnoDB",
};

class MariaDbStore : public MarketStore {
 public:
  MariaDbStore(const DbConfig & config, OpenMode mode, const Log & log) : config_(config) {
    if (mode == OpenMode::ReadWrite) {
      {
        Connection bootstrap(config, /*withSchema=*/false);
        bootstrap.exec("CREATE DATABASE IF NOT EXISTS `" + escapeIdentifier(config.name) + "` CHARACTER SET utf8mb4");
      }
    }
    connection_ = std::make_unique<Connection>(config, /*withSchema=*/true);
    prepareSchema(mode, log);
  }

  uint64_t putDataset(const std::string & code, const std::string & name) override {
    const auto key = datasetKey(code);
    if (!key) throw std::runtime_error("a dataset code is one to ten letters or digits: " + code);
    std::lock_guard lock(mutex_);
    connection_->ensureAlive();
    connection_->exec("INSERT INTO datasets (dataset, code, name) VALUES (" + u(key->value) + ", " + connection_->quote(code) + ", " +
                      connection_->quote(name) + ") ON DUPLICATE KEY UPDATE name=VALUES(name)");
    return key->value;
  }

  uint64_t beginBatch(uint64_t dataset, const std::string & note) override {
    std::lock_guard lock(mutex_);
    connection_->ensureAlive();
    connection_->exec("INSERT INTO batches (dataset, started_at, note) VALUES (" + u(dataset) + ", " + i(std::time(nullptr)) + ", " +
                      connection_->quote(note.substr(0, 255)) + ")");
    return connection_->insertId();
  }

  void putInstrument(const InstrumentRecord & r) override {
    std::lock_guard lock(mutex_);
    connection_->ensureAlive();
    connection_->exec("INSERT INTO instruments (base, quote, market, dataset, code, name, asset_class, first_period) VALUES (" +
                      u(r.edge.base) + ", " + u(r.edge.quote) + ", " + u(r.edge.market) + ", " + u(r.edge.dataset) + ", " +
                      connection_->quote(r.code) + ", " + connection_->quote(r.name) + ", " + connection_->quote(r.assetClass) + ", " +
                      i(r.firstPeriod) + ") ON DUPLICATE KEY UPDATE code=VALUES(code), name=VALUES(name), "
                      "asset_class=VALUES(asset_class), first_period=VALUES(first_period)");
  }

  void replaceBars(const Edge & edge, Resolution level, const std::vector<Bar> & bars, Timestamp lastBarTime, uint64_t version) override {
    std::lock_guard lock(mutex_);
    connection_->ensureAlive();
    const std::string prefix = u(edge.base) + ", " + u(edge.quote) + ", " + u(edge.market) + ", " + u(edge.dataset) + ", " +
                               u(static_cast<uint8_t>(level));
    connection_->exec("START TRANSACTION");
    try {
      connection_->exec("DELETE FROM bars WHERE " + where(edge) + " AND resolution=" + u(static_cast<uint8_t>(level)));
      constexpr size_t kChunk = 500;
      for (size_t at = 0; at < bars.size(); at += kChunk) {
        std::string sql = "INSERT INTO bars (base, quote, market, dataset, resolution, bar_key, open, high, low, close, version) VALUES ";
        for (size_t k = at; k < std::min(bars.size(), at + kChunk); k++) {
          const Bar & b = bars[k];
          if (k != at) sql += ", ";
          sql += "(" + prefix + ", " + i(b.key) + ", " + d(b.open) + ", " + d(b.high) + ", " + d(b.low) + ", " + d(b.close) + ", " +
                 u(version) + ")";
        }
        connection_->exec(sql);
      }
      connection_->exec("REPLACE INTO loads (base, quote, market, dataset, resolution, version, loaded_at, bar_count, first_key, "
                        "last_key, last_bar_time) VALUES (" + prefix + ", " + u(version) + ", " + i(std::time(nullptr)) + ", " +
                        i(static_cast<int64_t>(bars.size())) + ", " + i(bars.empty() ? 0 : bars.front().key) + ", " +
                        i(bars.empty() ? 0 : bars.back().key) + ", " + i(lastBarTime) + ")");
      connection_->exec("COMMIT");
    } catch (...) {
      try {
        connection_->exec("ROLLBACK");
      } catch (...) {
      }
      throw;
    }
  }

  std::vector<InstrumentInfo> instruments() override {
    std::lock_guard lock(mutex_);
    connection_->ensureAlive();
    std::vector<InstrumentInfo> out;
    for (const Row & r : connection_->query("SELECT base, quote, market, dataset, code, name, asset_class, first_period FROM instruments "
                                            "ORDER BY code")) {
      InstrumentInfo info;
      info.instrument.edge = {parseU(r[0]), parseU(r[1]), parseU(r[2]), parseU(r[3])};
      info.instrument.code = r[4];
      info.instrument.name = r[5];
      info.instrument.assetClass = r[6];
      info.instrument.firstPeriod = static_cast<int>(parseI(r[7]));
      out.push_back(std::move(info));
    }
    for (const Row & r : connection_->query("SELECT base, quote, market, dataset, resolution, version, loaded_at, bar_count, first_key, "
                                            "last_key, last_bar_time FROM loads ORDER BY resolution")) {
      const Edge edge{parseU(r[0]), parseU(r[1]), parseU(r[2]), parseU(r[3])};
      LevelState state;
      state.level = static_cast<Resolution>(parseU(r[4]));
      state.version = parseU(r[5]);
      state.loadedAt = parseI(r[6]);
      state.barCount = parseI(r[7]);
      state.firstKey = parseI(r[8]);
      state.lastKey = parseI(r[9]);
      state.lastBarTime = parseI(r[10]);
      for (InstrumentInfo & info : out)
        if (info.instrument.edge == edge) info.levels.push_back(state);
    }
    return out;
  }

  std::vector<Bar> bars(const Edge & edge, Resolution level, int64_t firstKey, int64_t lastKey) override {
    std::lock_guard lock(mutex_);
    connection_->ensureAlive();
    std::vector<Bar> out;
    for (const Row & r : connection_->query("SELECT bar_key, open, high, low, close FROM bars WHERE " + where(edge) + " AND resolution=" +
                                            u(static_cast<uint8_t>(level)) + " AND bar_key BETWEEN " + i(firstKey) + " AND " + i(lastKey) +
                                            " ORDER BY bar_key"))
      out.push_back({parseI(r[0]), parseD(r[1]), parseD(r[2]), parseD(r[3]), parseD(r[4])});
    return out;
  }

  bool healthy() override {
    try {
      std::lock_guard lock(mutex_);
      connection_->ensureAlive();
      return connection_->query("SELECT 1").size() == 1;
    } catch (const std::exception &) {
      return false;
    }
  }

 private:
  static std::string escapeIdentifier(const std::string & name) {
    std::string out;
    for (char c : name) {
      if (c == '`') out += '`';
      out += c;
    }
    return out;
  }

  void prepareSchema(OpenMode mode, const Log & log) {
    std::set<std::string> tables;
    for (const Row & r : connection_->query("SHOW TABLES")) tables.insert(r[0]);
    int found = 0;
    if (tables.count("meta")) {
      const auto rows = connection_->query("SELECT value FROM meta WHERE name='schema_version'");
      if (!rows.empty()) found = static_cast<int>(parseI(rows[0][0]));
    }
    if (found == kSchemaVersion) return;
    if (mode == OpenMode::ReadOnly)
      throw std::runtime_error("mariadb: the schema `" + config_.name + "` is " +
                               (found ? "version " + std::to_string(found) : std::string("not there")) + ", this program reads version " +
                               std::to_string(kSchemaVersion) + ": run market-ingest to make it");
    // Nothing is kept across a change of schema: the tables are made again, and what was in them is loaded again from the raw files.
    if (!tables.empty() && log) log("mariadb: the schema `" + config_.name + "` is " + (found ? "version " + std::to_string(found) : std::string("unversioned")) + ", starting over at version " + std::to_string(kSchemaVersion));
    for (const char * table : kTables) connection_->exec(std::string("DROP TABLE IF EXISTS ") + table);
    for (const char * sql : kCreate) connection_->exec(sql);
    connection_->exec("INSERT INTO meta (name, value) VALUES ('schema_version', '" + std::to_string(kSchemaVersion) + "')");
  }

  DbConfig config_;
  std::mutex mutex_;
  std::unique_ptr<Connection> connection_;
};

}  // namespace

std::unique_ptr<MarketStore> openMariaDbStore(const DbConfig & config, OpenMode mode, const Log & log) {
  return std::make_unique<MariaDbStore>(config, mode, log);
}

void dropMariaDbSchema(const DbConfig & config) {
  Connection connection(config, /*withSchema=*/false);
  std::string name;
  for (char c : config.name) {
    if (c == '`') name += '`';
    name += c;
  }
  connection.exec("DROP DATABASE IF EXISTS `" + name + "`");
}

}  // namespace market
