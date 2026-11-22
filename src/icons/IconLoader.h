#pragma once

#include "source/Http.h"

#include <cellui/Table.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Posted to the UI thread when icons have loaded: the table draws again.
struct IconsLoaded {};

// Where icons are kept between runs: stocks/icons under the user's cache directory.
std::filesystem::path defaultIconCacheDir();

// Loads instrument icons on a thread of its own, by URL: the same logo is shared by many instruments, so it is
// fetched, decoded and kept once. A caller says which URLs it needs (want(), the rows on screen and a margin) and asks
// what has arrived (get()); neither waits. An icon comes from memory, else from the disk cache, else from the network
// (one request at a time with a pause between them, and none after a 403 or 429).
//   Failures: a 404 (any other 4xx) and a body that is no image are remembered for the session. A refusal (403, 429)
//   stops all requests for the session. No answer or a 5xx (no network, a server in trouble) pauses requests for a
//   minute, and the URLs are asked for again afterwards. The disk cache and what is in memory keep working in all cases.
class IconLoader {
 public:
  struct Options {
    std::filesystem::path cache_dir;                       // empty: no disk cache
    size_t max_memory_bytes = 128u * 1024 * 1024;          // decoded pixels; least recently used leave first (disk serves the rest)
    uintmax_t max_disk_bytes = 512u * 1024 * 1024;         // the files as fetched; least recently used go first
    size_t burst = 100;                                    // requests in a row, then a pause (each burst 80-120 % of this)
    std::chrono::milliseconds burst_pause{2000};           // between bursts (each 50-150 % of this)
    std::chrono::milliseconds offline_pause{60000};        // after no answer
  };
  // `loaded` runs on the loader's thread after icons have arrived; a program hands it to the UI thread.
  IconLoader(std::unique_ptr<Http> http, Options options, std::function<void()> loaded);
  ~IconLoader();
  IconLoader(const IconLoader &) = delete;
  IconLoader & operator=(const IconLoader &) = delete;

  // The icon of `url` if it is in memory now; never waits for the thread, the disk or the network.
  std::shared_ptr<const cellui::TableIcon> get(const std::string & url);
  // What is wanted now, most wanted first. It replaces what was wanted before: URLs scrolled past are not loaded.
  void want(const std::vector<std::string> & urls);

  // Whether a refusal has stopped the network for this session.
  bool refused() const;
  // Blocks until nothing is queued or being loaded (for tests).
  void waitIdle();

 private:
  void run();
  // Loads one URL; returns whether an icon arrived.
  bool load(const std::string & url, bool & used_network);
  std::shared_ptr<const cellui::TableIcon> fromDisk(const std::string & url);
  void toDisk(const std::string & url, const std::string & bytes);
  void trimDisk();
  void remember(const std::string & url, std::shared_ptr<const cellui::TableIcon> icon);

  std::unique_ptr<Http> http_;
  Options options_;
  std::function<void()> loaded_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, idle_;
  std::list<std::pair<std::string, std::shared_ptr<const cellui::TableIcon>>> memory_;  // most recently used first
  std::unordered_map<std::string, decltype(memory_)::iterator> in_memory_;
  size_t memory_bytes_ = 0;
  std::unordered_set<std::string> failed_;
  std::list<std::string> queue_;
  bool busy_ = false, stop_ = false, refused_ = false;
  std::chrono::steady_clock::time_point offline_until_{};
  int disk_writes_ = 0;
  size_t sent_ = 0, burst_size_ = 0;  // requests in this burst, and where it ends
  std::mt19937 random_{std::random_device{}()};
  std::thread thread_;  // last: it starts in the constructor
};
