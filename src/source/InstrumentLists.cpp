#include "source/InstrumentLists.h"

#include "source/KnownMarkets.h"

#include <algorithm>
#include <fstream>

namespace {

// The fields of one CSV line: split at commas, a field may be in double quotes ("" is a quote inside one).
std::vector<std::string> fields(const std::string & line) {
  std::vector<std::string> out(1);
  bool quoted = false;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
        out.back() += '"';
        i++;
      } else if (c == '"') {
        quoted = false;
      } else {
        out.back() += c;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      out.emplace_back();
    } else {
      out.back() += c;
    }
  }
  return out;
}

std::string trimmed(std::string s) {
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
  while (!s.empty() && space(s.back())) s.pop_back();
  size_t i = 0;
  while (i < s.size() && space(s[i])) i++;
  return s.substr(i);
}

bool parseKind(const std::string & s, InstrumentKind & kind) {
  static const std::pair<const char *, InstrumentKind> kinds[] = {
    {"company", InstrumentKind::Company}, {"currency", InstrumentKind::Currency},
    {"commodity", InstrumentKind::Commodity}, {"index", InstrumentKind::Index}, {"etf", InstrumentKind::Etf},
    {"certificate", InstrumentKind::Certificate}, {"fund", InstrumentKind::Fund}, {"other", InstrumentKind::Other}};
  for (const auto & [name, k] : kinds)
    if (s == name) {
      kind = k;
      return true;
    }
  return false;
}

bool loadFile(const std::filesystem::path & path, InstrumentList & out, std::string & error) {
  std::ifstream in(path);
  if (!in) {
    error = path.string() + ": cannot be read";
    return false;
  }
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    number++;
    line = trimmed(line);
    if (line.empty()) continue;
    const std::string where = path.string() + ":" + std::to_string(number) + ": ";
    if (line[0] == '#') {
      std::string comment = trimmed(line.substr(1));
      continue;
    }
    std::vector<std::string> f = fields(line);
    if ((f.size() == 6 || f.size() == 7) && f[0] == "kind" && f[1] == "base") continue;  // the header
    if (f.size() != 6 && f.size() != 7) {
      error = where + "expected 6 or 7 fields (kind,base,quote,name,yahoo,isin[,mic]), found " + std::to_string(f.size());
      return false;
    }
    ListEntry e;
    for (std::string & s : f) s = trimmed(s);
    if (!parseKind(f[0], e.kind)) {
      error = where + "unknown kind '" + f[0] + "'";
      return false;
    }
    if (f[1].empty() || f[2].empty() || f[4].empty()) {
      error = where + "base, quote and yahoo cannot be empty";
      return false;
    }
    if (f.size() == 7) e.mic = f[6];
    if (!e.mic.empty()) {
      const char * name = marketNameOfMic(e.mic);
      if (!name) {
        error = where + "unknown market code '" + e.mic + "'";
        return false;
      }
      e.market = name;
    } else {
      e.mic =
          kNoMarket; // an index, a currency pair or a spot price: on no market
      e.market = marketNameOfMic(e.mic);
    }
    e.base = f[1];
    e.quote = f[2];
    e.name = f[3];
    e.yahoo = f[4];
    e.isin = f[5];
    if (e.isin.empty() && e.kind != InstrumentKind::Currency) {
      error = where + "'" + e.base + "' has no ISIN (only a currency is without)";
      return false;
    }
    out.push_back(std::move(e));
  }
  return true;
}

}  // namespace

InstrumentList loadInstrumentLists(const std::filesystem::path & directory, std::string & error) {
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    error = directory.string() + ": not a directory";
    return {};
  }
  std::vector<std::filesystem::path> files;
  for (const auto & entry : std::filesystem::directory_iterator(directory, ec))
    if (entry.is_regular_file() && entry.path().extension() == ".csv") files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  InstrumentList all;
  for (const auto & file : files) {
    if (!loadFile(file, all, error)) return {};
  }
  if (all.empty()) error = directory.string() + ": no instruments (*.csv)";
  return all;
}

InstrumentList onlyMarkets(const InstrumentList & list, const std::vector<std::string> & markets) {
  if (markets.empty()) return list;
  InstrumentList out;
  for (const ListEntry & e : list)
    if (std::find(markets.begin(), markets.end(), e.mic) != markets.end())
      out.push_back(e);
  return out;
}

