#include "market/offline/HistData.h"

#include "market/Zip.h"
#include "model/Dates.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <thread>

namespace histdata {

namespace {

constexpr int64_t kEstOffset = -5 * 3600;  // fixed EST, no daylight saving
const char * const kListUrl = "https://www.histdata.com/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes";
const char * const kListFile = "histdata_pairs.html";  // the list page, in the cache
const char * const kGetUrl = "https://www.histdata.com/get.php";
const char * const kOrigin = "https://www.histdata.com";

std::string upper(std::string s) {
  for (char & c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
std::string lower(std::string s) {
  for (char & c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool digits(std::string_view s, size_t at, size_t n, unsigned & out) {
  if (at + n > s.size()) return false;
  unsigned v = 0;
  for (size_t i = at; i < at + n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + static_cast<unsigned>(s[i] - '0');
  }
  out = v;
  return true;
}

bool number(std::string_view field, double & out) {
  if (field.empty() || field.size() > 31) return false;
  char buffer[32];
  field.copy(buffer, field.size());
  buffer[field.size()] = '\0';
  char * end = nullptr;
  out = std::strtod(buffer, &end);
  return end == buffer + field.size() && std::isfinite(out);
}

// The date a period's data is written in, in EST: where `time` (UTC seconds) is, as a year and month.
Period estMonthOf(Timestamp time) {
  const Date d = dateOfDayNumber(dayNumberOfInstant(time, static_cast<int>(kEstOffset)));
  return {d.year, static_cast<int>(d.month)};
}

// "eurusd:200005 ..." as the list was written from.
std::vector<market::InstrumentSpec> specsOf(const char * list, const char * assetClass) {
  std::vector<market::InstrumentSpec> out;
  std::istringstream in(list);
  std::string item;
  while (in >> item) {
    const size_t colon = item.find(':');
    market::InstrumentSpec spec;
    spec.code = item.substr(0, colon);
    spec.firstPeriod = std::atoi(item.c_str() + colon + 1);
    spec.base = upper(spec.code.substr(0, 3));
    spec.quote = upper(spec.code.substr(3));
    spec.name = spec.base + "/" + spec.quote;
    spec.assetClass = assetClass;
    out.push_back(std::move(spec));
  }
  return out;
}

int monthNumber(const std::string & name) {
  static const char * const months[] = {"january", "february", "march", "april", "may", "june",
                                        "july", "august", "september", "october", "november", "december"};
  const std::string n = lower(name);
  for (int i = 0; i < 12; i++)
    if (n == months[i]) return i + 1;
  return 0;
}

}  // namespace

Timestamp utcOfEst(int year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second) {
  return daysSinceUnixEpoch(year, month, day) * 86400 + hour * 3600 + minute * 60 + second - kEstOffset;
}

std::optional<market::MinuteBar> parseRow(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.remove_suffix(1);
  // 20120201 000100;1.306570;1.306570;1.306470;1.306560;0
  if (line.size() < 17 || line[8] != ' ' || line[15] != ';') return std::nullopt;
  unsigned y, mo, d, h, mi, s;
  if (!digits(line, 0, 4, y) || !digits(line, 4, 2, mo) || !digits(line, 6, 2, d) || !digits(line, 9, 2, h) || !digits(line, 11, 2, mi) ||
      !digits(line, 13, 2, s))
    return std::nullopt;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) return std::nullopt;
  double v[4];
  std::string_view rest = line.substr(16);
  for (int i = 0; i < 4; i++) {
    const size_t semicolon = rest.find(';');
    if (semicolon == std::string_view::npos && i < 3) return std::nullopt;
    if (!number(rest.substr(0, semicolon), v[i])) return std::nullopt;
    if (semicolon == std::string_view::npos) break;
    rest.remove_prefix(semicolon + 1);
  }
  // The volume that may follow is ignored: it is always 0.
  market::MinuteBar bar;
  bar.time = utcOfEst(static_cast<int>(y), mo, d, h, mi, s);
  bar.open = v[0], bar.high = v[1], bar.low = v[2], bar.close = v[3];
  return bar;
}

CsvResult parseCsv(std::string_view csv) {
  CsvResult out;
  size_t at = 0;
  while (at < csv.size()) {
    size_t end = csv.find('\n', at);
    if (end == std::string_view::npos) end = csv.size();
    std::string_view line = csv.substr(at, end - at);
    at = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.empty()) continue;
    if (auto bar = parseRow(line)) out.minutes.push_back(*bar);
    else out.badLines++;
  }
  return out;
}

std::string periodText(Period p) {
  char buffer[16];
  if (p.yearly()) std::snprintf(buffer, sizeof buffer, "%04d", p.year);
  else std::snprintf(buffer, sizeof buffer, "%04d%02d", p.year, p.month);
  return buffer;
}

std::string fileName(const std::string & code, Period p) { return "DAT_ASCII_" + upper(code) + "_M1_" + periodText(p) + ".zip"; }

std::string pageUrl(const std::string & code, Period p) {
  std::string url = std::string(kListUrl) + "/" + lower(code) + "/" + std::to_string(p.year);
  if (!p.yearly()) url += "/" + std::to_string(p.month);  // not zero-padded
  return url;
}

std::string formBody(const std::string & token, const std::string & code, Period p) {
  return "tk=" + token + "&date=" + std::to_string(p.year) + "&datemonth=" + periodText(p) + "&platform=ASCII&timeframe=M1&fxpair=" + upper(code);
}

std::string extractToken(const std::string & html) {
  const size_t id = html.find("id=\"tk\"");
  if (id == std::string::npos) return {};
  const size_t open = html.rfind('<', id);
  const size_t close = html.find('>', id);
  if (open == std::string::npos || close == std::string::npos) return {};
  const std::string tag = html.substr(open, close - open);
  const size_t value = tag.find("value=\"");
  if (value == std::string::npos) return {};
  const size_t from = value + 7, to = tag.find('"', from);
  if (to == std::string::npos) return {};
  std::string token = tag.substr(from, to - from);
  // A token is hexadecimal; anything else would be put in a form unescaped.
  if (!std::all_of(token.begin(), token.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; })) return {};
  return token;
}

std::vector<Period> periodsCovering(Timestamp first, Timestamp last, int currentYear, int firstPeriod) {
  std::vector<Period> out;
  Period at = estMonthOf(first);
  const Period end = estMonthOf(last);
  while (at <= end) {
    Period wanted = at.year == currentYear ? at : Period{at.year, 0};
    const int lastMonthOfIt = wanted.year * 100 + (wanted.yearly() ? 12 : wanted.month);
    if ((firstPeriod == 0 || lastMonthOfIt >= firstPeriod) && (out.empty() || !(out.back() == wanted))) out.push_back(wanted);
    if (++at.month > 12) at.year++, at.month = 1;
  }
  return out;
}

const std::vector<market::InstrumentSpec> & fallbackInstruments() {
  static const std::vector<market::InstrumentSpec> list = [] {
    std::vector<market::InstrumentSpec> all;
    auto add = [&all](std::vector<market::InstrumentSpec> part) { all.insert(all.end(), part.begin(), part.end()); };
    add(specsOf("eurusd:200005 usdchf:200005 usdjpy:200005 gbpusd:200005 usdcad:200006 audusd:200006 eurchf:200203 eurgbp:200203 "
                "eurjpy:200203 gbpjpy:200205 euraud:200208 gbpchf:200208 audjpy:200208 chfjpy:200208 nzdusd:200508 nzdjpy:200609 "
                "eurcad:200703 cadjpy:200703 audcad:200707 gbpaud:200709 audnzd:200709 gbpcad:200709 eurnzd:200803 nzdcad:200803 "
                "audchf:200803 nzdchf:200803 gbpnzd:200803 cadchf:200803 sgdjpy:200808 usdhkd:200808 usdnok:200808 eursek:200808 "
                "usdsek:200808 eurdkk:200808 eurnok:200808 usddkk:200808 usdsgd:200808 usdczk:201008 usdmxn:201011 usdtry:201011 "
                "eurhuf:201011 eurpln:201011 usdhuf:201011 usdpln:201011 usdzar:201011 zarjpy:201011 eurczk:201011 eurtry:201011",
                "currency"));
    add(specsOf("xauusd:200903 xagusd:200905 xauaud:200905 xauchf:200905 xaueur:200905 xaugbp:200905 wtiusd:201011 bcousd:201011", "commodity"));
    add(specsOf("spxusd:201011 nsxusd:201011 udxusd:201011 grxeur:201011 frxeur:201011 etxeur:201011 ukxgbp:201011 jpxjpy:201011 "
                "hkxhkd:201011 auxaud:201011",
                "index"));
    return all;
  }();
  return list;
}

std::vector<market::InstrumentSpec> parseDiscovery(const std::string & html) {
  // <a href="/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/EURUSD" ...><strong>EUR/USD</strong></a><br/>(2000/May)
  std::vector<market::InstrumentSpec> out;
  std::unordered_set<std::string> seen;
  const std::string marker = "1-minute-bar-quotes/";
  for (size_t at = html.find(marker); at != std::string::npos; at = html.find(marker, at + 1)) {
    size_t i = at + marker.size();
    const size_t codeStart = i;
    while (i < html.size() && std::isalnum(static_cast<unsigned char>(html[i]))) i++;
    if (i == codeStart || i >= html.size() || html[i] != '"') continue;  // a link to a pair, not to one of its years
    const std::string code = lower(html.substr(codeStart, i - codeStart));
    const size_t link = html.find("</a>", i);
    if (link == std::string::npos) break;
    size_t paren = html.find('(', link);
    if (paren == std::string::npos || paren - link > 20) continue;
    unsigned year = 0;
    if (!digits(html, paren + 1, 4, year) || html[paren + 5] != '/') continue;
    size_t nameEnd = paren + 6;
    while (nameEnd < html.size() && std::isalpha(static_cast<unsigned char>(html[nameEnd]))) nameEnd++;
    const int month = monthNumber(html.substr(paren + 6, nameEnd - paren - 6));
    if (!month || code.size() != 6 || !seen.insert(code).second) continue;
    market::InstrumentSpec spec;
    spec.code = code;
    spec.base = upper(code.substr(0, 3));
    spec.quote = upper(code.substr(3));
    spec.name = spec.base + "/" + spec.quote;
    spec.firstPeriod = static_cast<int>(year) * 100 + month;
    spec.assetClass = "other";
    for (const auto & known : fallbackInstruments())
      if (known.code == code) spec.assetClass = known.assetClass;
    out.push_back(std::move(spec));
  }
  return out;
}

HistData::HistData(Http & http, market::RawCache cache, Settings settings) : http_(http), cache_(std::move(cache)), settings_(std::move(settings)) {
  if (!settings_.now) settings_.now = [] { return static_cast<Timestamp>(std::time(nullptr)); };
  if (!settings_.sleep) settings_.sleep = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
}

std::vector<market::InstrumentSpec> HistData::discover(const market::Log & log, bool refresh) {
  std::vector<market::InstrumentSpec> found;
  // The page is kept in the cache beside the files, so that a run that has what it needs asks for nothing.
  if (!refresh)
    if (auto saved = cache_.read(kListFile)) found = parseDiscovery(*saved);
  if (!found.empty()) return reportDifferences(found, log), found;
  const HttpResult page = http_.get(kListUrl);
  requested_ = true, lastRequest_ = std::chrono::steady_clock::now();
  if (page.ok()) {
    found = parseDiscovery(page.body);
    std::string error;
    if (!found.empty() && !cache_.store(kListFile, page.body, error)) log("histdata: the list page is not cached: " + error);
  }
  if (found.empty()) {
    log(std::string("histdata: the list of pairs could not be read from ") + kListUrl + (page.ok() ? " (no pairs on the page)" : " (" + (page.error.empty() ? "status " + std::to_string(page.status) : page.error) + ")") +
        "; using the built-in list of " + std::to_string(fallbackInstruments().size()));
    return fallbackInstruments();
  }
  reportDifferences(found, log);
  return found;
}

void HistData::reportDifferences(const std::vector<market::InstrumentSpec> & found, const market::Log & log) {
  // Say how the page differs from the list this was written from.
  std::unordered_map<std::string, int> known, now;
  for (const auto & s : fallbackInstruments()) known[s.code] = s.firstPeriod;
  for (const auto & s : found) now[s.code] = s.firstPeriod;
  int differences = 0;
  std::vector<std::string> codes;
  for (const auto & entry : now) codes.push_back(entry.first);
  std::sort(codes.begin(), codes.end());
  for (const std::string & code : codes) {
    const int first = now[code];
    const auto it = known.find(code);
    if (it == known.end()) log("histdata: " + code + " is on the page and not in the built-in list (first month " + std::to_string(first) + "; asset class other)"), differences++;
    else if (it->second != first) log("histdata: " + code + " begins " + std::to_string(first) + " on the page, " + std::to_string(it->second) + " in the built-in list"), differences++;
  }
  std::vector<std::string> missing;
  for (const auto & entry : known)
    if (!now.count(entry.first)) missing.push_back(entry.first);
  std::sort(missing.begin(), missing.end());
  for (const std::string & code : missing) log("histdata: " + code + " is in the built-in list and not on the page"), differences++;
  log("histdata: " + std::to_string(found.size()) + " pairs on the page, " + std::to_string(differences) + " difference" + (differences == 1 ? "" : "s") + " from the built-in list");
}

void HistData::pace() {
  const auto now = std::chrono::steady_clock::now();
  if (requested_ && now - lastRequest_ < settings_.pause)
    settings_.sleep(std::chrono::duration_cast<std::chrono::milliseconds>(settings_.pause - (now - lastRequest_)));
  requested_ = true;
  lastRequest_ = std::chrono::steady_clock::now();
}

HistData::Got HistData::download(const std::string & code, Period period, const market::Log & log) {
  Got got;
  const std::string url = pageUrl(code, period);
  for (int attempt = 0; attempt <= settings_.retries; attempt++) {
    if (attempt > 0) log("histdata: " + fileName(code, period) + ": " + got.why + "; trying again (" + std::to_string(attempt) + " of " + std::to_string(settings_.retries) + ")");
    pace();
    const HttpResult page = http_.get(url);
    if (!page.ok()) {
      got.why = "the page gave " + (page.error.empty() ? "status " + std::to_string(page.status) : page.error);
      continue;
    }
    const std::string token = extractToken(page.body);
    if (token.empty()) return {Fetch::NoData, {}, "no download token"};
    pace();
    const HttpResult answer = http_.post(kGetUrl, formBody(token, code, period), {{"Referer", url}, {"Origin", kOrigin}});
    if (!answer.ok()) {
      got.why = "the download gave " + (answer.error.empty() ? "status " + std::to_string(answer.status) : answer.error);
      continue;
    }
    if (answer.body.empty()) return {Fetch::NoData, {}, "empty answer"};
    if (answer.body.size() < 4 || answer.body.compare(0, 2, "PK") != 0) {
      got.why = "the answer is not a zip file";
      continue;
    }
    return {Fetch::Ok, answer.body, {}};
  }
  got.status = Fetch::Failed;
  return got;
}

namespace {

// The size of the CSV in a zip, 0 where it is not a zip with one.
uint64_t csvSizeOf(const std::string & zip) {
  std::vector<market::ZipEntry> entries;
  std::string error;
  if (!market::listZip(zip, entries, error)) return 0;
  const market::ZipEntry * csv = market::findZipEntry(entries, ".csv");
  return csv ? csv->size : 0;
}

}  // namespace

HistData::Got HistData::obtain(const std::string & code, Period period, bool refresh, int currentYear, market::LoadResult & result,
                               const market::Log & log) {
  const std::string name = fileName(code, period);
  const bool again = refresh && !period.yearly() && period.year == currentYear;  // only a month still being filled grows
  if (cache_.has(name) && !again) {
    if (auto bytes = cache_.read(name)) {
      result.cached++;
      return {Fetch::Ok, std::move(*bytes), {}};
    }
    return {Fetch::Failed, {}, "cannot read " + cache_.pathOf(name).string()};
  }
  result.downloads++;
  Got got = download(code, period, log);
  if (got.status != Fetch::Ok) {
    if (again && cache_.has(name)) {
      // The file we have is the last one that was good; a month that cannot be asked for again is read from it.
      if (auto bytes = cache_.read(name)) {
        log("histdata: " + name + ": " + (got.status == Fetch::NoData ? "the site has none now" : got.why) + "; using the cached copy");
        result.cached++;
        return {Fetch::Ok, std::move(*bytes), {}};
      }
    }
    return got;
  }
  if (csvSizeOf(got.bytes) == 0) return {Fetch::Failed, {}, "the zip has no CSV"};
  // A month's file only grows: a shorter one than what is in the cache is not put over it.
  if (again && cache_.has(name)) {
    const auto old = cache_.read(name);
    if (old && csvSizeOf(*old) > csvSizeOf(got.bytes)) {
      log("histdata: " + name + ": the new file is shorter than the cached one; keeping the cached one");
      result.cached++;
      return {Fetch::Ok, *old, {}};
    }
  }
  std::string error;
  if (!cache_.store(name, got.bytes, error)) {
    log("histdata: " + name + ": not cached: " + error);  // the data is still used this time
  }
  return got;
}

market::LoadResult HistData::load(const market::InstrumentSpec & instrument, const market::LoadOptions & options, const market::Log & log) {
  using market::Bar;
  using market::MinuteBar;
  market::LoadResult result;
  const std::string code = lower(instrument.code);

  // The minutes of a file: from its zip, or the reason it cannot be had.
  auto minutesOf = [&](const Got & got, Period period, std::vector<MinuteBar> & minutes, std::string & why) {
    std::vector<market::ZipEntry> entries;
    if (!market::listZip(got.bytes, entries, why)) return false;
    const market::ZipEntry * csv = market::findZipEntry(entries, ".csv");
    if (!csv) {
      why = "the zip has no CSV";
      return false;
    }
    std::string text;
    if (!market::readZipEntry(got.bytes, *csv, text, why)) return false;
    CsvResult parsed = parseCsv(text);
    if (parsed.badLines > 0) log("histdata: " + fileName(code, period) + ": " + std::to_string(parsed.badLines) + " lines are not rows and were left out");
    minutes = std::move(parsed.minutes);
    return true;
  };

  const Timestamp now = settings_.now();
  const Period current = estMonthOf(now);
  const int currentYear = current.year;
  // A pair is carried while it has data this year. The data lags by weeks, so in the first weeks of a year the last year still counts.
  const int activeFromYear = estMonthOf(now - 45 * 86400).year;

  // 1. The latest file there is: the current month, back through the year, then (early in a year) the year before. A period the site
  // has nothing for (no token or an empty answer) is walked past; a file that could not be had at all stops it, or the data would
  // end at an older file than the latest and look current. Nothing in the years that count: the pair is inactive, and no file of an
  // earlier year is asked for.
  std::vector<Period> candidates;
  for (int m = current.month; m >= 1; m--) candidates.push_back({currentYear, m});
  const int firstYear = instrument.firstPeriod ? instrument.firstPeriod / 100 : activeFromYear;
  for (int y = currentYear - 1; y >= std::max(activeFromYear, firstYear); y--) candidates.push_back({y, 0});

  std::optional<Period> latest;
  std::vector<MinuteBar> latestMinutes;
  for (const Period & period : candidates) {
    if (instrument.firstPeriod && period.year * 100 + (period.yearly() ? 12 : period.month) < instrument.firstPeriod) break;
    const Got got = obtain(code, period, options.refresh, currentYear, result, log);
    if (got.status == Fetch::NoData) continue;
    if (got.status == Fetch::Failed) {
      result.failed.push_back(periodText(period) + ": " + got.why);
      log("histdata: " + code + " " + periodText(period) + ": " + got.why + "; given up");
      for (market::Level level : options.levels) result.skipped.push_back(std::string(market::name(level)) + ": the latest file could not be had");
      return result;
    }
    std::string why;
    if (!minutesOf(got, period, latestMinutes, why)) {
      result.failed.push_back(periodText(period) + ": " + why);
      log("histdata: " + code + " " + periodText(period) + ": " + why);
      for (market::Level level : options.levels) result.skipped.push_back(std::string(market::name(level)) + ": the latest file could not be read");
      return result;
    }
    if (latestMinutes.empty()) continue;  // a file with no rows has no last bar
    latest = period;
    break;
  }
  if (!latest) {
    result.inactive = true;
    result.activeFromYear = activeFromYear;
    return result;
  }
  Timestamp lastBarTime = 0;
  for (const MinuteBar & m : latestMinutes) lastBarTime = std::max(lastBarTime, m.time);
  result.lastBarTime = lastBarTime;

  // 2. The files each level needs, the window of the level back from the last bar.
  std::unordered_map<market::Level, std::vector<Period>> needs;
  std::vector<Period> all{*latest};  // already in hand
  for (market::Level level : options.levels) {
    needs[level] = periodsCovering(market::windowStartTime(level, lastBarTime), lastBarTime, currentYear, instrument.firstPeriod);
    all.insert(all.end(), needs[level].begin(), needs[level].end());
  }
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());

