#pragma once

#include "model/Store.h"

#include <filesystem>
#include <string>
#include <vector>

// One instrument a preset shows: what one unit of `base` costs in `quote`, on `market`, with the symbol each provider
// uses for it. These are the instrument lists of the Yahoo adapter (Yahoo has no discovery), which stand in until every
// adapter finds its own instruments; the synthetic source follows them too. They are files (data/yahoo/*.csv), not code.
struct ListEntry {
  std::string market, base, quote, name, yahoo;
  InstrumentKind kind = InstrumentKind::Other;  // of the base
};
using InstrumentList = std::vector<ListEntry>;

// Reads every *.csv in `directory`, in file name order. A file is one market:
//   # market: Nasdaq Helsinki          the market's name (required; other # lines are comments)
//   kind,base,quote,name,yahoo         the header (optional)
//   company,NOKIA,EUR,Nokia Oyj,NOKIA.HE
// `kind` is company, currency, commodity, index, etf, certificate or other; a field with a comma is in double quotes. Returns nothing,
// with `error` set, if the directory or a file is wrong.
InstrumentList loadInstrumentLists(const std::filesystem::path & directory, std::string & error);

// The entries of `list` whose market is one of `markets` (all of them if `markets` is empty), in the same order.
InstrumentList onlyMarkets(const InstrumentList & list, const std::vector<std::string> & markets);

// Where the lists are: `given` if it is not empty, else data/yahoo here, else the source tree's. Empty if none is.
std::filesystem::path findListsDirectory(const std::string & given);

// The name of a currency that only appears as a quote ("" if unknown).
const char * currencyName(const std::string & symbol);

// The node for a currency's symbol: the one already in the store, whoever made it, else a new currency named as it is
// usually called. A provider that has a currency gets the node this way and does not describe it.
Instrument currencyNode(Store &, const std::string & symbol);

// Which lists a source loads: a directory (empty for the default) and the markets to keep (empty for all of them).
struct ListSpec {
  std::string directory;
  std::vector<std::string> markets;
};

// The list `spec` names, read from disk. Empty, with `error` set, if there is none.
InstrumentList resolveList(const ListSpec & spec, std::string & error);
