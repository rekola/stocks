#include "source/KnownMarkets.h"

namespace {

// The markets met so far on the stock and ETF lists, and those the instrument lists add; the codes are what the providers' own pages say.
constexpr KnownMarket kMarkets[] = {
    {4, "Xetra", "XETR"},
    {11, "Nasdaq Stockholm", "XSTO"},
    {13, "Nordic Growth Market", "XNGM"},
    {14, "Nasdaq Copenhagen", "XCSE"},
    {15, "Oslo Børs", "XOSL"},
    {17, "New York Stock Exchange", "XNYS"},
    {18, "NYSE American", "XASE"},
    {19, "Nasdaq", "XNAS"},
    {21, "OTC Markets", "XOTC"},
    {24, "Nasdaq Helsinki", "XHEL"},
    {25, "Toronto Stock Exchange", "XTSE"},
    {26, "TSX Venture Exchange", "XTSX"},
    {49, "Euronext Growth Oslo", "MERK"},
    {52, "Spotlight Stock Market", "XSAT"},
    {53, "First North Sweden", "FNSE"},
    {54, "First North Finland", "FNFI"},
    {55, "First North Denmark", "FNDK"},
    {61, "Nordic MTF", "NMTF"},
    {67, "Spotlight Stock Market DK", "SPDK"},
    {68, "Spotlight Stock Market NO", "SPNO"},
    {70, "London Stock Exchange", "XLON"},
    {72, "Euronext Paris", "XPAR"},
    {73, "Euronext Lisbon", "XLIS"},
    {74, "Euronext Amsterdam", "XAMS"},
    {75, "Euronext Brussels", "XBRU"},
    {76, "Euronext Dublin", "XDUB"},
    {77, "Euronext Milan", "XMIL"},
    {81, "Vienna Stock Exchange", "XWBO"},
    {82, "Madrid Stock Exchange", "XMAD"},
    {83, "SIX Swiss Exchange", "XSWX"},
    {0, "NYSE Arca", "ARCX"},
    {0, "No market", "XXXX"}, // indices, spot prices and currency pairs: what
                              // is quoted on no venue
    {0, "Off exchange",
     "XOFF"}, // what is priced by its provider and traded on no venue: funds
};

}  // namespace

const KnownMarket * knownMarketByNordnetId(int id) {
  if (id == 0) return nullptr;
  for (const KnownMarket & m : kMarkets)
    if (m.nordnetId == id) return &m;
  return nullptr;
}

const char * marketNameOfMic(const std::string & mic) {
  for (const KnownMarket & m : kMarkets)
    if (m.mic && mic == m.mic) return m.name;
  return nullptr;
}
