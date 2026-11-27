// market-server: the read-only HTTP API of the market data, for the desktop app. No authentication: it listens on the loopback address unless the
// config says otherwise. It reads the application's own Store, restored from the database the ingest writes (the node attributes, the summaries
// of the edges and their queues) once, when it starts. The ingest is offline: it is run while the server is not, and what the server keeps up to date
// afterwards it will write itself (from a realtime source, later).
//
//   market-server [--config FILE] [--port N] [--listen ADDRESS]
//
//   GET /health   GET /instruments   GET /summary[?edge=ID,..&dataset=CODE&resolution=R,..]        (see market/Api.h)
#include "market/Api.h"
#include "market/Backends.h"
#include "market/Config.h"

#include <charconv>
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
    else if (arg == "--port" && i + 1 < argc) {
      // The whole of the text is the number, and it is a port: atoi would take "80x" for 80 and "x" for 0.
      const std::string text = argv[++i];
      int value = -1;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (text.empty() || parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || value < 0 || value > 65535) {
        std::fprintf(stderr, "market-server: --port is a number from 0 to 65535, not \"%s\"\n", text.c_str());
        return 2;
      }
      port = value;
    }
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
  // A store as the database has it, with its queues in memory.
  auto restore = [&]() {
    StoreOptions options = market::storeOptionsOf(config.database, log);
    auto store = std::shared_ptr<Store>(makeMemoryStore(std::move(options)));
    market::loadQueues(*store);
    return store;
  };

  try {
    const std::shared_ptr<Store> current = restore();
    market::ApiServer server([&] { return current; });
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
