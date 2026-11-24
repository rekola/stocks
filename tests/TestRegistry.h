#pragma once

#include "model/Store.h"
#include "source/IsinFile.h"

#include <memory>
#include <string>

// The registry the program ships with (data/isins/registry.csv), read once, and a store that keeps what has no ISIN by it.
inline std::shared_ptr<const IsinRegistry> shippedRegistry() {
  static const std::shared_ptr<const IsinRegistry> registry = [] {
    IsinRegistry r;
    std::string error;
    if (!loadIsinRegistry(r, error)) std::fprintf(stderr, "  registry: %s\n", error.c_str());
    return std::make_shared<const IsinRegistry>(std::move(r));
  }();
  return registry;
}

inline std::unique_ptr<Store> storeWithRegistry() {
  StoreOptions options;
  options.isins = shippedRegistry();
  return makeMemoryStore(std::move(options));
}
