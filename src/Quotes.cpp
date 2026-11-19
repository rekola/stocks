#include "Quotes.h"

#include <cmath>
#include <cstdlib>
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
    r.change = (r.history.back() / r.history[r.history.size() - 2] - 1.0) * 100.0;  // over the last day
    rows.push_back(std::move(r));
  }
  return rows;
}


std::string fixed(double v, int decimals, bool sign) {
  // Whole numbers only: printf's %f writes the locale's decimal point, and a window toolkit sets the locale (a
  // decimal comma then reads back as another number when the table sorts).
  long long unit = 1;
  for (int i = 0; i < decimals; i++) unit *= 10;
  long long n = std::llround(std::abs(v) * static_cast<double>(unit));
  std::string out = std::to_string(n / unit);
  if (decimals > 0) {
    std::string fraction = std::to_string(n % unit);
    out += '.' + std::string(static_cast<size_t>(decimals) - fraction.size(), '0') + fraction;
  }
  if (v < 0 && n != 0) out.insert(out.begin(), '-');
  else if (sign) out.insert(out.begin(), '+');
  return out;
}

std::string Quotes::text(int r, int c) const {
  const Instrument & i = at(r);
  switch (c) {
    case BASE: return i.base;
    case QUOTE: return i.quote;
    case NAME: return i.name;
    case PRICE: return fixed(i.price, 2);
    case CHANGE: return fixed(i.change, 2, true) + "%";
    default: return "";
  }
}

std::optional<cellui::Color> Quotes::color(int r, int c) const {
  const Instrument & i = at(r);
  bool up;
  if (c == CHANGE) up = i.change >= 0;
  else if (c == HISTORY && !i.history.empty()) up = i.history.back() >= i.history.front();
  else return std::nullopt;
  return up ? cellui::Color(0x3f, 0xb9, 0x50) : cellui::Color(0xf8, 0x51, 0x49);
}

int Quotes::compare(int column, int a, int b) const {
  auto by = [](double x, double y) { return x < y ? -1 : y < x ? 1 : 0; };
  if (column == PRICE) return by(at(a).price, at(b).price);
  if (column == CHANGE) return by(at(a).change, at(b).change);
  return TableModel::compare(column, a, b);
}
