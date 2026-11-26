#include "source/SifterSource.h"

#include "model/Dates.h"
#include "source/FundRows.h"

#include <nlohmann/json.hpp>

#include <algorithm>

using nlohmann::json;

namespace {

constexpr const char * kUrl = "https://ir.altusinvestor.com/clients/am/sifter/chart-html5-new/sifter/dataservice.php?id=SIFFDGLLXI";

// A share class, and the member of a row that has its NAV.
struct ShareClass {
  const char * key;
  const char * isin;
  const char * name;
  const char * symbol;
};
constexpr ShareClass kClasses[] = {
    {"cr", "LU0168736675", "Sifter Fund Global PA", "SIFTERPA"},
    {"c", "LU0168577939", "Sifter Fund Global PB", "SIFTERPB"},
    {"cpi", "LU1194076995", "Sifter Fund Global PI", "SIFTERPI"},
};

}  // namespace

bool SifterSource::parse(const std::string & body, Store & store, std::string & error, std::vector<std::string> & notes) {
  const json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_array()) {
    error = "not a list of days";
    return false;
  }
  std::vector<std::vector<Candle>> bars(std::size(kClasses));
  int skipped = 0;
  for (const json & row : doc) {
    const auto day = row.is_object() && row.contains("d") && row["d"].is_string() ? dayNumberOfIsoDate(row["d"].get<std::string>()) : std::nullopt;
    if (!day) {
      skipped++;
      continue;
    }
    for (size_t i = 0; i < std::size(kClasses); i++) {
      auto it = row.find(kClasses[i].key);
      if (it == row.end() || it->is_null()) continue;  // the class has no NAV that day
      const auto value = looseNumber(*it);
      if (!value || *value <= 0) {
        skipped++;
        continue;
      }
      bars[i].push_back({*day, kUndefined, kUndefined, kUndefined, *value, kUndefined});
    }
  }
  if (skipped) notes.push_back(std::to_string(skipped) + " values that cannot be read skipped");
  const bool any = std::any_of(bars.begin(), bars.end(), [](const auto & b) { return !b.empty(); });
  if (!any) {
    error = "no prices";
    return false;
  }
  store.batch([&](Batch & b) {
    for (size_t i = 0; i < std::size(kClasses); i++) {
      auto & series = bars[i];
      std::stable_sort(series.begin(), series.end(), [](const Candle & a, const Candle & c) { return a.time < c.time; });
      std::vector<Candle> unique;  // one bar to a day: the later in the answer stands
      for (const Candle & c : series) {
        if (!unique.empty() && unique.back().time == c.time) unique.back() = c;
        else unique.push_back(c);
      }
      FundRow fund;
      fund.isin = kClasses[i].isin;
      fund.name = kClasses[i].name;
      fund.symbol = kClasses[i].symbol;
      writeFund(b, kSifterDataset, fund, unique);
    }
  });
  return true;
}

void SifterSource::load(Store & store, Http & http, SourceListener & listener) {
  if (listener.cancelled()) return;
  static const HttpHeaders headers = browserHeaders(kUrl);  // the chart page's own data call: same-origin
  const HttpResult r = http.get(kUrl, headers);
  if (r.status == 429 || r.status == 403) {
    listener.message(r.status == 429 ? "sifter: rate limited, stopping" : "sifter: refused, stopping");
    return;
  }
  if (!r.ok()) {
    listener.message("sifter: " + (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)));
    return;
  }
  std::string error;
  std::vector<std::string> notes;
  const bool done = parse(r.body, store, error, notes);
  for (const std::string & note : notes) listener.message("sifter: " + note);
  if (done) listener.changed();
  else listener.message("sifter: " + error);
}
