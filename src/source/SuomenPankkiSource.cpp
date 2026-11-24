#include "source/SuomenPankkiSource.h"

#include "model/Dates.h"
#include "source/InstrumentLists.h"
#include "source/KnownMarkets.h"
#include "source/LeftOut.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>

// What is stored is the rate, in per cent, as the bank publishes it, one bar a day (only its close; there is no open or high).
// It is not accumulated into a price, as the legacy loader did (a running product of 1 + rate / 100 / 360 over the samples):
// that is a choice about how to accrue (the day count, the days between two samples, whether a 12-month rate is reinvested
// daily), made at load time and baked into numbers that cannot be told from observations. The rate is the fact; a total-return
// index is derived from it, by whoever wants one.

namespace {

constexpr const char * kUrl =
    "https://reports.suomenpankki.fi/WebForms/ReportViewerPage.aspx?report=/tilastot/markkina-_ja_hallinnolliset_korot/"
    "euribor_korot_xml_short_fi&output=xml";

// A rate of the report. The report names a maturity in Finnish, "3 kk (tod.pv/360)" (the part before the bracket is the maturity,
// the bracket the day count). The ISINs are the ones the Euribor rates have.
struct Tenor {
  const char * label;  // the report's, before " ("
  const char * symbol;
  const char * name;
  const char * isin;
};
constexpr Tenor kTenors[] = {
    {"1 vko", "EURIBOR1W", "Euribor 1W", "EU0009678507"},  {"1 kk", "EURIBOR1M", "Euribor 1M", "EU0009659937"},
    {"3 kk", "EURIBOR3M", "Euribor 3M", "EU0009652783"},   {"6 kk", "EURIBOR6M", "Euribor 6M", "EU0009652791"},
    {"12 kk", "EURIBOR12M", "Euribor 12M", "EU0009652809"},
};

// What the report calls the maturity: its name up to the bracket.
std::string labelOf(const std::string & name) {
  std::string label = name.substr(0, name.find(" ("));
  while (!label.empty() && label.back() == ' ') label.pop_back();
  return label;
}

// "-0,545" as -0.545, or none for an empty value (a rate the bank has not for that day) or one that is not a number.
std::optional<double> rateOf(std::string text) {
  std::replace(text.begin(), text.end(), ',', '.');
  text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
  if (text.empty()) return std::nullopt;
  char * end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (*end != '\0' || !std::isfinite(v)) return std::nullopt;
  return v;
}

}  // namespace

bool SuomenPankkiSource::parse(const std::string & xml, Store & store, std::string & error, std::vector<std::string> & notes) {
  pugi::xml_document doc;
  // The answer starts with a byte order mark, which pugixml takes off.
  if (const pugi::xml_parse_result loaded = doc.load_buffer(xml.data(), xml.size()); !loaded) {
    error = std::string("not XML: ") + loaded.description();
    return false;
  }
  std::map<std::string, std::vector<Candle>> bars;  // by the tenor's symbol
  std::map<std::string, int> unknown;               // the rates that are not known, and how many days had them
  int badDays = 0, badValues = 0;
  for (const pugi::xpath_node & p : doc.select_nodes("//period")) {
    const pugi::xml_node period = p.node();
    const auto day = dayNumberOfIsoDate(period.attribute("value").as_string());
    if (!day) {
      badDays++;
      continue;
    }
    for (const pugi::xpath_node & r : period.select_nodes(".//rate")) {
      const std::string name = r.node().attribute("name").as_string();
      const std::string label = labelOf(name);
      if (label == "Eonia") continue;  // overnight, and not published since 2022: the report still has the column, empty
      const Tenor * tenor = nullptr;
      for (const Tenor & t : kTenors)
        if (label == t.label) tenor = &t;
      if (!tenor) {
        unknown[name]++;
        continue;
      }
      const char * text = r.node().select_node(".//intr").node().attribute("value").as_string();
      if (!*text) continue;  // no rate that day
      const auto rate = rateOf(text);
      if (!rate) {
        badValues++;
        continue;
      }
      bars[tenor->symbol].push_back({*day, kUndefined, kUndefined, kUndefined, *rate, kUndefined});
    }
  }
  for (const auto & [name, days] : unknown) notes.push_back("unknown rate '" + name + "' (" + std::to_string(days) + " days) skipped");
  if (badDays) notes.push_back(std::to_string(badDays) + " days with a date that cannot be read skipped");
  if (badValues) notes.push_back(std::to_string(badValues) + " rates that are not numbers skipped");
  if (bars.empty()) {
    error = "no rates";
    return false;
  }
  for (auto & [symbol, series] : bars) std::sort(series.begin(), series.end(), [](const Candle & a, const Candle & b) { return a.time < b.time; });

  // The whole report is one publication.
  LeftOut leftOut;
  store.batch([&](Batch & b) {
    const DatasetId dataset = b.addDataset(kSuomenPankkiDataset, "Suomen Pankki");
    const MarketId market = b.addMarket(kNoMarket, marketNameOfMic(kNoMarket));
    const InstrumentId quote = quoteNode(b, "pct");
    for (const Tenor & t : kTenors) {
      auto it = bars.find(t.symbol);
      if (it == bars.end()) continue;
      const InstrumentId base = addInstrumentUnlessSaid(b, "", t.name, InstrumentKind::Index, t.isin);
      if (!base.valid() || !quote.valid() || !market.valid() || !dataset.valid()) {  // left out, not the report
        leftOut.add(t.symbol);
        continue;
      }
      const EdgeKey edge{base, quote, market, dataset};
      b.appendCandles(edge, Resolution::Day, it->second);
      b.setListing({edge, t.symbol, "", "", "", t.name});
    }
  });
  if (leftOut.count()) notes.push_back(leftOut.text("", "rate series", "rate series"));
  return true;
}

void SuomenPankkiSource::load(Store & store, Http & http, SourceListener & listener) {
  if (listener.cancelled()) return;
  HttpResult r = http.get(kUrl);
  if (r.status == 429 || r.status == 403) {
    listener.message(r.status == 429 ? "suomenpankki: rate limited, stopping" : "suomenpankki: refused, stopping");
    return;
  }
  if (!r.ok()) {
    listener.message("suomenpankki: " + (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)));
    return;
  }
  std::string error;
  std::vector<std::string> notes;
  const bool done = parse(r.body, store, error, notes);
  for (const std::string & note : notes) listener.message("suomenpankki: " + note);
  if (done) listener.changed();
  else listener.message("suomenpankki: " + error);
}
