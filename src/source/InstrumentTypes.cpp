#include "source/InstrumentTypes.h"

InstrumentKind kindOfNordnetType(const std::string & type) {
  if (type == "UETF" || type == "KETF") return InstrumentKind::Etf;
  if (type == "PTC") return InstrumentKind::Certificate;
  // Shares: ordinary, preference, multilateral-facility and over-the-counter listings.
  if (type == "ESH" || type == "PESH" || type == "ESHMTF" || type == "ESHOTC") return InstrumentKind::Company;
  if (type == "FUCITS" || type == "FOTH") return InstrumentKind::Fund;
  return InstrumentKind::Other;
}

InstrumentKind kindOfYahooType(const std::string & type) {
  if (type == "EQUITY") return InstrumentKind::Company;
  if (type == "ETF") return InstrumentKind::Etf;
  if (type == "INDEX") return InstrumentKind::Index;
  if (type == "FUTURE") return InstrumentKind::Commodity;
  if (type == "CURRENCY") return InstrumentKind::Currency;
  return InstrumentKind::Other;
}
