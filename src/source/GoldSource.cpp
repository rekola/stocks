#include "source/GoldSource.h"

#include "model/Dates.h"
#include "source/DataDirectory.h"
#include "source/KnownMarkets.h"

#include <cellui/NumberParse.h>

#include <cmath>
#include <fstream>
#include <sstream>

namespace {

std::vector<std::string> fields(const std::string & line) {
  std::vector<std::string> out;
  std::string cell;
  std::istringstream in(line);
  while (std::getline(in, cell, ',')) out.push_back(cell);
  if (!line.empty() && line.back() == ',') out.emplace_back();
  return out;
}

std::optional<double> number(const std::string & text) {
  double v = 0;
  const char * last = text.data() + text.size();
  const auto parsed = cellui::parseFloat(text.data(), last, v);
  if (text.empty() || parsed.ec != std::errc() || parsed.ptr != last || !std::isfinite(v)) return std::nullopt;
  return v;
}

}  // namespace

bool parseGoldTable(const std::string & text, GoldTable & table, std::string & error) {
  table = {};
  std::istringstream in(text);
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    n++;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const std::vector<std::string> f = fields(line);
    const std::string where = "line " + std::to_string(n) + ": ";
    if (f[0] == "spot") {
      if (f.size() != 2 || f[1].empty()) {
        error = where + "a spot row is spot,SYMBOL";
        return false;
      }
      table.spots.push_back(f[1]);
    } else if (f[0] == "item") {
      if (f.size() != 6 && f.size() != 7) {
        error = where + "an item row is item,SYMBOL,NAME,OUNCES,BID,ASK[,ICON_SYMBOL]";
        return false;
      }
      const auto ounces = number(f[3]), bid = number(f[4]), ask = number(f[5]);
      if (f[1].empty() || f[2].empty() || !ounces || !bid || !ask || *ounces <= 0 || *bid <= 0 || *ask <= 0) {
        error = where + "an item needs a symbol, a name and numbers above zero";
        return false;
      }
      table.items.push_back({f[1], f[2], *ounces, *bid, *ask, f.size() == 7 ? f[6] : std::string()});
    } else {
      error = where + "a row is spot or item";
      return false;
    }
  }
  if (table.spots.empty() || table.items.empty()) {
    error = "it needs a spot row and an item row";
    return false;
  }
  return true;
}

bool loadGoldTable(GoldTable & table, std::string & error) {
  const std::filesystem::path dir = findDataDirectory("gold");
  if (dir.empty()) {
    error = "no data/gold directory found";
    return false;
  }
  std::ifstream in(dir / "items.csv");
  if (!in) {
    error = (dir / "items.csv").string() + " cannot be read";
    return false;
  }
  std::stringstream text;
  text << in.rdbuf();
  return parseGoldTable(text.str(), table, error);
}

void GoldSource::load(Store & store, Http &, SourceListener & listener) {
  const auto snapshot = store.snapshot();
  std::optional<EdgeKey> spot;
  if (const auto market = snapshot->findMarket(kNoMarket))
    for (const std::string & symbol : table_.spots)
      if (!spot) spot = snapshot->findEdge(symbol, *market);
  const CandleSeries last = spot ? snapshot->lastCandles(*spot, Resolution::Day, 1) : CandleSeries{};
  if (!spot || last.candles.empty() || !isDefined(last.candles.back().close)) {
    // Another source writes the spot, perhaps still loading: try again at the next refresh, and say so once if it stays away.
    if (++misses_ == 3) listener.message("gold: no gold spot price in the store yet, so no estimate for gold");
    return;
  }
  misses_ = 0;
  const Candle spotBar = last.candles.back();
  store.batch([&](Batch & b) {
    const DatasetId dataset = b.addDataset(kGoldDataset, "Estimate");
    const MarketId market = b.addMarket(kOffExchange, marketNameOfMic(kOffExchange));
    for (const GoldItem & item : table_.items) {
      const InstrumentId instrument = b.resolveInstrument({}, {{predicate::name, item.name},
                                                               {predicate::symbol, item.symbol},
                                                               {predicate::kind, kindName(InstrumentKind::Commodity)},
                                                               {predicate::type, "PHYSICAL"}});
      if (!instrument.valid()) continue;
      if (!item.symbol_icon.empty()) b.setAttribute(NodeRef(instrument), {predicate::iconSymbol, item.symbol_icon});
      const EdgeKey edge{instrument, spot->quote, market, dataset};
      const double scale = item.ounces * item.bid;  // an undefined open, high or low stays undefined
      b.appendCandles(edge, Resolution::Day,
                      {{spotBar.time, spotBar.open * scale, spotBar.high * scale, spotBar.low * scale, spotBar.close * scale, kUndefined}});
      b.setListing({edge, item.symbol, "", "", "PHYSICAL", item.name});
    }
  });
  listener.changed();
}
