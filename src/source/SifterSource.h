#pragma once

#include "source/DatasetCodes.h"
#include "source/Source.h"

#include <chrono>
#include <string>
#include <vector>

// The Sifter Fund's share classes (Altus Investor's fund pages, ir.altusinvestor.com). The fund has no list to discover and no
// history call of its own: one request gives every daily NAV of every share class, `[{"d": "2026-11-19", "cr": 12.3, "c": 11.2,
// "cpi": 10.1}, ...]` (`cr` the PA class, `c` the PB, `cpi` the PI; a class that has no NAV that day has no member). So the three
// classes are known here, and their history is loaded with them, not by a HistoryProvider. No key. The dataset is "SIFTER".
class SifterSource : public Source {
 public:
  explicit SifterSource(std::chrono::milliseconds refresh = std::chrono::hours(6)) : refresh_(refresh) {}

  std::string name() const override { return "sifter"; }
  std::vector<std::string> datasetCodes() const override { return {kSifterDataset}; }
  std::string description() const override { return "Sifter Fund Global: the NAV history of its PA, PB and PI share classes (no key)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }

  // The answer into the store. False, with `error` set, when it is not a list of days. A row that cannot be read (no date, or a value that
  // is not a number above zero) is left out and said in `notes`.
  static bool parse(const std::string & body, Store &, std::string & error, std::vector<std::string> & notes);
};
