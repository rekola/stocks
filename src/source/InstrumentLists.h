#pragma once

#include "model/Store.h"

#include <filesystem>
#include <string>
#include <vector>

// One instrument a preset shows: what one unit of `base` costs in `quote`, on `market`, with the symbol each provider
// uses for it. These are the instrument lists of the Yahoo adapter (Yahoo has no discovery), which stand in until every
// adapter finds its own instruments; the synthetic source follows them too. They are files (data/lists/*.csv), not code.
struct ListEntry {
  std::string market, base, quote, name, yahoo;
  InstrumentKind kind = InstrumentKind::Other;  // of the base
  std::string isin;  // of the base: what the instrument is (empty only for a currency, which is its code)
  std::string mic =
      kNoMarket; // the market's ISO 10383 code (XXXX where the row has none: an
                 // index, a spot price, a currency pair)
};
using InstrumentList = std::vector<ListEntry>;

// Reads every *.csv in `directory`, in file name order. A row is on a market,
// given by its code (a seventh field); a row with none is on no market (XXXX:
// an index, a spot price, a currency pair). A line starting with # is a
// comment.
//   kind,base,quote,name,yahoo,isin,mic   the header (optional; the last field
//   may be left out of a row) company,NOKIA,EUR,Nokia
//   Oyj,NOKIA.HE,FI0009000681,XHEL
// The code is the market (KnownMarkets names it); a code not known is refused.
// A currency has no ISIN and is its code; anything else must have one (Yahoo
// gives none, so the data does). An index is quoted in `pts`, index points.
// `kind` is company, currency, commodity, index, etf, certificate or other; a
// field with a comma is in double quotes. Returns nothing, with `error` set, if
// the directory or a file is wrong.
InstrumentList loadInstrumentLists(const std::filesystem::path & directory, std::string & error);

// The entries of `list` whose market code is one of `markets` (all of them if
// `markets` is empty), in the same order.
InstrumentList onlyMarkets(const InstrumentList & list, const std::vector<std::string> & markets);

// Where the lists are: `given` if it is not empty, else the data/lists of findDataDirectory. Empty if none is.
std::filesystem::path findListsDirectory(const std::string & given);

// The name of a currency that only appears as a quote ("" if unknown).
const char * currencyName(const std::string & symbol);

// The node for a currency's symbol: the one already in the store, whoever made it, else a new currency named as it is
// usually called. A provider that has a currency gets the node this way and does not describe it.
// Inside a batch it looks up what the batch has written, and outside one it is a batch of its own.
InstrumentId currencyNode(Batch &, const std::string & symbol);
InstrumentId currencyNode(Store &, const std::string & symbol);
// The instrument by its ISIN, saying its name and type only where none is said yet: for a source that is not the
// instrument's own, whose word must not replace another's each time it loads. (Without an ISIN the name is what the
// instrument is found by, so it is said as it is.)
InstrumentId addInstrumentUnlessSaid(Batch &, const std::string & symbol, const std::string & name, InstrumentKind, const std::string & isin);
// What a price is in: a currency's node, or the unit node for `pts`, index points.
InstrumentId quoteNode(Batch &, const std::string & code);
InstrumentId quoteNode(Store &, const std::string & code);

// Which lists a source loads: a directory (empty for the default) and the markets to keep (empty for all of them).
struct ListSpec {
  std::string directory;
  std::vector<std::string> markets;
};

// The list `spec` names, read from disk. Empty, with `error` set, if there is none.
InstrumentList resolveList(const ListSpec & spec, std::string & error);