  // 3. Each file once, oldest first, into every level that needs it.
  std::unordered_map<market::Level, std::vector<Bar>> built;
  std::unordered_map<market::Level, std::string> incomplete;
  for (const Period & period : all) {
    std::vector<market::Level> users;
    for (market::Level level : options.levels)
      if (std::find(needs[level].begin(), needs[level].end(), period) != needs[level].end()) users.push_back(level);
    if (users.empty()) continue;
    std::vector<MinuteBar> minutes;
    if (period == *latest) {
      minutes = latestMinutes;
    } else {
      const Got got = obtain(code, period, options.refresh, currentYear, result, log);
      if (got.status == Fetch::NoData) {
        // Nothing for the period (before the instrument's data, or a gap): not a failure.
        log("histdata: " + code + " " + periodText(period) + ": no data");
        continue;
      }
      std::string why = got.why;
      if (got.status != Fetch::Ok || !minutesOf(got, period, minutes, why)) {
        result.failed.push_back(periodText(period) + ": " + why);
        log("histdata: " + code + " " + periodText(period) + ": " + why + "; moving on");
        for (market::Level level : users) incomplete[level] = periodText(period) + " could not be had";
        continue;
      }
    }
    for (market::Level level : users) built[level] = market::joinBars(std::move(built[level]), market::aggregate(market::barOf(level), minutes));
  }

  // 4. Only a level built from every file it needs replaces what is stored: a gap in the middle of ten years would otherwise
  // wipe good bars.
  for (market::Level level : options.levels) {
    if (incomplete.count(level)) {
      result.skipped.push_back(std::string(market::name(level)) + ": " + incomplete[level]);
      continue;
    }
    result.levels.push_back({level, market::cutToWindow(level, std::move(built[level]), lastBarTime)});
  }
  return result;
}

}  // namespace histdata
