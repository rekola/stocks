#include "source/AktiaSource.h"

#include <regex>
#include <thread>

using nlohmann::json;

namespace {

const std::string kBase = "https://misc.aktia.fi/data-service/investment/";

// Product id to ISIN. An empty ISIN: not wanted (closed, joined to another fund, or priced less often than daily).
struct Known {
  const char * productId;
  const char * isin;
};
const Known kKnown[] = {
    {"ROBURSVERIGEA", "SE0000996233"}, {"AKTNORDICHYJB", "FI4000104773"}, {"AKTEUKASSAKONEJB", "FI4000354261"},
    {"AKTARVOREINJB", "FI4000340146"}, {"AKTIALIKVI", "FI0008804943"}, {"AKTEURHYB", "FI4000400056"},
    {"AKTEMFRONTB", "FI4000185459"}, {"AKTEMLCB", "FI0008813555"}, {"AKTEMERB", "FI0008811708"},
    {"AKTCORBONB", "FI0008803853"}, {"ROBURROVAR", "SE0000538969"}, {"ROBURCONTU", "SE0000538944"},
    {"ROBURJAPAN", "SE0000539413"}, {"ROBURPACIF", "SE0000539447"}, {"ODINSVERIGE", "NO0010924772"},
    {"ODINNORGE", "NO0010924053"}, {"HQINDIEN", "SE0001209834"}, {"PARINDIA", "LU0823428346"},
    {"PAREQWRLDEME", "LU0823413074"}, {"PARCHINA", "LU0823425839"}, {"PAREQASIA", "LU0823397368"},
    {"AKTNORSCAB", "FI4000029632"}, {"AKTNORDMCB", "FI4000223565"}, {"AKTNORDICB", "FI0008810569"},
    {"AKTEURSCAB", "FI4000122999"}, {"AKTIAEUROB", "FI0008804844"}, {"AKTVARH50B", "FI4000297817"},
    {"AKTVARH25B", "FI4000297791"}, {"AKTIOSAKEB", "FI4000270269"}, {"AKTABAB", "FI0008811799"},
    {"AKTEURSMAB", "FI4000583109"}, {"AKTPROSPB", "FI4000582259"},
    // Not wanted. Nothing to load: no ISIN known, or closed, or joined into another fund.
    {"AKTPASEURB", ""}, {"AKTPASEMEB", ""}, {"AKTPASUSAB", ""}, {"AKTPASJAPB", ""}, {"PBSIJKORI5", ""}, {"KSSIJKORI5", ""},
    {"KSSIJKORI1", ""}, {"PBSIJKORI1", ""}, {"AKTVARAMALTIB", ""}, {"AKTVARATASAB", ""}, {"AKTVAIHTB", ""},
    {"AKTVARATUOTTOB", ""}, {"ASUNNOTKORI", ""}, {"TTKKORI", ""}, {"IMPKORI", ""}, {"AKTMIKROREINJB", ""},
    {"AKTMIKROMARKKAJB", ""}, {"AKTASUNNOTAO", ""}, {"AKTIMPAKTIB", ""}, {"SIJKORI5", ""}, {"SIJKORI4", ""},
    {"SIJKORI3", ""}, {"SIJKORI2", ""}, {"SIJKORI1", ""}, {"KSSIJKORI4", ""}, {"KSSIJKORI3", ""}, {"KSSIJKORI2", ""},
    {"PBSIJKORI4", ""}, {"PBSIJKORI3", ""}, {"PBSIJKORI2", ""}, {"AKTGOVBONB", ""}, {"AKTIASOLIB", ""}, {"AKTIASECB", ""},
    {"AKTEMCORPB", ""},  // combined into Aktia Emerging Market Bond+ B
    {"AKTIAGLOBB", ""}, {"AKTEMESELEB", ""}, {"AKTIACAPB", ""}, {"AKTIAMERIB", ""},
    {"AKTTTKIINA", ""},  // Aktia Toimitilakiinteistöt, priced quarterly
    {"AKTVARH75B", ""},
    // Russia: closed to trading.
    {"ROBUROSTEU", ""}, {"HQRYSSLAND", ""}, {"ROBURYSSLA", ""},
    {"AKTVAKAAKORKOB", ""},  // joined
    {"AURINKOTUULI3", ""},   // Aktia Aurinkotuuli III Ky
};

}  // namespace

std::optional<std::string> AktiaSource::isinOf(const std::string & productId, bool & known) {
  static const std::regex luxembourg("^(LU\\d{10})EUR$");  // these carry their ISIN
  std::smatch m;
  known = true;
  if (std::regex_match(productId, m, luxembourg)) return m[1].str();
  for (const Known & k : kKnown)
    if (productId == k.productId) return *k.isin ? std::optional<std::string>(k.isin) : std::nullopt;
  known = false;
  return std::nullopt;
}

