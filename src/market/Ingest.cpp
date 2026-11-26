#include "market/Ingest.h"

#include "model/Dates.h"
#include "model/NodeKey.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

namespace market {

namespace {

std::string lower(std::string s) {
  for (char & c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

bool IngestReport::complete() const {
  if (!unknownCodes.empty()) return false;
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

IngestReport runIngest(Dataset & dataset, MarketStore & store, const IngestOptions & options, const Log & log) {
  IngestReport report;
  const std::vector<InstrumentSpec> found = dataset.discover(log, options.refresh);

  std::vector<InstrumentSpec> chosen;
  if (options.codes.empty()) {
    chosen = found;
  } else {
    std::set<std::string> done;
    for (const std::string & asked : options.codes) {
      const std::string code = lower(asked);
      if (!done.insert(code).second) continue;
      const auto it = std::find_if(found.begin(), found.end(), [&](const InstrumentSpec & s) { return s.code == code; });
      if (it == found.end()) report.unknownCodes.push_back(asked);
      else chosen.push_back(*it);
    }
  }
  for (const std::string & code : report.unknownCodes) log(dataset.code() + ": no instrument " + code);

  const uint64_t datasetId = store.putDataset(dataset.code(), dataset.name());
  std::string note = dataset.code() + ":";
  for (Resolution level : options.levels) note += std::string(" ") + name(level);
  if (options.refresh) note += " refresh";
  report.version = store.beginBatch(datasetId, note);

  size_t index = 0;
  for (const InstrumentSpec & spec : chosen) {
    index++;
    InstrumentReport out;
    out.code = spec.code;
    const auto base = currencyKey(spec.base), quote = currencyKey(spec.quote), market = marketKey(spec.market);
    if (!base || !quote || !market) {
      out.failed.push_back("the keys of " + spec.base + "/" + spec.quote + " on " + spec.market + " cannot be made");
      log(dataset.code() + ": " + spec.code + ": " + out.failed.back());
      report.instruments.push_back(std::move(out));
      continue;
    }
    const Edge edge{base->value, quote->value, market->value, datasetId};
    store.putInstrument({edge, spec.code, spec.name, spec.assetClass, spec.firstPeriod});

    log("[" + std::to_string(index) + "/" + std::to_string(chosen.size()) + "] " + spec.code + " (" + spec.name + ")");
    LoadResult loaded = dataset.load(spec, {options.levels, options.refresh}, log);
    out.noData = loaded.noData;
    out.lastBarTime = loaded.lastBarTime;
    out.failed = loaded.failed;
    out.skipped = loaded.skipped;
    out.downloads = loaded.downloads;
    out.cached = loaded.cached;
    for (const LevelBars & built : loaded.levels) {
      store.replaceBars(edge, built.level, built.bars, loaded.lastBarTime, report.version);
      LevelReport level;
      level.level = built.level;
      level.bars = static_cast<int64_t>(built.bars.size());
      if (!built.bars.empty()) level.firstBar = barStart(built.level, built.bars.front().key), level.lastBar = barStart(built.level, built.bars.back().key);
      out.levels.push_back(level);
    }
    if (loaded.noData) log(dataset.code() + ": " + spec.code + ": no data at all");
    report.instruments.push_back(std::move(out));
  }
  return report;
}

std::string formatReport(const IngestReport & report) {
  std::string text;
  char line[160];
  std::snprintf(line, sizeof line, "%-8s %-5s %7s  %-20s  %-20s\n", "code", "level", "bars", "first bar", "last bar");
  text += line;
  for (const InstrumentReport & r : report.instruments) {
    if (r.levels.empty()) {
      std::snprintf(line, sizeof line, "%-8s %-5s %7s  %s\n", r.code.c_str(), "-", "-", r.noData ? "(no data)" : "(nothing stored)");
      text += line;
    }
    for (const LevelReport & l : r.levels) {
      std::snprintf(line, sizeof line, "%-8s %-5s %7lld  %-20s  %-20s\n", r.code.c_str(), name(l.level), static_cast<long long>(l.bars),
                    l.bars ? formatTime(l.firstBar).c_str() : "-", l.bars ? formatTime(l.lastBar).c_str() : "-");
      text += line;
    }
  }
  int downloads = 0, cached = 0;
  for (const InstrumentReport & r : report.instruments) downloads += r.downloads, cached += r.cached;
  text += "\nfiles: " + std::to_string(downloads) + " downloaded, " + std::to_string(cached) + " from the cache; batch " + std::to_string(report.version) + "\n";
  bool any = false;
  for (const InstrumentReport & r : report.instruments)
    for (const std::string & f : r.failed) {
      if (!any) text += "\nfailed periods:\n", any = true;
      text += "  " + r.code + " " + f + "\n";
    }
  for (const InstrumentReport & r : report.instruments)
    for (const std::string & s : r.skipped) {
      if (!any) text += "\nfailed periods:\n", any = true;
      text += "  " + r.code + " level left as it was, " + s + "\n";
    }
  for (const std::string & c : report.unknownCodes) text += "unknown instrument: " + c + "\n";
  if (!any && report.unknownCodes.empty()) text += "no failed periods\n";
  return text;
}

}  // namespace market
