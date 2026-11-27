// market-server: the read-only HTTP API of the market data, for the desktop app. No authentication: it listens on the loopback address unless the
// config says otherwise. It reads the application's own Store, restored from the database the ingest writes (the node attributes, the summaries
// of the edges and their queues), and renews it every `refresh_seconds`, so that what the ingest wrote since is served.
//
//   market-server [--config FILE] [--port N] [--listen ADDRESS]
//
//   GET /health   GET /instruments   GET /summary[?codes=a,b]        (see market/Api.h)
#include "market/Api.h"
#include "market/Backends.h"
#include "market/Config.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

  const FactLog log = [](const std::string & line) { std::fprintf(stderr, "market-server: %s\n", line.c_str()); };
  // A store as the database has it now, with its queues in memory.
  auto restore = [&]() {
    StoreOptions options = market::storeOptionsOf(config.database, log);
    auto store = std::shared_ptr<Store>(makeMemoryStore(std::move(options)));
    market::loadQueues(*store);
    return store;
  };

  try {
    std::mutex mutex;
    std::shared_ptr<Store> current = restore();
    market::ApiServer server([&] {
      std::lock_guard lock(mutex);
      return current;
    });
    g_server = &server;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    if (!server.bind(config.listen, config.port, error)) {
      std::fprintf(stderr, "market-server: %s\n", error.c_str());
      return 1;
    }
    std::atomic<bool> stopping{false};
    std::thread renew([&] {
      for (int waited = 0; !stopping;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++waited < config.refreshSeconds) continue;
        waited = 0;
        try {
          auto fresh = restore();
          std::lock_guard lock(mutex);
          current = std::move(fresh);
        } catch (const std::exception & e) {
          log(std::string("could not read the database again: ") + e.what());
        }
      }
    });
    std::fprintf(stderr, "market-server: listening on %s:%d\n", config.listen.c_str(), server.port());
    server.serve();
    g_server = nullptr;
    stopping = true;
    renew.join();
    return 0;
  } catch (const std::exception & e) {
    std::fprintf(stderr, "market-server: %s\n", e.what());
    return 1;
  }
}
