#include "market/offline/Lme.h"

#include "market/Bars.h"
#include "model/Dates.h"
#include "source/KnownIsins.h"
#include "source/KnownMarkets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <thread>

using json = nlohmann::json;

namespace lme {

namespace {

constexpr const char * kEndpoint = "https://karasmussen.fi/wp-json/price-history/v1/prices";
constexpr const char * kPage = "https://karasmussen.fi/paivan-hinnat/";  // the page whose script asks, which is the Referer

struct Metal {
  const char * code;  // the answer's
  const char * symbol, *name, *isin;
  const char * market;  // the MIC: the London Bullion Market for the four it fixes; no market for rhodium, which neither it nor the LME prices
  int firstPeriod;      // the month of the first price, YYYYMM
};
constexpr Metal kMetals[] = {{"AU", "XAU", "Gold", known_isin::kGold, "XLBM", 201911},
                             {"AG", "XAG", "Silver", known_isin::kSilver, "XLBM", 201911},
                             {"PT", "XPT", "Platinum", known_isin::kPlatinum, "XLBM", 201911},
                             {"PD", "XPD", "Palladium", known_isin::kPalladium, "XLBM", 201911},
                             {"RH", "XRH", "Rhodium", known_isin::kRhodium, kNoMarket, 202102}};
constexpr const char * kCurrencies[] = {"USD", "EUR", "NOK", "SEK", "DKK"};
constexpr int kFirstYear = 2019;  // the first year of any metal's prices

std::string lower(std::string s) {
  for (char & c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const Metal * metalOfCode(const std::string & code) {
  for (const Metal & m : kMetals)
    if (code == m.code) return &m;
  return nullptr;
}

const Metal * metalOfSymbol(const std::string & symbol) {
  for (const Metal & m : kMetals)
    if (symbol == m.symbol) return &m;
  return nullptr;
}

bool carried(const std::string & currency) {
  return std::find(std::begin(kCurrencies), std::end(kCurrencies), currency) != std::end(kCurrencies);
}

std::optional<double> number(const json & row, const char * key) {
  const auto it = row.find(key);
  if (it == row.end() || !it->is_number()) return std::nullopt;
  const double v = it->get<double>();
  return std::isfinite(v) && v > 0 ? std::optional<double>(v) : std::nullopt;
}

}  // namespace

bool parse(const std::string & body, Parsed & out, std::string & error) {
  out = {};
  const json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_array()) {
    error = "not a list of metals";
    return false;
  }
  bool any = false;
  for (const json & metal : doc) {
    if (!metal.is_object() || !metal.contains("Prices") || !metal["Prices"].is_array() || !metal.contains("Code") || !metal["Code"].is_string()) continue;
    any = true;
    const std::string code = metal["Code"].get<std::string>();
    if (!metalOfCode(code)) continue;
    for (const json & price : metal["Prices"]) {
      if (!price.is_object()) {
        out.skipped++;
        continue;
      }
      const auto currency = price.find("Currency");
      if (currency == price.end() || !currency->is_string()) {
        out.skipped++;
        continue;
      }
      if (!carried(currency->get<std::string>())) continue;
      // "2026-11-26T15:00:00": UTC, though it says no zone.
      const auto time = price.contains("TradeTime") && price["TradeTime"].is_string() ? instantOfIsoTimestamp(price["TradeTime"].get<std::string>() + "Z") : std::nullopt;
      const auto bid = number(price, "BidGram"), ask = number(price, "AskGram");
      if (!time || !bid || !ask || *bid > *ask) {
        out.skipped++;
        continue;
      }
      out.rows.push_back({code, currency->get<std::string>(), *time, *bid * kGramsPerTroyOunce, *ask * kGramsPerTroyOunce});
    }
  }
  if (!any) {
    error = "no metals in the answer";
    return false;
  }
  return true;
}

std::string url(int year) {
  const std::string y = std::to_string(year);
  return std::string(kEndpoint) + "?from=" + y + "-01-01&to=" + y + "-12-31";
}

std::string fileName(int year) { return "lme_" + std::to_string(year) + ".json"; }

const std::vector<market::InstrumentSpec> & instruments() {
  static const std::vector<market::InstrumentSpec> list = [] {
    std::vector<market::InstrumentSpec> all;
    for (const Metal & m : kMetals)
      for (const char * currency : kCurrencies) {
        market::InstrumentSpec s;
        s.code = lower(std::string(m.symbol) + currency);
        s.symbol = m.symbol;
        s.name = std::string(m.name) + ", per troy oz";
        s.type = "METAL";
        s.baseSymbol = m.symbol;
        s.baseName = m.name;
        s.baseIsin = m.isin;
        s.baseKind = InstrumentKind::Commodity;
        s.quote = currency;
        s.market = m.market;
        s.firstPeriod = m.firstPeriod;
        all.push_back(std::move(s));
      }
    return all;
  }();
  return list;
}

Lme::Lme(Http & http, market::RawCache cache, Settings settings) : http_(http), cache_(std::move(cache)), settings_(std::move(settings)) {
  if (!settings_.now) settings_.now = [] { return static_cast<Timestamp>(std::time(nullptr)); };
  if (!settings_.sleep) settings_.sleep = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
}

std::vector<market::InstrumentSpec> Lme::discover(const market::Log &, bool) { return instruments(); }

void Lme::pace() {
  const auto now = std::chrono::steady_clock::now();
  if (requested_ && now - lastRequest_ < settings_.pause) settings_.sleep(std::chrono::duration_cast<std::chrono::milliseconds>(settings_.pause - (now - lastRequest_)));
  requested_ = true;
  lastRequest_ = std::chrono::steady_clock::now();
}

Timestamp Lme::fetchedAt(const std::string & file) const {
  const auto note = cache_.read(file + ".fetched");
  if (!note) return 0;
  char * end = nullptr;
  const long long value = std::strtoll(note->c_str(), &end, 10);
  return end != note->c_str() ? static_cast<Timestamp>(value) : 0;
}

void Lme::noteFetched(const std::string & file) {
  std::string error;
  cache_.store(file + ".fetched", std::to_string(settings_.now()) + "\n", error);  // a note that cannot be kept only makes the file asked for again
}

const Lme::Year & Lme::year(int y, bool refresh, int currentYear, market::LoadResult & result, const market::Log & log) {
  if (const auto it = years_.find(y); it != years_.end()) return it->second;
  Year & out = years_[y];
  const std::string file = fileName(y);
  const auto read = [&](const std::string & body) {
    Parsed parsed;
    if (!parse(body, parsed, out.why)) return false;
    if (parsed.skipped > 0) log("lme: " + file + ": " + std::to_string(parsed.skipped) + " rows are not prices and were left out");
    out.rows = std::move(parsed.rows);
    out.ok = true;
    return true;
  };

  // A past year is final once it is past: a file fetched before the year was over is asked for again, and the current year when `refresh` says.
  const Timestamp yearEnd = static_cast<Timestamp>((dayNumber(y + 1, 1, 1) - kUnixEpochDayNumber) * 86400);
  const bool cached = cache_.has(file);
  const bool unfinished = cached && y < currentYear && fetchedAt(file) < yearEnd;
  if (unfinished) log("lme: " + file + ": it was fetched before the year was over; asking again");
  const bool again = (refresh && y == currentYear) || unfinished;
  std::optional<std::string> kept = cached ? cache_.read(file) : std::nullopt;
  if (kept && !again) {
    result.cached++;
    if (!read(*kept)) log("lme: " + file + ": " + out.why);
    return out;
  }

  std::string why;
  for (int attempt = 0; attempt <= settings_.retries; attempt++) {
    if (attempt > 0) log("lme: " + file + ": " + why + "; trying again (" + std::to_string(attempt) + " of " + std::to_string(settings_.retries) + ")");
    pace();
    const std::string address = url(y);
    result.downloads++;
    const HttpResult answer = http_.get(address, browserHeaders(address, {{"Referer", kPage}}));
    if (!answer.ok()) {
      why = "the site gave " + (answer.error.empty() ? "status " + std::to_string(answer.status) : answer.error);
      continue;
    }
    if (!read(answer.body)) {
      why = out.why;
      continue;
    }
    std::string error;
    if (!cache_.store(file, answer.body, error)) log("lme: " + file + ": not cached: " + error);  // the data is still used this time
    else noteFetched(file);
    return out;
  }
  out = {};
  // The file we have is the last one that was good; a year that cannot be asked for again is read from it.
  if (kept && read(*kept)) {
    log("lme: " + file + ": " + why + "; using the cached copy");
    result.cached++;
    return out;
  }
  out.why = why;
  return out;
}

market::LoadResult Lme::load(const market::InstrumentSpec & instrument, const market::LoadOptions & options, const market::Log & log) {
  market::LoadResult result;
  const Metal * metal = metalOfSymbol(instrument.baseSymbol);
  if (!metal) {
    result.failed.push_back(instrument.code + ": not a metal this dataset knows");
    return result;
  }
  const Timestamp now = settings_.now();
  const int currentYear = civilFromUnixDays(now / 86400).year;
  const int activeFromYear = civilFromUnixDays((now - 45 * 86400) / 86400).year;  // the last year still counts in the first weeks of one

  std::vector<market::MinuteBar> prices;  // one a day, put through the bars' own aggregation
  std::optional<Row> last;
  bool incomplete = false;
  for (int y = std::max(kFirstYear, instrument.firstPeriod / 100); y <= currentYear; y++) {
    const Year & got = year(y, options.refresh, currentYear, result, log);
    if (!got.ok) {
      result.failed.push_back(std::to_string(y) + ": " + got.why);
      log("lme: " + instrument.code + " " + std::to_string(y) + ": " + got.why);
      incomplete = true;
      continue;
    }
    for (const Row & row : got.rows) {
      if (row.metal != metal->code || row.currency != instrument.quote) continue;
      const double price = row.mid();
      prices.push_back({row.time, price, price, price, price});
      if (!last || row.time > last->time) last = row;
    }
  }
  if (!last) {
    if (!incomplete) {
      result.inactive = true;
      result.activeFromYear = activeFromYear;
    }
    return result;
  }
  if (civilFromUnixDays(last->time / 86400).year < activeFromYear) {
    result.inactive = true;
    result.activeFromYear = activeFromYear;
    return result;
  }
  result.lastBarTime = last->time;

  // The one price a day is a bar of a day and of a week; the finer resolutions have nothing to say. A gap in a year leaves what is stored as it is.
  for (Resolution level : options.resolutions) {
    if (level != Resolution::Day && level != Resolution::Week) continue;
    if (incomplete) {
      result.skipped.push_back(std::string(::name(level)) + ": a year could not be had");
      continue;
    }
    result.series.push_back({level, market::cutToWindow(level, market::aggregate(level, prices), last->time)});
  }

  Quote quote;
  quote.last = last->mid();
  quote.time = last->time;
  if (last->bid != last->ask) quote.bid = last->bid, quote.ask = last->ask;  // two sides only where the answer gives them
  result.quote = quote;
  return result;
}

}  // namespace lme
