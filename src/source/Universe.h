#pragma once

#include "model/Types.h"

#include <vector>

// The instruments the presets show, with the symbol each provider uses for them. A provider adapter reads its own
// column; the market is where the preset looks.
struct UniverseEntry {
  const char *market, *base, *quote, *name, *yahoo;
  InstrumentKind kind;  // of the base
};

const std::vector<UniverseEntry> & universe();

// The name of a currency that only appears as a quote ("" if unknown).
const char * currencyName(const std::string & symbol);