std::filesystem::path findListsDirectory(const std::string & given) {
  std::error_code ec;
  if (!given.empty()) return given;
  for (const std::filesystem::path & candidate : {std::filesystem::path("data") / "lists",
                                                    std::filesystem::path(STOCKS_DATA_DIR) / "lists"})
    if (std::filesystem::is_directory(candidate, ec)) return candidate;
  return {};
}

InstrumentList resolveList(const ListSpec & spec, std::string & error) {
  std::filesystem::path directory = findListsDirectory(spec.directory);
  if (directory.empty()) {
    error = "no instrument lists (data/lists); the lists option names a directory";
    return {};
  }
  InstrumentList list = loadInstrumentLists(directory, error);
  if (list.empty()) return {};
  list = onlyMarkets(list, spec.markets);
  if (list.empty()) error = "no instruments in the markets asked for";
  return list;
}

const char * currencyName(const std::string & symbol) {
  static const std::pair<const char *, const char *> names[] = {
    {"USD", "US dollar"}, {"EUR", "Euro"}, {"GBP", "British pound"}, {"JPY", "Japanese yen"}, {"CHF", "Swiss franc"},
    {"AUD", "Australian dollar"}, {"NZD", "New Zealand dollar"}, {"CAD", "Canadian dollar"},
    {"SEK", "Swedish krona"}, {"NOK", "Norwegian krone"}, {"DKK", "Danish krone"}, {"PLN", "Polish zloty"},
    {"CZK", "Czech koruna"}, {"HUF", "Hungarian forint"}, {"RON", "Romanian leu"}, {"TRY", "Turkish lira"},
    {"ZAR", "South African rand"}, {"MXN", "Mexican peso"}, {"BRL", "Brazilian real"}, {"INR", "Indian rupee"},
    {"CNY", "Chinese yuan"}, {"HKD", "Hong Kong dollar"}, {"SGD", "Singapore dollar"}, {"KRW", "South Korean won"},
    {"THB", "Thai baht"}, {"ILS", "Israeli shekel"}, {"AED", "UAE dirham"}, {"SAR", "Saudi riyal"},
    {"IDR", "Indonesian rupiah"}, {"PHP", "Philippine peso"}, {"MYR", "Malaysian ringgit"},
    {"TWD", "New Taiwan dollar"}, {"CLP", "Chilean peso"}, {"COP", "Colombian peso"}, {"ARS", "Argentine peso"},
    {"ISK", "Icelandic krona"}, {"BGN", "Bulgarian lev"}};
  for (const auto & [s, n] : names)
    if (symbol == s) return n;
  return "";
}

InstrumentId currencyNode(Batch & store, const std::string & symbol) {
  if (auto known = store.view().findInstrument(symbol)) return *known;
  const char * label = currencyName(symbol);
  return store.addInstrument(symbol, *label ? label : symbol, InstrumentKind::Currency);
}

InstrumentId addInstrumentUnlessSaid(Batch & store, const std::string & symbol, const std::string & name, InstrumentKind kind,
                                     const std::string & isin, const std::string & type) {
  std::string sayName = name, sayType = type;
  if (!isin.empty())
    if (auto known = store.view().findInstrumentByIsin(isin)) {
      if (!store.view().said(NodeRef(*known), predicate::name).empty()) sayName.clear();
      if (!store.view().said(NodeRef(*known), predicate::type).empty()) sayType.clear();
    }
  return store.addInstrument(symbol, sayName, kind, isin, sayType);
}

InstrumentId quoteNode(Batch & store, const std::string & code) {
  if (code != "pts") return currencyNode(store, code);
  if (auto known = store.view().findInstrument(code)) return *known;
  return store.addInstrument(code, "Index points", InstrumentKind::Unit);
}

InstrumentId currencyNode(Store & store, const std::string & symbol) {
  InstrumentId id;
  store.batch([&](Batch & b) { id = currencyNode(b, symbol); });
  return id;
}

InstrumentId quoteNode(Store & store, const std::string & code) {
  InstrumentId id;
  store.batch([&](Batch & b) { id = quoteNode(b, code); });
  return id;
}
