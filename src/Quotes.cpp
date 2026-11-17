#include "Quotes.h"

#include <cstdio>
#include <random>

// A random walk per instrument, the same every run.
std::vector<Instrument> makeRows(int preset) {
  struct Seed {
    const char *base, *quote, *name;
  };
  static const Seed seeds[3][12] = {
    {{"XAU", "USD", "Gold"}, {"XAG", "USD", "Silver"}, {"BRENT", "USD", "Brent Crude"}, {"WTI", "USD", "WTI Crude"},
     {"HG", "USD", "Copper"}, {"NG", "USD", "Natural Gas"}, {"XPT", "USD", "Platinum"}, {"ZW", "USD", "Wheat"},
     {"ZC", "USD", "Corn"}, {"KC", "USD", "Coffee"}, {"SB", "USD", "Sugar"}, {"CT", "USD", "Cotton"}},
    {{"NOKIA", "EUR", "Nokia"}, {"FORTUM", "EUR", "Fortum"}, {"SAMPO", "EUR", "Sampo"}, {"KNEBV", "EUR", "Kone"},
     {"UPM", "EUR", "UPM-Kymmene"}, {"NESTE", "EUR", "Neste"}, {"STERV", "EUR", "Stora Enso"}, {"WRT1V", "EUR", "Wartsila"},
     {"ELISA", "EUR", "Elisa"}, {"ORNBV", "EUR", "Orion"}, {"NDA FI", "EUR", "Nordea"}, {"METSO", "EUR", "Metso"}},
    {{"EUR", "USD", "Euro"}, {"USD", "JPY", "US dollar"}, {"GBP", "USD", "British pound"}, {"USD", "CHF", "US dollar"},
     {"AUD", "USD", "Australian dollar"}, {"USD", "CAD", "US dollar"}, {"EUR", "SEK", "Euro"}, {"EUR", "GBP", "Euro"},
     {"USD", "NOK", "US dollar"}, {"EUR", "CHF", "Euro"}, {"NZD", "USD", "New Zealand dollar"}, {"USD", "CNY", "US dollar"}}};
  std::mt19937 rng(static_cast<unsigned>(preset) * 7919u + 1u);
  std::uniform_real_distribution<double> step(-0.03, 0.032), start(1.5, 300.0);
  std::vector<Instrument> rows;
  for (const Seed & seed : seeds[preset]) {
    Instrument r;
    r.base = seed.base;
    r.quote = seed.quote;
    r.name = seed.name;
    double p = start(rng);
    for (int d = 0; d < 60; d++) {
      p *= 1.0 + step(rng);
      r.history.push_back(static_cast<float>(p));
    }
    r.price = p;
    r.change = (r.history.back() / r.history[r.history.size() - 2] - 1.0) * 100.0;
    rows.push_back(std::move(r));
  }
  return rows;
}


std::string fixed(double v, int decimals, bool sign) {
  char buf[32];
  std::snprintf(buf, sizeof buf, sign ? "%+.*f" : "%.*f", decimals, v);
  return buf;
}
