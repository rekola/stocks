#include "market/SqlStore.h"

#include <sqlpp11/mysql/mysql.h>

namespace market {

namespace {

std::shared_ptr<sqlpp::mysql::connection_config> configOf(const DbConfig & config, bool withSchema) {
  auto c = std::make_shared<sqlpp::mysql::connection_config>();
  c->host = config.host;
  c->port = static_cast<unsigned>(config.port);
  c->user = config.user;
  c->password = config.password;
  if (withSchema) c->database = config.name;
  if ((config.host == "localhost" || config.host.empty()) && !config.socket.empty()) c->unix_socket = config.socket;
  c->charset = "utf8mb4";
  return c;
}

std::string quoted(const std::string & identifier) {
  std::string out = "`";
  for (char ch : identifier) out += ch == '`' ? "``" : std::string(1, ch);
  return out + "`";
}

struct MariaDbBackend {
  using Connection = sqlpp::mysql::connection;
  static constexpr const char * kTableTail = " ENGINE=InnoDB";
  static constexpr const char * kClusteredTableTail = " ENGINE=InnoDB";  // InnoDB clusters on the primary key

  static std::unique_ptr<Connection> connect(const DbConfig & config, bool create) {
    if (create) {
      Connection bootstrap(configOf(config, /*withSchema=*/false));
      bootstrap.execute("CREATE DATABASE IF NOT EXISTS " + quoted(config.name) + " CHARACTER SET utf8mb4");
    }
    return std::make_unique<Connection>(configOf(config, /*withSchema=*/true));
  }
  static std::string describe(const DbConfig & config) { return "the MariaDB schema `" + config.name + "`"; }
};

}  // namespace

std::unique_ptr<MarketStore> openMariaDbStore(const DbConfig & config, OpenMode mode, const Log & log) {
  return std::make_unique<SqlStore<MariaDbBackend>>(config, mode, log);
}

void dropMariaDbStore(const DbConfig & config) {
  sqlpp::mysql::connection connection(configOf(config, /*withSchema=*/false));
  connection.execute("DROP DATABASE IF EXISTS " + quoted(config.name));
}

}  // namespace market
