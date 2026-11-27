#include "market/Ingest.h"

#include "model/Dates.h"
#include "source/InstrumentLists.h"
#include "source/KnownMarkets.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_set>

namespace market {

namespace {

std::string lower(std::string s) {
  for (char & c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The edge of an instrument, the nodes made as the sources make them. None where a node cannot be had (a refused ISIN).
std::optional<EdgeKey> makeEdge(Batch & b, const Dataset & dataset, const InstrumentSpec & spec, std::string & why) {
  const DatasetId ds = b.addDataset(dataset.code(), dataset.name());
  const MarketId market = b.addMarket(spec.market, marketNameOfMic(spec.market));
  const InstrumentId base = spec.baseKind == InstrumentKind::Currency
                                ? currencyNode(b, spec.baseSymbol)
                                : addInstrumentUnlessSaid(b, spec.baseSymbol, spec.baseName, spec.baseKind, spec.baseIsin, spec.type);
  const InstrumentId quote = quoteNode(b, spec.quote);
  if (!ds.valid() || !market.valid() || !base.valid() || !quote.valid()) {
    why = "a node of " + spec.symbol + " could not be made (the dataset " + dataset.code() + ", the market " + spec.market + ", " + spec.baseSymbol + ", " +
          spec.quote + ")";
    return std::nullopt;
  }
  return EdgeKey{base, quote, market, ds};
}

}  // namespace

bool IngestReport::complete() const {
  if (!unknownCodes.empty() || failedWrites > 0) return false;
  if (!instruments.empty() && static_cast<size_t>(inactive) == instruments.size()) return false;
  for (const InstrumentReport & r : instruments)
    if (!r.failed.empty() || !r.skipped.empty()) return false;
  return true;
}

std::string formatTime(Timestamp t) {
  const int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
  const Date d = civilFromUnixDays(days);
  const int64_t rest = t - days * 86400;
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%04d-%02u-%02uT%02d:%02d:%02dZ", d.year, d.month, d.day, static_cast<int>(rest / 3600),
                static_cast<int>(rest % 3600 / 60), static_cast<int>(rest % 60));
  return buffer;
}

IngestReport runIngest(Dataset & dataset, Store & store, const IngestOptions & options, const Log & log) {
  IngestReport report;
  const std::vector<InstrumentSpec> found = dataset.discover(log, options.refresh);

  std::vector<InstrumentSpec> chosen;
  if (options.codes.empty()) {
    chosen = found;
  } else {
    std::unordered_set<std::string> done;
    for (const std::string & asked : options.codes) {
      const std::string code = lower(asked);
      if (!done.insert(code).second) continue;
      const auto it = std::find_if(found.begin(), found.end(), [&](const InstrumentSpec & s) { return s.code == code; });
      if (it == found.end()) report.unknownCodes.push_back(asked);
      else chosen.push_back(*it);
    }
  }
  for (const std::string & code : report.unknownCodes) log(dataset.code() + ": no instrument " + code);

  const uint64_t failedBefore = store.failedWrites();
  size_t index = 0;
  for (const InstrumentSpec & spec : chosen) {
    index++;
    InstrumentReport out;
    out.code = spec.code;
    log("[" + std::to_string(index) + "/" + std::to_string(chosen.size()) + "] " + spec.code + " (" + spec.name + ")");
    LoadResult loaded = dataset.load(spec, {options.resolutions, options.refresh}, log);
    out.lastBarTime = loaded.lastBarTime;
    out.failed = loaded.failed;
    out.skipped = loaded.skipped;
    out.downloads = loaded.downloads;
    out.cached = loaded.cached;
    out.inactive = loaded.inactive;
    if (loaded.inactive) {
      // Not carried: nothing of it is written (what an earlier run wrote stays as it was, and is never said again).
      report.inactive++;
      log(dataset.code() + ": " + spec.code + ": no data in " + std::to_string(loaded.activeFromYear) + " or since, left out");
      report.instruments.push_back(std::move(out));
      continue;
    }
    std::string why;
    try {
      store.batch([&](Batch & b) {
        const auto edge = makeEdge(b, dataset, spec, why);
        if (!edge) return;
        b.setListing({*edge, spec.symbol, "", "", spec.type, spec.name});
        for (const Series & s : loaded.series) b.appendCandles(*edge, s.resolution, s.bars);
        if (loaded.quote) b.setQuote(*edge, *loaded.quote);
      });
    } catch (const std::exception & e) {
      why = e.what();
    }
    if (!why.empty()) {
      out.failed.push_back(why);
      log(dataset.code() + ": " + spec.code + ": " + why);
    } else {
      for (const Series & s : loaded.series) {
        SeriesReport r;
        r.resolution = s.resolution;
        r.bars = static_cast<int64_t>(s.bars.size());
        if (!s.bars.empty()) r.firstBar = barStart(s.resolution, s.bars.front().time), r.lastBar = barStart(s.resolution, s.bars.back().time);
        out.series.push_back(r);
      }
    }
    report.instruments.push_back(std::move(out));
  }
  store.flush();
  report.failedWrites = store.failedWrites() - failedBefore;
  if (report.failedWrites > 0) log(dataset.code() + ": " + std::to_string(report.failedWrites) + " writes to the database failed");
  return report;
}

std::string formatReport(const IngestReport & report) {
  std::string text;
  char line[160];
  std::snprintf(line, sizeof line, "%-8s %-5s %7s  %-20s  %-20s\n", "code", "bars", "count", "first bar", "last bar");
  text += line;
  for (const InstrumentReport & r : report.instruments) {
    if (r.series.empty()) {
      std::snprintf(line, sizeof line, "%-8s %-5s %7s  %s\n", r.code.c_str(), "-", "-", r.inactive ? "(left out: no data this year)" : "(nothing written)");
      text += line;
    }
    for (const SeriesReport & s : r.series) {
      std::snprintf(line, sizeof line, "%-8s %-5s %7lld  %-20s  %-20s\n", r.code.c_str(), name(s.resolution), static_cast<long long>(s.bars),
                    s.bars ? formatTime(s.firstBar).c_str() : "-", s.bars ? formatTime(s.lastBar).c_str() : "-");
      text += line;
    }
  }
  int downloads = 0, cached = 0;
  for (const InstrumentReport & r : report.instruments) downloads += r.downloads, cached += r.cached;
  text += "\nfiles: " + std::to_string(downloads) + " downloaded, " + std::to_string(cached) + " from the cache\n";
  bool any = false;
  for (const InstrumentReport & r : report.instruments)
    for (const std::string & f : r.failed) {
      if (!any) text += "\nfailed periods:\n", any = true;
      text += "  " + r.code + " " + f + "\n";
    }
  for (const InstrumentReport & r : report.instruments)
    for (const std::string & s : r.skipped) {
      if (!any) text += "\nfailed periods:\n", any = true;
      text += "  " + r.code + " left as it was, " + s + "\n";
    }
  for (const std::string & c : report.unknownCodes) text += "unknown instrument: " + c + "\n";
  if (report.inactive) text += std::to_string(report.inactive) + " instruments left out for having no data this year\n";
  if (!report.instruments.empty() && static_cast<size_t>(report.inactive) == report.instruments.size()) text += "every instrument came out inactive: the site may not be answering\n";
  if (report.failedWrites) text += std::to_string(report.failedWrites) + " writes to the database failed: what was loaded is not all kept\n";
  if (!any && report.unknownCodes.empty() && !report.failedWrites) text += "no failed periods\n";
  return text;
}

}  // namespace market
