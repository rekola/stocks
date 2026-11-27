#include "TestFramework.h"
#include "MarketTestSupport.h"
#include "market/Config.h"

#include <fstream>

using namespace market;

namespace {

// The result of reading a config file with this text: the error, empty where it was read.
std::string errorOf(const std::string & json, Config * out = nullptr) {
  TempDir dir;
  const auto file = dir.path / "market.json";
  std::ofstream(file) << json;
  Config config;
  std::string error;
  const bool ok = loadConfig(file.string(), config, error);
  if (out) *out = config;
  return ok ? std::string() : (error.empty() ? "failed" : error);
}

}  // namespace

TEST(market_config_defaults_to_sqlite_which_every_build_has) {
  const Config config;
  CHECK(config.database.type == "sqlite");
  CHECK(hasBackend(config.database.type));
  Config read;
  CHECK(errorOf("{}", &read).empty() && read.database.type == "sqlite");
  CHECK(errorOf(R"({"database": {"type": "oracle"}})").find("database.type") != std::string::npos);
  CHECK(errorOf(R"({"database": {"type": "mariadb"}})").empty());  // asking for it is allowed; a build without it says so when it opens it
}

TEST(market_config_refuses_a_refresh_that_never_comes_and_ports_that_are_none) {
  CHECK(errorOf(R"({"server": {"refresh_seconds": 0}})").find("refresh_seconds") != std::string::npos);
  CHECK(errorOf(R"({"server": {"refresh_seconds": -5}})").find("refresh_seconds") != std::string::npos);
  CHECK(errorOf(R"({"server": {"refresh_seconds": 1}})").empty());
  CHECK(errorOf(R"({"server": {"port": 65536}})").find("server.port") != std::string::npos);
  CHECK(errorOf(R"({"server": {"port": -1}})").find("server.port") != std::string::npos);
  CHECK(errorOf(R"({"server": {"port": 0}})").empty());  // any free one
  CHECK(errorOf(R"({"server": {"port": 65535}})").empty());
  CHECK(errorOf(R"({"database": {"port": 70000}})").find("database.port") != std::string::npos);
  CHECK(errorOf(R"({"server": {"port": "80"}})").find("server.port") != std::string::npos);  // a number, not text
}
