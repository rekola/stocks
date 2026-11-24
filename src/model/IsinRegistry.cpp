#include "model/IsinRegistry.h"

#include "model/Isin.h"

#include <map>
#include <sstream>

bool IsinRegistry::parse(const std::string & text, IsinRegistry & out, std::string & error) {
  out = {};
  std::istringstream in(text);
  std::string line;
  int n = 0;
  std::map<std::string, int> isins, symbols, names;  // to the line that has them
  while (std::getline(in, line)) {
    n++;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const std::string where = "line " + std::to_string(n) + ": ";
    Entry e;
    e.line = n;
    size_t from = 0;
    std::string * parts[] = {&e.isin, &e.symbol, &e.name};
    bool ok = true;
    for (std::string * part : parts) {
      const size_t comma = line.find(',', from);
      if (comma == std::string::npos) {
        ok = false;
        break;
      }
      *part = line.substr(from, comma - from);
      from = comma + 1;
    }
    if (!ok) {
      error = where + "a row is isin,symbol,name,note";
      return false;
    }
    e.note = line.substr(from);
    if (!isValidIsin(e.isin)) {
      const char check = e.isin.size() == 12 ? isinCheckDigit(e.isin.substr(0, 11)) : '\0';
      error = where + e.isin + " is not a valid ISIN" + (check ? std::string(" (its check digit should be ") + check + ")" : std::string());
      return false;
    }
    if (e.symbol.empty() && e.name.empty()) {
      error = where + "a row names nothing: it needs a symbol or a name";
      return false;
    }
    // A symbol is looked for before a name, so a symbol that is another row's name (or a name that is another row's symbol) would be found
    // for the wrong thing.
    if (!e.symbol.empty() && names.count(e.symbol)) {
      error = where + "the symbol " + e.symbol + " is the name of the row at line " + std::to_string(names[e.symbol]);
      return false;
    }
    if (!e.name.empty() && symbols.count(e.name)) {
      error = where + "the name " + e.name + " is the symbol of the row at line " + std::to_string(symbols[e.name]);
      return false;
    }
    auto collide = [&](std::map<std::string, int> & seen, const std::string & key, const char * what) {
      if (key.empty()) return false;
      const auto [it, fresh] = seen.emplace(key, n);
      if (fresh) return false;
      error = where + std::string(what) + " " + key + " is already in the registry, at line " + std::to_string(it->second);
      return true;
    };
    if (collide(isins, e.isin, "the ISIN") || collide(symbols, e.symbol, "the symbol") || collide(names, e.name, "the name")) return false;
    out.entries_.push_back(std::move(e));
  }
  return true;
}

std::string IsinRegistry::find(const std::string & symbol, const std::string & name) const {
  if (!symbol.empty())
    for (const Entry & e : entries_)
      if (e.symbol == symbol) return e.isin;
  if (!name.empty())
    for (const Entry & e : entries_)
      if (e.name == name) return e.isin;
  return {};
}
