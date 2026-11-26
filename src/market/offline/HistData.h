#pragma once

#include "market/Dataset.h"
#include "market/RawCache.h"
#include "source/Http.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// HistData.com's free 1-minute bid quotes (ASCII), as a market-data dataset: its instruments, its files and how to read them. Everything
// that names HistData's endpoints, headers and formats is in this adapter. It is an offline dataset, loaded by market-ingest on request,
// and so is not with the sources of src/source/, which run all the time beside the browser.
//
// No API and no login; one zip per request:
//   1. GET the page of the file (`pageUrl`) and read the hidden <input id="tk">. A period with no data may still give a token (the
//      current month before its file exists does), so the verdict is the next step's.
//   2. POST https://www.histdata.com/get.php (form: tk, date, datemonth, platform, timeframe, fxpair) with the page as Referer and
//      Origin set. The body is a zip; an empty body means there is no data for the period.
// A full past year is one file (period YYYY); the current year is one file per month (YYYYMM). The zip holds
// DAT_ASCII_{CODE}_M1_{period}.csv (and a .txt gap report, ignored): no header, `20120201 000100;o;h;l;c;0`, semicolon separated, a
// row for each minute that had quotes. Prices are bid quotes, the volume is always 0 (ignored), the time is fixed EST (UTC-5, no
// daylight saving), so UTC is 5 hours later all year round.
namespace histdata {

// ---- the format ----
// The UTC seconds of a time written in HistData's fixed EST.
Timestamp utcOfEst(int year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second);
// One row of the CSV; none for a line that is not one.
std::optional<market::MinuteBar> parseRow(std::string_view line);
struct CsvResult {
  std::vector<market::MinuteBar> minutes;  // in the file's order
  size_t badLines = 0;                     // lines that were not rows (an empty one is not counted)
};
CsvResult parseCsv(std::string_view csv);

// ---- periods and files ----
struct Period {
  int year = 0;
  int month = 0;  // 0: the whole year
  bool yearly() const { return month == 0; }
  friend bool operator==(const Period &, const Period &) = default;
  friend auto operator<=>(const Period &, const Period &) = default;
};
std::string periodText(Period);                                                   // "2026", "202611"
std::string fileName(const std::string & code, Period);                           // "DAT_ASCII_EURUSD_M1_202611.zip"
std::string pageUrl(const std::string & code, Period);                            // the page that holds the token (month not zero-padded)
std::string formBody(const std::string & token, const std::string & code, Period);  // what is POSTed
// The token on a file's page, empty where there is none.
std::string extractToken(const std::string & html);
// The files that cover the UTC instants from `first` to `last`: the months of the current year one by one, a past year as one
// file, in order. Nothing before `firstPeriod` (YYYYMM, 0 for no limit).
std::vector<Period> periodsCovering(Timestamp first, Timestamp last, int currentYear, int firstPeriod);

// ---- the instruments ----
// The list page is kept in the cache (histdata_pairs.html) and read from there until a run asks to refresh.
// The pairs on HistData's list page, with the month each begins; empty where the page is not that.
std::vector<market::InstrumentSpec> parseDiscovery(const std::string & html);
// The list to fall back on (the one the adapter was written from).
const std::vector<market::InstrumentSpec> & fallbackInstruments();

struct Settings {
  std::chrono::milliseconds pause{2000};  // between two requests
  int retries = 2;                        // more tries of a request that failed
  std::function<Timestamp()> now;         // UTC seconds; the system clock where empty
  std::function<void(std::chrono::milliseconds)> sleep;  // the thread's sleep where empty
};

class HistData : public market::Dataset {
 public:
  HistData(Http & http, market::RawCache cache, Settings settings = {});
  std::string code() const override { return "HISTDATA"; }
  std::string name() const override { return "HistData.com"; }
  std::vector<market::InstrumentSpec> discover(const market::Log & log, bool refresh) override;
  market::LoadResult load(const market::InstrumentSpec & instrument, const market::LoadOptions & options, const market::Log & log) override;

 private:
  enum class Fetch { Ok, NoData, Failed };
  struct Got {
    Fetch status = Fetch::Failed;
    std::string bytes, why;
  };
  void reportDifferences(const std::vector<market::InstrumentSpec> & found, const market::Log & log);
  void pace();
  // One file from the site, with the retries; never from the cache.
  Got download(const std::string & code, Period, const market::Log & log);
  // One file, from the cache if it is there (unless `refresh` asks again, which only the months of the current year are asked for),
  // else from the site, which then puts it in the cache.
  Got obtain(const std::string & code, Period, bool refresh, int currentYear, market::LoadResult &, const market::Log & log);
  Http & http_;
  market::RawCache cache_;
  Settings settings_;
  std::chrono::steady_clock::time_point lastRequest_{};
  bool requested_ = false;
};

}  // namespace histdata
