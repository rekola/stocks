#include "source/NordnetDetails.h"
#include "source/DatasetCodes.h"

#include "model/Dates.h"
#include "source/Countries.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <thread>

using nlohmann::json;

namespace {

// The details answer to a browser's headers and a Nordic locale only (the descriptions come in that language).
const HttpHeaders kHeaders = {{"Accept", "*/*"},
                              {"Accept-Language", "en-US,en;q=0.9,fi;q=0.8"},
                              {"Origin", "https://www.nordnet.fi"},
                              {"Referer", "https://www.nordnet.fi/"},
                              {"x-locale", "fi-FI"}};

const json * member(const json & o, const char * key) {
  if (!o.is_object()) return nullptr;
  auto it = o.find(key);
  return it == o.end() || it->is_null() ? nullptr : &*it;
}

// The value at a path of members ("tradingInfo.cutOffTime"), if there is one.
const json * at(const json & o, std::initializer_list<const char *> path) {
  const json * v = &o;
  for (const char * key : path) {
    v = member(*v, key);
    if (!v) return nullptr;
  }
  return v;
}

void text(const json & o, std::initializer_list<const char *> path, const char * predicate, std::vector<Attribute> & out) {
  const json * v = at(o, path);
  if (v && v->is_string() && !v->get<std::string>().empty()) out.push_back({predicate, v->get<std::string>()});
}

void real(const json & o, std::initializer_list<const char *> path, const char * predicate, std::vector<Attribute> & out) {
  const json * v = at(o, path);
  if (v && v->is_number()) out.push_back({predicate, v->get<double>()});
}

void integer(const json & o, std::initializer_list<const char *> path, const char * predicate, std::vector<Attribute> & out) {
  const json * v = at(o, path);
  if (v && v->is_number()) out.push_back({predicate, static_cast<int64_t>(v->get<double>())});
}

void flag(const json & o, std::initializer_list<const char *> path, const char * predicate, std::vector<Attribute> & out) {
  const json * v = at(o, path);
  if (v && v->is_boolean()) out.push_back({predicate, v->get<bool>()});
}

// "1992-04-27" as a day number.
void day(const json & o, std::initializer_list<const char *> path, const char * predicate, std::vector<Attribute> & out) {
  const json * v = at(o, path);
  if (!v || !v->is_string()) return;
  int y = 0;
  unsigned m = 0, d = 0;
  if (std::sscanf(v->get<std::string>().c_str(), "%d-%u-%u", &y, &m, &d) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31)
    out.push_back({predicate, dayNumber(y, m, d)});
}

// The risk statistics of a period of a fund: the figures of `object[period]`.
void statistics(const json & object, const char * period, const char * suffix, std::vector<Attribute> & out) {
  const json * stats = member(object, period);
  if (!stats) return;
  static const struct {
    const char * key;
    const char * name;
  } kFigures[] = {{"alpha", "alpha-"},        {"beta", "beta-"},
                  {"r2", "r2-"},              {"sharpeRatio", "sharpe-"},
                  {"standardDeviation", "deviation-"}, {"trackingError", "tracking-error-"}};
  for (const auto & f : kFigures) {
    const json * v = member(*stats, f.key);
    if (v && v->is_number()) out.push_back({std::string(f.name) + suffix, v->get<double>()});
  }
}

// The current value of a company's key figure, by its type.
std::optional<double> keyFigure(const json & stock, const char * type) {
  const json * list = member(stock, "keyFiguresPreviews");
  if (!list || !list->is_array()) return std::nullopt;
  for (const json & f : *list) {
    const json * t = member(f, "keyFigureType");
    const json * v = member(f, "currentValue");
    if (t && t->is_string() && t->get<std::string>() == type && v && v->is_number()) return v->get<double>();
  }
  return std::nullopt;
}

}  // namespace

std::string NordnetDetails::stockUrl(const std::string & orderBook) {
  return "https://api.prod.nntech.io/instrument-screening/v3/stocks/web/" + orderBook;
}

std::string NordnetDetails::fundUrl(const std::string & orderBook) {
  return "https://api.prod.nntech.io/instrument/v2/funds/" + orderBook;
}

