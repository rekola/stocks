#pragma once

#include "model/Types.h"

#include <filesystem>
#include <string>
#include <vector>

// One instrument a preset shows: what one unit of `base` costs in `quote`, on `market`, with the symbol each provider
// uses for it. The lists are files (data/universe/*.csv), not code.
struct UniverseEntry {
  std::string market, base, quote, name, yahoo;
  InstrumentKind kind = InstrumentKind::Other;  // of the base
};
using Universe = std::vector<UniverseEntry>;

// Reads every *.csv in `directory`, in file name order. A file is one market:
//   # market: Nasdaq Helsinki          the market's name (required; other # lines are comments)
//   kind,base,quote,name,yahoo         the header (optional)
//   company,NOKIA,EUR,Nokia Oyj,NOKIA.HE
// `kind` is company, currency, commodity, index or other; a field with a comma is in double quotes. Returns nothing,
// with `error` set, if the directory or a file is wrong.
Universe loadUniverse(const std::filesystem::path & directory, std::string & error);

// The entries of `universe` whose market is one of `markets` (all of them if `markets` is empty), in the same order.
Universe onlyMarkets(const Universe & universe, const std::vector<std::string> & markets);

// Where the lists are: `given` if it is not empty, else data/universe here, else the source tree's. Empty if none is.
std::filesystem::path findUniverseDirectory(const std::string & given);

// The name of a currency that only appears as a quote ("" if unknown).
const char * currencyName(const std::string & symbol);
