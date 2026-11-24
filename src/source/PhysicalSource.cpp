#include "source/PhysicalSource.h"

#include "model/Dates.h"
#include "source/DataDirectory.h"
#include "source/KnownIsins.h"
#include "source/KnownMarkets.h"

#include <cellui/NumberParse.h>

#include <algorithm>
#include <cmath>
#include <map>
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

// The ISIN of an item, by its symbol: the one source/KnownIsins.h gives it, so an item that is added to the file needs its ISIN added there first
// (naming the constant here is what makes a missing one a compile error). Empty for an item not named here.
std::string itemIsin(const std::string & symbol) {
  static const std::pair<const char *, const char *> table[] = {
      {"GOLDBAR", known_isin::kGoldBar}, {"GOLDCOIN", known_isin::kGoldCoin}, {"PLATBAR", known_isin::kPlatinumBar}};
  for (const auto & [name, isin] : table)
    if (symbol == name) return isin;
  return {};
}

}  // namespace

bool parsePhysicalTable(const std::string & text, PhysicalTable & table, std::string & error) {
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
      if (f.size() != 3 || f[1].empty() || f[2].empty()) {
        error = where + "a spot row is spot,METAL,SYMBOL";
        return false;
      }
      table.spots.push_back({f[1], f[2]});
    } else if (f[0] == "item") {
      if (f.size() != 7 && f.size() != 8) {
        error = where + "an item row is item,METAL,SYMBOL,NAME,OUNCES,BID,ASK[,ICON_SYMBOL]";
        return false;
      }
      const auto ounces = number(f[4]), bid = number(f[5]), ask = number(f[6]);
      if (f[1].empty() || f[2].empty() || f[3].empty() || !ounces || !bid || !ask || *ounces <= 0 || *bid <= 0 || *ask <= 0) {
        error = where + "an item needs a metal, a symbol, a name and numbers above zero";
        return false;
      }
      const bool known = std::any_of(table.spots.begin(), table.spots.end(), [&](const PhysicalTable::Spot & x) { return x.metal == f[1]; });
      if (!known) {
        error = where + "no spot row for the metal " + f[1] + " before it";
        return false;
      }
      const std::string isin = itemIsin(f[2]);
      if (isin.empty()) {
        error = where + "the item " + f[2] + " has no ISIN: add one to source/KnownIsins.h and name it in itemIsin() in PhysicalSource.cpp";
        return false;
      }
      table.items.push_back({f[1], f[2], f[3], *ounces, *bid, *ask, f.size() == 8 ? f[7] : std::string(), isin});
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

bool loadPhysicalTable(PhysicalTable & table, std::string & error) {
  const std::filesystem::path dir = findDataDirectory("physical");
  if (dir.empty()) {
    error = "no data/physical directory found";
    return false;
  }
  std::ifstream in(dir / "items.csv");
  if (!in) {
    error = (dir / "items.csv").string() + " cannot be read";
    return false;
  }
  std::stringstream text;
  text << in.rdbuf();
  return parsePhysicalTable(text.str(), table, error);
}

void PhysicalSource::load(Store & store, Http &, SourceListener & listener) {
  const auto snapshot = store.snapshot();
  // The spot prices there are, for each metal one for each currency they are in: of the symbols the file names for the metal, the first
  // it names that has a price in that currency (the file puts the one with prices in both euros and dollars first).
  struct Spot {
    size_t rank;
    EdgeKey edge;
    Candle bar;
  };
  std::map<std::string, std::map<uint64_t, Spot>> spots;  // by the metal, then by the quote currency
  if (const auto market = snapshot->findMarket(kNoMarket))
    for (const EdgeKey & key : snapshot->edges(*market)) {
      const auto listing = snapshot->listing(key);
      if (!listing) continue;
      const CandleSeries last = snapshot->lastCandles(key, Resolution::Day, 1);
      if (last.candles.empty() || !isDefined(last.candles.back().close)) continue;
      for (size_t rank = 0; rank < table_.spots.size(); rank++) {
        if (table_.spots[rank].symbol != listing->symbol) continue;
        auto & of = spots[table_.spots[rank].metal];
        const auto found = of.find(key.quote.value);
        if (found == of.end() || rank < found->second.rank) of[key.quote.value] = {rank, key, last.candles.back()};
      }
    }
  if (spots.empty()) {
    // Another source writes the spot, perhaps still loading: try again soon, and say so once if it stays away.
    if (++misses_ == 3) listener.message("physical: no spot price in the store yet, so no estimate for physical gold");
    return;
  }
  misses_ = 0;
  store.batch([&](Batch & b) {
    const DatasetId dataset = b.addDataset(kPhysicalDataset, "Estimate");
    const MarketId market = b.addMarket(kOffExchange, marketNameOfMic(kOffExchange));
    for (const PhysicalItem & item : table_.items) {
      const auto metal = spots.find(item.metal);
      if (metal == spots.end()) continue;  // no price for this metal yet: nothing of it is made
      const InstrumentId instrument = b.resolveInstrument({{predicate::isin, item.isin}}, {{predicate::name, item.name},
                                                               {predicate::symbol, item.symbol},
                                                               {predicate::kind, kindName(InstrumentKind::Commodity)},
                                                               {predicate::type, "PHYSICAL"}});
      if (!instrument.valid()) continue;
      if (!item.symbol_icon.empty()) b.setAttribute(NodeRef(instrument), {predicate::iconSymbol, item.symbol_icon});
      const double scale = item.ounces * item.bid;  // an undefined open, high or low stays undefined
      // One edge for each currency the metal's spot is in: a price in euros for an item held in euros, and one in dollars.
      for (const auto & [quote, spot] : metal->second) {
        const EdgeKey edge{instrument, spot.edge.quote, market, dataset};
        const Candle & bar = spot.bar;
        b.appendCandles(edge, Resolution::Day, {{bar.time, bar.open * scale, bar.high * scale, bar.low * scale, bar.close * scale, kUndefined}});
        b.setListing({edge, item.symbol, "", "", "PHYSICAL", item.name});
      }
    }
  });
  listener.changed();
}
