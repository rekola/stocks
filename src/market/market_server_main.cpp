// market-server: the read-only HTTP API of the market data, for the desktop app. No authentication: it listens on the loopback
// address unless the config says otherwise, and the database user it connects as should be allowed to read only.
//
//   market-server [--config FILE] [--port N] [--listen ADDRESS]
//
//   GET /health   GET /instruments   GET /summary[?codes=a,b]        (see market/Api.h)
#include "market/Api.h"
#include "market/Config.h"
#include "market/SqlStores.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace {

market::ApiServer * g_server = nullptr;
void onSignal(int) {
  if (g_server) g_server->stop();
}

}  // namespace

int main(int argc, char ** argv) {
  std::string configPath;
  bool explicitConfig = false;
  int port = -1;
  std::string listen;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) configPath = argv[++i], explicitConfig = true;
    else if (arg == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
    else if (arg == "--listen" && i + 1 < argc) listen = argv[++i];
    else {
      std::fprintf(stderr, "usage: market-server [--config FILE] [--port N] [--listen ADDRESS]\n");
      return arg == "--help" || arg == "-h" ? 0 : 2;
    }
  }
  if (configPath.empty()) {
    const char * env = std::getenv("MARKET_CONFIG");
    configPath = env ? env : "market.json";
    explicitConfig = env != nullptr;
  }
  market::Config config;
  std::string error;
  if ((explicitConfig || std::filesystem::exists(configPath)) && !market::loadConfig(configPath, config, error)) {
    std::fprintf(stderr, "market-server: %s\n", error.c_str());
    return 2;
  }
  if (port >= 0) config.port = port;
  if (!listen.empty()) config.listen = listen;

  try {
    auto store = market::openStore(config.database, market::OpenMode::ReadOnly);
    market::ApiServer server(*store);
    g_server = &server;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    if (!server.bind(config.listen, config.port, error)) {
      std::fprintf(stderr, "market-server: %s\n", error.c_str());
      return 1;
    }
    std::fprintf(stderr, "market-server: listening on %s:%d\n", config.listen.c_str(), server.port());
    server.serve();
    g_server = nullptr;
    return 0;
  } catch (const std::exception & e) {
    std::fprintf(stderr, "market-server: %s\n", e.what());
    return 1;
  }
}
