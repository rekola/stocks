#pragma once

#include "market/Dataset.h"
#include "market/Store.h"

#include <string>
#include <vector>

// One ingest run: a dataset's instruments are discovered, the missing instrument records are made, each instrument's levels are
// loaded and each level replaces what the store had of it. Names no provider.
namespace market {

struct IngestOptions {
  std::vector<std::string> codes;                       // the instruments to load; empty: all of them
  std::vector<Resolution> levels{kLevels.begin(), kLevels.end()};
  bool refresh = false;
};

struct LevelReport {
  Resolution level = Resolution::Day;
  int64_t bars = 0;
  Timestamp firstBar = 0, lastBar = 0;  // the start of the first and of the last bar stored (UTC seconds)
};

struct InstrumentReport {
  std::string code;
  bool noData = false;
  Timestamp lastBarTime = 0;
  std::vector<LevelReport> levels;
  std::vector<std::string> failed, skipped;
  int downloads = 0, cached = 0;
};

struct IngestReport {
  uint64_t version = 0;
  std::vector<InstrumentReport> instruments;
  std::vector<std::string> unknownCodes;  // asked for and not in the dataset
  // Whether everything asked for was done: no code unknown, no period that failed, no level skipped.
  bool complete() const;
};

IngestReport runIngest(Dataset &, MarketStore &, const IngestOptions &, const Log & log);

// The report as text: a line for each instrument and level (bars, first and last bar time), then what failed.
std::string formatReport(const IngestReport &);
// 2026-11-13T21:58:00Z
std::string formatTime(Timestamp utcSeconds);

}  // namespace market
