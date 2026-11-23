#pragma once

#include <string>

// The markets the lists know by name: what a provider calls one (Nordnet by an id) and its ISO 10383 market code (MIC), the way an ISIN
// is the code of an instrument. A market is found by its code before its name, so no two share a code; one that is a segment of
// another has its segment's code.
struct KnownMarket {
  int nordnetId;       // Nordnet's id for it; zero where only a code is known
  const char * name;
  const char *mic; // null where not known exactly: a listing on it is left out,
                   // as data with no market is refused
};

// The market Nordnet calls `id`, or null.
const KnownMarket * knownMarketByNordnetId(int id);
// The name of the market with this code, or null: what a list that gives only codes calls its markets.
const char * marketNameOfMic(const std::string & mic);
