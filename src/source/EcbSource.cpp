#include "source/EcbSource.h"

#include "model/Dates.h"
#include "source/InstrumentLists.h"
#include "source/KnownMarkets.h"

#include <cellui/NumberParse.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace {

// EST is the euro short-term rate dataset; its key is frequency (B: business days), benchmark item (the series' ISIN) and the kind of
// series: WT the rate itself (the volume-weighted trimmed mean), CI the compounded index. The two series wanted, in one request.
constexpr const char * kUrl = "https://data-api.ecb.europa.eu/service/data/EST/B.EU000A2X2A25+EU000A2QQF08.WT+CI?format=structurespecificdata";

// A series wanted, found in the answer by its benchmark item and kind (the ISIN the ECB gives it).
struct Wanted {
  const char * isin;
  const char * kind;
  const char * symbol;
  const char * name;
  const char * quote;  // kPercentUnit: the rate; kPointsUnit: an index
};
constexpr Wanted kWanted[] = {
    {"EU000A2X2A25", "WT", "ESTR", "Euro short-term rate (€STR)", kPercentUnit},
    {"EU000A2QQF08", "CI", "ESTRINDEX", "Compounded €STR index", kPointsUnit},
};

}  // namespace

bool EcbSource::parse(const std::string & xml, Store & store, std::string & error, std::vector<std::string> & notes) {
  pugi::xml_document doc;
  if (const pugi::xml_parse_result loaded = doc.load_buffer(xml.data(), xml.size()); !loaded) {
    error = std::string("not XML: ") + loaded.description();
    return false;
  }
  std::map<std::string, std::vector<Candle>> bars;  // by the wanted series' symbol
  int skipped = 0;
  // Elements are found by their local names: the answer's namespaces are the ECB's to change.
  for (const pugi::xpath_node & s : doc.select_nodes("//*[local-name()='Series']")) {
    const pugi::xml_node series = s.node();
    const Wanted * wanted = nullptr;
    for (const Wanted & w : kWanted)
      if (std::string(series.attribute("BENCHMARK_ITEM").as_string()) == w.isin && std::string(series.attribute("DATA_TYPE_EST").as_string()) == w.kind) wanted = &w;
    if (!wanted) continue;  // another series of the dataset: not asked for, and not ours
    std::vector<Candle> & out = bars[wanted->symbol];
    for (const pugi::xpath_node & o : series.select_nodes("*[local-name()='Obs']")) {
      const auto day = dayNumberOfIsoDate(o.node().attribute("TIME_PERIOD").as_string());
      const std::string text = o.node().attribute("OBS_VALUE").as_string();
      // cellui::parseFloat, not strtod: the window toolkit sets the locale, and strtod would read "99.99" as 99 in one with a decimal comma.
      double value = 0;
      const char * last = text.data() + text.size();
      const auto parsed = cellui::parseFloat(text.data(), last, value);
      if (!day || text.empty() || parsed.ec != std::errc() || parsed.ptr != last || !std::isfinite(value)) {
        skipped++;
        continue;
      }
      out.push_back({*day, kUndefined, kUndefined, kUndefined, value, kUndefined});
    }
    std::sort(out.begin(), out.end(), [](const Candle & a, const Candle & b) { return a.time < b.time; });
  }
  if (skipped) notes.push_back(std::to_string(skipped) + " observations that cannot be read skipped");
  std::erase_if(bars, [](const auto & b) { return b.second.empty(); });
  if (bars.empty()) {
    error = "no series";
    return false;
  }

  // The whole answer is one publication.
  store.batch([&](Batch & b) {
    const DatasetId dataset = b.addDataset(kEcbDataset, "European Central Bank");
    const MarketId market = b.addMarket(kNoMarket, marketNameOfMic(kNoMarket));
    for (const Wanted & w : kWanted) {
      auto it = bars.find(w.symbol);
      if (it == bars.end()) continue;
      const InstrumentId base = addInstrumentUnlessSaid(b, "", w.name, InstrumentKind::Index, w.isin);
      const EdgeKey edge{base, quoteNode(b, w.quote), market, dataset};
      b.appendCandles(edge, Resolution::Day, it->second, QueueKind::Scalar);  // a rate and an index value: one value a day
      b.setListing({edge, w.symbol, "", "", "", w.name});
    }
  });
  return true;
}

void EcbSource::load(Store & store, Http & http, SourceListener & listener) {
  if (listener.cancelled()) return;
  HttpResult r = http.get(kUrl);
  if (r.status == 429 || r.status == 403) {
    listener.message(r.status == 429 ? "ecb: rate limited, stopping" : "ecb: refused, stopping");
    return;
  }
  if (!r.ok()) {
    listener.message("ecb: " + (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)));
    return;
  }
  std::string error;
  std::vector<std::string> notes;
  const bool done = parse(r.body, store, error, notes);
  for (const std::string & note : notes) listener.message("ecb: " + note);
  if (done) listener.changed();
  else listener.message("ecb: " + error);
}
