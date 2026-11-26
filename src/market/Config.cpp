#include "market/Config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace market {

namespace {

// Reads a member of the given type into `out`, if it is there; false where it is there and of another type.
template <class T>
bool take(const nlohmann::json & object, const char * name, T & out, std::string & error, const std::string & where) {
  const auto it = object.find(name);
  if (it == object.end()) return true;
  try {
    out = it->get<T>();
  } catch (const nlohmann::json::exception &) {
    error = where + name + " has the wrong type";
    return false;
  }
  return true;
}

}  // namespace

bool loadConfig(const std::string & path, Config & config, std::string & error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot read " + path;
    return false;
  }
  nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
  if (!root.is_object()) {
    error = path + ": not a JSON object";
    return false;
  }
  const auto section = [&](const char * name) -> const nlohmann::json * {
    const auto it = root.find(name);
    return it != root.end() && it->is_object() ? &*it : nullptr;
  };
  if (const auto * db = section("database")) {
    DbConfig & d = config.database;
    if (!take(*db, "host", d.host, error, "database.") || !take(*db, "port", d.port, error, "database.") ||
        !take(*db, "socket", d.socket, error, "database.") || !take(*db, "user", d.user, error, "database.") ||
        !take(*db, "password", d.password, error, "database.") || !take(*db, "name", d.name, error, "database."))
      return false;
  }
  if (const auto * datasets = section("datasets"))
    if (auto it = datasets->find("histdata"); it != datasets->end() && it->is_object())
      if (!take(*it, "cache_dir", config.histdataCacheDir, error, "datasets.histdata.")) return false;
  if (const auto * server = section("server"))
    if (!take(*server, "listen", config.listen, error, "server.") || !take(*server, "port", config.port, error, "server.")) return false;
  if (const char * password = std::getenv("MARKET_DB_PASSWORD")) config.database.password = password;

  const std::filesystem::path base = std::filesystem::path(path).parent_path();
  std::filesystem::path cache = config.histdataCacheDir;
  if (cache.is_relative()) cache = base / cache;
  config.histdataCacheDir = cache.lexically_normal().string();
  return true;
}

}  // namespace market