bool AktiaSource::parseList(const std::string & body, std::vector<Product> & products, std::vector<std::string> & unknown,
                            std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_array()) {
    error = "not a product list";
    return false;
  }
  for (const json & i : doc) {
    if (!i.is_object() || !i.contains("productId") || !i["productId"].is_string()) continue;
    const std::string id = i["productId"].get<std::string>();
    bool known = false;
    std::optional<std::string> isin = isinOf(id, known);
    if (!known) {
      unknown.push_back(id + " (" + (i.contains("productNameFi") && i["productNameFi"].is_string() ? i["productNameFi"].get<std::string>() : "") + ")");
      continue;
    }
    if (!isin) continue;
    Product p;
    p.productId = id;
    p.fund.isin = *isin;
    p.fund.symbol = id;
    p.fund.identifier = id;
    p.fund.name = i.contains("productNameEn") && i["productNameEn"].is_string() ? i["productNameEn"].get<std::string>() : id;
    if (auto costs = looseNumber(i, "runningCosts")) p.fund.managementFee = *costs * 100;  // a fraction in the answer
    p.fund.risk = looseInteger(i, "riskClass");
    p.fund.rating = looseInteger(i, "morningstarRating");
    products.push_back(std::move(p));
  }
  return true;
}

bool AktiaSource::parseFees(const std::string & body, FundRow & fund, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("values") || !doc["values"].is_object()) {
    error = "not a product description";
    return false;
  }
  const json & values = doc["values"];
  // The fee for an online order where there is one, else the ordinary fee.
  auto fee = [&](const char * online, const char * ordinary) -> std::optional<double> {
    for (const char * key : {online, ordinary}) {
      auto it = values.find(key);
      if (it != values.end() && it->is_object())
        if (auto v = looseNumber(*it, "value")) return v;
    }
    return std::nullopt;
  };
  fund.redemptionFee = fee("onlineRedemptionFee", "redemptionFee");
  fund.subscriptionFee = fee("onlineSubscriptionFee", "subscriptionFee");
  return true;
}

void AktiaSource::load(Store & store, Http & http, SourceListener & listener) {
  auto failure = [](const HttpResult & r) {
    return r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status);
  };
  HttpResult list = http.get(kBase + "formatted/product-list/fund");
  if (!list.ok()) {
    listener.message("aktia: the fund list: " + failure(list));
    return;
  }
  std::vector<Product> products;
  std::vector<std::string> unknown;
  std::string error;
  if (!parseList(list.body, products, unknown, error)) {
    listener.message("aktia: the fund list: " + error);
    return;
  }
  for (const std::string & u : unknown) listener.message("aktia: no ISIN for " + u + ", left out");
  size_t loaded = 0, skippedSparse = 0;
  for (Product & p : products) {
    if (listener.cancelled()) return;
    // The fees are asked for once.
    if (auto have = fees_.find(p.productId); have != fees_.end()) {
      p.fund.subscriptionFee = have->second.subscriptionFee;
      p.fund.redemptionFee = have->second.redemptionFee;
    } else {
      std::this_thread::sleep_for(pause_);
      HttpResult r = http.get(kBase + "formatted/product-base-details/" + p.productId);
      if (r.status == 429 || r.status == 403) {
        listener.message("aktia: refused, stopping");
        return;
      }
      if (!r.ok()) listener.message("aktia: " + p.productId + " fees: " + failure(r));
      else if (!parseFees(r.body, p.fund, error)) listener.message("aktia: " + p.productId + " fees: " + error);
      else fees_[p.productId] = p.fund;
    }
    std::this_thread::sleep_for(pause_);
    HttpResult r = http.get(kBase + "base/rates/" + p.productId);
    if (r.status == 429 || r.status == 403) {
      listener.message("aktia: refused, stopping");
      return;
    }
    std::vector<Candle> bars;
    if (!r.ok()) listener.message("aktia: " + p.productId + ": " + failure(r));
    else if (!parseNavSeries(r.body, bars, error)) listener.message("aktia: " + p.productId + ": " + error);
    else if (tooSparse(bars)) skippedSparse++;
    else if (writeFund(store, "aktia", "Aktia Funds", p.fund, bars)) {
      loaded++;
      listener.changed();
    }
  }
  listener.message("aktia: " + std::to_string(loaded) + " of " + std::to_string(products.size()) + " funds" + (skippedSparse ? ", " + std::to_string(skippedSparse) + " left out for pricing less often than daily" : std::string()));
}
