#include "source/Universe.h"

const std::vector<UniverseEntry> & universe() {
  using K = InstrumentKind;
  static const std::vector<UniverseEntry> u = {
    {"Commodities", "XAU", "USD", "Gold", "GC=F", K::Commodity},
    {"Commodities", "XAG", "USD", "Silver", "SI=F", K::Commodity},
    {"Commodities", "BRENT", "USD", "Brent Crude", "BZ=F", K::Commodity},
    {"Commodities", "WTI", "USD", "WTI Crude", "CL=F", K::Commodity},
    {"Commodities", "HG", "USD", "Copper", "HG=F", K::Commodity},
    {"Commodities", "NG", "USD", "Natural Gas", "NG=F", K::Commodity},
    {"Commodities", "XPT", "USD", "Platinum", "PL=F", K::Commodity},
    {"Commodities", "ZW", "USD", "Wheat", "ZW=F", K::Commodity},
    {"Commodities", "ZC", "USD", "Corn", "ZC=F", K::Commodity},
    {"Commodities", "KC", "USD", "Coffee", "KC=F", K::Commodity},
    {"Commodities", "SB", "USD", "Sugar", "SB=F", K::Commodity},
    {"Commodities", "CT", "USD", "Cotton", "CT=F", K::Commodity},
    {"Nasdaq Helsinki", "NOKIA", "EUR", "Nokia", "NOKIA.HE", K::Company},
    {"Nasdaq Helsinki", "FORTUM", "EUR", "Fortum", "FORTUM.HE", K::Company},
    {"Nasdaq Helsinki", "SAMPO", "EUR", "Sampo", "SAMPO.HE", K::Company},
    {"Nasdaq Helsinki", "KNEBV", "EUR", "Kone", "KNEBV.HE", K::Company},
    {"Nasdaq Helsinki", "UPM", "EUR", "UPM-Kymmene", "UPM.HE", K::Company},
    {"Nasdaq Helsinki", "NESTE", "EUR", "Neste", "NESTE.HE", K::Company},
    {"Nasdaq Helsinki", "STERV", "EUR", "Stora Enso", "STERV.HE", K::Company},
    {"Nasdaq Helsinki", "WRT1V", "EUR", "Wartsila", "WRT1V.HE", K::Company},
    {"Nasdaq Helsinki", "ELISA", "EUR", "Elisa", "ELISA.HE", K::Company},
    {"Nasdaq Helsinki", "ORNBV", "EUR", "Orion", "ORNBV.HE", K::Company},
    {"Nasdaq Helsinki", "NDA FI", "EUR", "Nordea", "NDA-FI.HE", K::Company},
    {"Nasdaq Helsinki", "METSO", "EUR", "Metso", "METSO.HE", K::Company},
    {"Currencies", "EUR", "USD", "Euro", "EURUSD=X", K::Currency},
    {"Currencies", "USD", "JPY", "US dollar", "USDJPY=X", K::Currency},
    {"Currencies", "GBP", "USD", "British pound", "GBPUSD=X", K::Currency},
    {"Currencies", "USD", "CHF", "US dollar", "USDCHF=X", K::Currency},
    {"Currencies", "AUD", "USD", "Australian dollar", "AUDUSD=X", K::Currency},
    {"Currencies", "USD", "CAD", "US dollar", "USDCAD=X", K::Currency},
    {"Currencies", "EUR", "SEK", "Euro", "EURSEK=X", K::Currency},
    {"Currencies", "EUR", "GBP", "Euro", "EURGBP=X", K::Currency},
    {"Currencies", "USD", "NOK", "US dollar", "USDNOK=X", K::Currency},
    {"Currencies", "EUR", "CHF", "Euro", "EURCHF=X", K::Currency},
    {"Currencies", "NZD", "USD", "New Zealand dollar", "NZDUSD=X", K::Currency},
    {"Currencies", "USD", "CNY", "US dollar", "USDCNY=X", K::Currency},
  };
  return u;
}

const char * currencyName(const std::string & symbol) {
  static const std::pair<const char *, const char *> names[] = {
    {"USD", "US dollar"}, {"EUR", "Euro"}, {"JPY", "Japanese yen"}, {"CHF", "Swiss franc"}, {"CAD", "Canadian dollar"},
    {"SEK", "Swedish krona"}, {"GBP", "British pound"}, {"NOK", "Norwegian krone"}, {"CNY", "Chinese yuan"}};
  for (const auto & [s, n] : names)
    if (symbol == s) return n;
  return "";
}
