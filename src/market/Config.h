#pragma once

#include <string>

// The settings of the market-data programs, from one JSON file (market.example.json at the top of the repository is a model):
//
//   { "database": { "type": "mariadb", "host": "localhost", "port": 3306, "socket": "", "user": "market", "password": "", "name": "market" },
//                    or { "type": "sqlite", "path": "market-data" } },
//     "datasets": { "histdata": { "cache_dir": "cache/histdata" } },
//     "server":   { "listen": "127.0.0.1", "port": 8080, "refresh_seconds": 60 } }
//
// A port is 0 to 65535 (0: any free one, for a server) and refresh_seconds at least 1; anything else is refused. A relative path is relative to the directory of the file. The environment variable MARKET_DB_PASSWORD, when set, replaces the
// database password, so that the file need not hold it.
namespace market {

struct DbConfig {
  std::string type = "sqlite";  // "sqlite" or "mariadb" (which needs a build with libmariadb): what the stores keep is the same either way
  std::string path = "market-data";  // sqlite: the directory of the files (facts.sqlite, bars.sqlite, summaries/); relative to the config file
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
  int refreshSeconds = 60;  // how often the server reads what the ingest wrote since: 1 or more (there is no "never": restart the server)
};

// Reads the file; false, with `error`, where it cannot be read or holds something else than the above.
bool loadConfig(const std::string & path, Config & config, std::string & error);

}  // namespace market