bool NordnetDetails::parseStock(const std::string & body, std::vector<Attribute> & said, std::string & error) {
  json j = json::parse(body, nullptr, false);
  if (!j.is_object() || !member(j, "companyProfile")) {
    error = "not a stock's page";
    return false;
  }
  const json & p = j["companyProfile"];
  text(p, {"companyIdentity", "businessDescription"}, "description", said);
  text(p, {"companyIdentity", "websiteUrl"}, "company-website", said);
  if (const json * name = at(p, {"address", "country"}); name && name->is_string())
    if (auto code = countryCodeOfFinnishName(name->get<std::string>())) said.push_back({predicate::country, *code});
  text(p, {"classification", "economy", "economy"}, "economy", said);
  text(p, {"classification", "sector", "sector"}, "sector", said);
  text(p, {"classification", "industry", "industry"}, "industry", said);
  if (member(p, "marketInfo") && member(p["marketInfo"], "publicShares")) integer(p, {"marketInfo", "publicShares"}, "shares-outstanding", said);
  else integer(p, {"marketInfo", "commonSharesOutstandingBasic"}, "shares-outstanding", said);
  integer(p, {"marketInfo", "commonSharesOutstandingDiluted"}, "shares-diluted", said);
  if (auto v = keyFigure(j, "REVENUE")) said.push_back({"revenue", *v});
  if (auto v = keyFigure(j, "EBIT")) said.push_back({"ebit", *v});
  return true;
}

bool NordnetDetails::parseFund(const std::string & body, std::vector<Attribute> & said, std::string & error) {
  json j = json::parse(body, nullptr, false);
  if (!j.is_object() || !member(j, "isin")) {
    error = "not a fund's page";
    return false;
  }
  text(j, {"description"}, "description", said);
  text(j, {"tradingInfo", "cutOffTime"}, "fund-cut-off", said);  // the time of day, as given ("10:00:00")
  // The Morningstar figures say what the lists say, under the same names.
  integer(j, {"morningstarValues", "rating", "value"}, "morningstar-rating", said);
  integer(j, {"morningstarValues", "risk", "value"}, "risk-class", said);
  real(j, {"fees", "managementFee"}, "management-fee", said);
  real(j, {"fees", "ongoingCost"}, "ongoing-cost", said);
  real(j, {"fees", "totalFee"}, "total-fee", said);
  text(j, {"assetClass"}, "asset-class", said);
  text(j, {"baseCurrency"}, "base-currency", said);
  text(j, {"dividend", "distributionType"}, "distribution", said);
  text(j, {"fundCompany", "name"}, "admin-company", said);
  text(j, {"fundCompany", "homepage"}, "admin-company-website", said);
  day(j, {"inceptionDate"}, "start-day", said);
  flag(j, {"ucits"}, "ucits", said);
  real(j, {"tradingInfo", "minInvestment", "value"}, "min-investment", said);
  if (const json * stats = member(j, "risksStatistics")) {
    statistics(*stats, "oneYear", "1y", said);
    statistics(*stats, "threeYears", "3y", said);
    statistics(*stats, "fiveYears", "5y", said);
  }
  return true;
}

bool NordnetDetails::loadBatch(Store & store, Http & http, SourceListener & listener, std::chrono::milliseconds pause, size_t batch) {
  struct Target {
    InstrumentId id;
    std::string orderBook;
    bool fund;
  };
  std::vector<Target> targets;
  {
    auto snapshot = store.snapshot();
    auto dataset = snapshot->findDataset(kNordnetDataset);
    if (!dataset) return false;
    std::set<uint64_t> seen;
    for (MarketId market : snapshot->edgeMarkets()) {
      for (const EdgeKey & edge : snapshot->edges(market)) {
        if (edge.dataset != *dataset || tried_.count(edge.base.value) || !seen.insert(edge.base.value).second) continue;
        auto listing = snapshot->listing(edge);
        auto instrument = snapshot->instrument(edge.base);
        if (!listing || listing->orderBook.empty() || !instrument) continue;
        if (snapshot->attribute(edge.base, "description")) continue;  // asked for in an earlier run
        bool fund = instrument->kind == InstrumentKind::Fund || instrument->kind == InstrumentKind::Etf;
        if (!fund && instrument->kind != InstrumentKind::Company) continue;
        targets.push_back({edge.base, listing->orderBook, fund});
      }
    }
  }
  size_t done = 0;
  for (const Target & t : targets) {
    if (listener.cancelled()) return true;
    if (done >= batch) return true;
    tried_.insert(t.id.value);
    done++;
    HttpResult r = http.get(t.fund ? fundUrl(t.orderBook) : stockUrl(t.orderBook), kHeaders);
    std::vector<Attribute> said;
    std::string error;
    if (!r.ok()) {
      if (r.status != 404) tried_.erase(t.id.value);  // a failure of the connection is tried again at the next load
    } else if (!(t.fund ? parseFund(r.body, said, error) : parseStock(r.body, said, error))) {
      listener.message("nordnet details: " + error);
    } else if (!said.empty()) {
      store.setAttributes(t.id, said);
      listener.changed();
    }
    if (pause.count() > 0) std::this_thread::sleep_for(pause);
  }
  return false;
}
