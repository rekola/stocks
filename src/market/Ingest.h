#pragma once

#include "market/Dataset.h"
#include "model/Store.h"

#include <string>
#include <vector>

// One ingest run: a dataset's instruments are discovered and each one's bars are loaded and written to the application's own Store, the way a
// source writes them: the dataset, the market, the base (a currency, or an instrument by its ISIN) and the quote are made by the same helpers the
// sources use (source/InstrumentLists.h), the edge gets its listing, and the bars go in with `appendCandles`. What the store does with them (the
// queues of the summaries, a retention for each resolution, what goes to the disk) is the store's; so is what it does not do twice. A pair with no
// data in the current year is left out. Names no provider.
namespace market {

struct IngestOptions {
  std::vector<std::string> codes;                               // the instruments to load; empty: all of them
  std::vector<Resolution> resolutions{kResolutions.begin(), kResolutions.end()};
  bool refresh = false;
};

struct SeriesReport {
  Resolution resolution = Resolution::Day;
  int64_t bars = 0;
  Timestamp firstBar = 0, lastBar = 0;  // the start of the first and of the last bar written (UTC seconds)
};

struct InstrumentReport {
  std::string code;
  bool inactive = false;  // left out: no data this year
  Timestamp lastBarTime = 0;
  std::vector<SeriesReport> series;
  std::vector<std::string> failed, skipped;
  int downloads = 0, cached = 0;
};

struct IngestReport {
  std::vector<InstrumentReport> instruments;
  std::vector<std::string> unknownCodes;  // asked for and not in the dataset
  int inactive = 0;                       // instruments left out for having no data this year
  // Whether everything asked for was done: no code unknown, no period that failed, no resolution skipped.
  bool complete() const;
};

// Writes to the store and waits (`flush()`) until the disk has it.
IngestReport runIngest(Dataset &, Store &, const IngestOptions &, const Log & log);

// The report as text: a line for each instrument and resolution (bars, first and last bar time), then what failed.
std::string formatReport(const IngestReport &);
// 2026-11-13T21:58:00Z
std::string formatTime(Timestamp utcSeconds);

}  // namespace market
