#pragma once

#include <string>

// The settings of the market-data programs, from one JSON file (market.example.json at the top of the repository is a model):
//
//   { "database": { "host": "localhost", "port": 3306, "socket": "", "user": "market", "password": "", "name": "market" },
//     "datasets": { "histdata": { "cache_dir": "cache/histdata" } },
//     "server":   { "listen": "127.0.0.1", "port": 8080 } }
//
// A relative path is relative to the directory of the file. The environment variable MARKET_DB_PASSWORD, when set, replaces the
// database password, so that the file need not hold it.
namespace market {

struct DbConfig {
  std::string host = "localhost";
  int port = 3306;
  std::string socket;  // a Unix socket path; used for the host "localhost" where set (the server's default otherwise)
  std::string user = "market";
  std::string password;
  std::string name = "market";  // the schema (database) of the market data, apart from any other
};

struct Config {
  DbConfig database;
  std::string histdataCacheDir = "cache/histdata";
  std::string listen = "127.0.0.1";  // there is no authentication: only the local machine can reach it unless this says otherwise
  int port = 8080;
};

// Reads the file; false, with `error`, where it cannot be read or holds something else than the above.
bool loadConfig(const std::string & path, Config & config, std::string & error);

}  // namespace market
