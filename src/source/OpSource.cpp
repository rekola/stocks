#include "source/OpSource.h"

#include "model/Isin.h"

#include <regex>
#include <thread>

using nlohmann::json;

namespace {

const std::string kPage = "https://www.op.fi/henkiloasiakkaat/saastot-ja-sijoitukset/rahastot/kaikki-rahastot";

// What the endpoints expect of a browser.
const HttpHeaders kHeaders = {{"Referer", "https://www.op.fi"}, {"Origin", "https://www.op.fi/"}};

// Funds priced monthly or quarterly: a daily series would have nothing to show.
const char * const kLessOften[] = {"FI4000439971", "FI4000108436", "FI4000281738", "FI4000530860", "FI4000060934", "FI4000306774"};

std::string text(const json & o, const char * key) {
  auto it = o.find(key);
  return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// The token without the `&` or `?` that may start it, so that it joins to a query with one `&`.
std::string trimmed(std::string token) {
  size_t from = 0;
  while (from < token.size() && (token[from] == '&' || token[from] == '?')) from++;
  return token.substr(from);
}

}  // namespace

bool OpSource::pricedLessOften(const std::string & isin) {
  for (const char * i : kLessOften)
    if (isin == i) return true;
  return false;
}

bool OpSource::parseToken(const std::string & page, std::string & token, std::string & error) {
  static const std::regex re("window\\.sessionStorage\\.setItem\\(\"data_token\", \"([^\"]*)\"\\);");
  std::smatch m;
  if (!std::regex_search(page, m, re) || trimmed(m[1].str()).empty()) {
    error = "no data token on the page";
    return false;
  }
  token = trimmed(m[1].str());
  return true;
}

std::string OpSource::listUrl(const std::string & token) {
  return "https://marketdata.op.fi/portal2/fundlist-json.php?" + token +
         "&y_rahastot=-1&group=all&region=all&tab=today&risk=1,7&tab=today&sort=fund_name%20asc";
}

std::string OpSource::chartUrl(const std::string & notation, const std::string & token) {
  return "https://marketdata.op.fi/portal2/charts//fundchartdata.php?ID_NOTATION=" + notation + "&type=line&" + token;
}

bool OpSource::parseList(const std::string & body, std::vector<Product> & products, std::vector<std::string> & unnamed,
                         std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("inst") || !doc["inst"].is_array()) {
    error = "not a fund list";
    return false;
  }
  for (const json & i : doc["inst"]) {
    if (!i.is_object()) continue;
    Product p;
    p.fund.isin = text(i, "isin");
    p.fund.name = text(i, "name");
    // The notation is a number in the answer or a string of one.
    if (auto it = i.find("id_notation"); it != i.end()) {
      if (it->is_string()) p.notation = it->get<std::string>();
      else if (it->is_number_integer()) p.notation = std::to_string(it->get<int64_t>());
    }
    if (p.notation.empty()) continue;
    if (!isValidIsin(p.fund.isin)) {
      unnamed.push_back(p.fund.name.empty() ? "notation " + p.notation : p.fund.name);
      continue;
    }
    if (pricedLessOften(p.fund.isin)) continue;
    // The page is a path on the bank's site in the answer.
    if (std::string link = text(i, "link"); !link.empty())
      p.fund.url = link.rfind("http", 0) == 0 ? link : "https://www.op.fi" + (link[0] == '/' ? link : "/" + link);
    p.fund.symbol = p.fund.isin;
    p.fund.identifier = p.notation;
    p.fund.managementFee = looseNumber(i, "management_fee");
    p.fund.subscriptionFee = looseNumber(i, "subs_fee");
    p.fund.redemptionFee = looseNumber(i, "redemption_fee");
    p.fund.risk = looseInteger(i, "risk_level");
    p.fund.rating = looseInteger(i, "morningstar");
    p.fund.esg = looseInteger(i, "esg_rating");
    products.push_back(std::move(p));
  }
  return true;
}

void OpSource::load(Store & store, Http & http, SourceListener & listener) {
  auto failure = [](const HttpResult & r) {
    return r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status);
  };
  std::string error, token;
  HttpResult page = http.get(kPage, kHeaders);
  if (!page.ok()) {
    listener.message("op: the funds page: " + failure(page));
    return;
  }
  if (!parseToken(page.body, token, error)) {
    listener.message("op: " + error);
    return;
  }
  std::this_thread::sleep_for(pause_);
  HttpResult list = http.get(listUrl(token), kHeaders);
  if (!list.ok()) {
    listener.message("op: the fund list: " + failure(list));
    return;
  }
  std::vector<Product> products;
  std::vector<std::string> unnamed;
  if (!parseList(list.body, products, unnamed, error)) {
    listener.message("op: the fund list: " + error);
    return;
  }
  for (const std::string & u : unnamed) listener.message("op: no ISIN for " + u + ", left out");
  size_t loaded = 0, skippedSparse = 0;
  for (const Product & p : products) {
    if (listener.cancelled()) return;
    std::this_thread::sleep_for(pause_);
    HttpResult r = http.get(chartUrl(p.notation, token), kHeaders);
    if (r.status == 429 || r.status == 403) {
      listener.message("op: refused, stopping");
      return;
    }
    std::vector<Candle> bars;
    if (!r.ok()) listener.message("op: " + p.fund.isin + ": " + failure(r));
    else if (!parseNavSeries(r.body, bars, error)) listener.message("op: " + p.fund.isin + ": " + error);
    else if (tooSparse(bars)) skippedSparse++;
    else if (writeFund(store, "op", "OP Funds", p.fund, bars)) {
      loaded++;
      listener.changed();
    }
  }
  listener.message("op: " + std::to_string(loaded) + " of " + std::to_string(products.size()) + " funds" + (skippedSparse ? ", " + std::to_string(skippedSparse) + " left out for pricing less often than daily" : std::string()));
}
