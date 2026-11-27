#include "source/FundSource.h"

#include "model/Dates.h"
#include "model/Isin.h"
#include "model/Period.h"
#include "source/DataDirectory.h"
#include "source/LeftOut.h"
#include "model/NodeKey.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using nlohmann::json;

namespace {

// The value at a JSON pointer, or null where there is none or the pointer is not one.
const json * valueAt(const json & doc, const std::string & pointer) {
  try {
    const json::json_pointer p(pointer);
    if (doc.contains(p)) {
      const json & v = doc.at(p);
      if (!v.is_null()) return &v;
    }
  } catch (const json::exception &) {
  }
  return nullptr;
}

const json * firstOf(const json & doc, const FundSchema::Pointers & pointers) {
  for (const std::string & p : pointers)
    if (const json * v = valueAt(doc, p)) return v;
  return nullptr;
}

std::string textOf(const json & v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
  return {};
}

// The first string anywhere in `doc` that is an ISIN, and where it is.
std::optional<std::pair<std::string, std::string>> findIsin(const json & doc, const std::string & path = {}) {
  if (doc.is_string()) {
    const std::string s = doc.get<std::string>();
    if (s.size() == 12 && isValidIsin(s)) return std::make_pair(path.empty() ? std::string("/") : path, s);
  } else if (doc.is_object()) {
    for (const auto & [key, v] : doc.items())
      if (auto found = findIsin(v, path + "/" + key)) return found;
  } else if (doc.is_array()) {
    for (size_t i = 0; i < doc.size(); i++)
      if (auto found = findIsin(doc[i], path + "/" + std::to_string(i))) return found;
  }
  return std::nullopt;
}

std::string trimmed(const std::string & s) {
  const size_t from = s.find_first_not_of(" \t\r\n"), to = s.find_last_not_of(" \t\r\n");
  return from == std::string::npos ? std::string() : s.substr(from, to - from + 1);
}

std::string trimmedToken(const std::string & token) {
  size_t from = 0;
  while (from < token.size() && (token[from] == '&' || token[from] == '?')) from++;
  return token.substr(from);
}

bool pointersOf(const json & v, FundSchema::Pointers & out) {
  auto add = [&](const json & one) {
    if (!one.is_string()) return false;
    const std::string p = one.get<std::string>();
    if (!p.empty() && p[0] != '/') return false;
    try {
      json::json_pointer check(p);
    } catch (const json::exception &) {
      return false;
    }
    out.push_back(p);
    return true;
  };
  if (v.is_array()) {
    for (const json & one : v)
      if (!add(one)) return false;
    return !out.empty();
  }
  return add(v);
}

bool requestOf(const json & o, const char * what, FundSchema::Request & r, std::string & error) {
  if (!o.is_object() || !o.contains("url") || !o["url"].is_string()) {
    error = std::string(what) + ": no url";
    return false;
  }
  r.url = o["url"].get<std::string>();
  if (auto rows = o.find("rows"); rows != o.end()) {
    FundSchema::Pointers p;
    if (!rows->is_string() || !pointersOf(*rows, p)) {
      error = std::string(what) + ": rows is not a JSON pointer";
      return false;
    }
    r.rows = p[0];
  }
  if (auto prefix = o.find("linkPrefix"); prefix != o.end() && prefix->is_string()) r.linkPrefix = prefix->get<std::string>();
  if (auto fields = o.find("fields"); fields != o.end()) {
    if (!fields->is_object()) {
      error = std::string(what) + ": fields is not an object";
      return false;
    }
    for (const auto & [key, v] : fields->items()) {
      FundSchema::Pointers p;
      if (!pointersOf(v, p)) {
        error = std::string(what) + ": field " + key + " is not a JSON pointer or a list of them";
        return false;
      }
      r.fields[key] = std::move(p);
    }
  }
  if (auto attributes = o.find("attributes"); attributes != o.end()) {
    if (!attributes->is_object()) {
      error = std::string(what) + ": attributes is not an object";
      return false;
    }
    for (const auto & [key, v] : attributes->items()) {
      FundSchema::Pointers p;
      if (!pointersOf(v, p)) {
        error = std::string(what) + ": attribute " + key + " is not a JSON pointer or a list of them";
        return false;
      }
      r.attributes[key] = std::move(p);
    }
  }
  if (auto scale = o.find("scale"); scale != o.end() && scale->is_object())
    for (const auto & [key, v] : scale->items())
      if (v.is_number()) r.scale[key] = v.get<double>();
  return true;
}

// What `doc` says of the fund, as far as the request's fields reach: a field it does not find leaves what the fund has.
void applyFields(const FundSchema::Request & request, const json & doc, FundRow & fund, std::string & id) {
  for (const auto & [key, pointers] : request.fields) {
    const json * v = firstOf(doc, pointers);
    if (!v) continue;
    const auto setChange = [&](Period p, std::optional<double> pct) {
      if (pct) fund.changes.pct[static_cast<size_t>(p)] = *pct;
    };
    // A return per annum over `years` years, as the change over all of them.
    const auto setAnnual = [&](Period p, int years, std::optional<double> pct) {
      if (pct && *pct > -100) setChange(p, (std::pow(1 + *pct / 100, years) - 1) * 100);
    };
    auto scaled = [&](std::optional<double> n) -> std::optional<double> {
      auto s = request.scale.find(key);
      return n && s != request.scale.end() ? std::optional<double>(*n * s->second) : n;
    };
    if (key == "id") id = textOf(*v);
    else if (key == "symbol") fund.symbol = trimmed(textOf(*v));
    else if (key == "name") fund.name = trimmed(textOf(*v));
    else if (key == "isin") fund.isin = textOf(*v);
    else if (key == "risk") fund.risk = looseInteger(*v);
    else if (key == "rating") fund.rating = looseInteger(*v);
    else if (key == "esg") fund.esg = looseInteger(*v);
    else if (key == "managementFee") fund.managementFee = scaled(looseNumber(*v));
    else if (key == "subscriptionFee") fund.subscriptionFee = scaled(looseNumber(*v));
    else if (key == "redemptionFee") fund.redemptionFee = scaled(looseNumber(*v));
    else if (key == "price") fund.price = looseNumber(*v);
    else if (key == "change") fund.change = scaled(looseNumber(*v));
    else if (key == "date") fund.day = looseDay(*v);
    else if (key == "changeWeek") setChange(Period::Week, scaled(looseNumber(*v)));
    else if (key == "changeMonth") setChange(Period::Month, scaled(looseNumber(*v)));
    else if (key == "changeQuarter") setChange(Period::Quarter, scaled(looseNumber(*v)));
    else if (key == "changeYear") setChange(Period::Year, scaled(looseNumber(*v)));
    else if (key == "annualYear3") setAnnual(Period::Year3, 3, scaled(looseNumber(*v)));
    else if (key == "annualYear10") setAnnual(Period::Year10, 10, scaled(looseNumber(*v)));
    else if (key == "link") {
      std::string link = textOf(*v);
      if (!link.empty() && link.rfind("http", 0) != 0) link = request.linkPrefix + (link[0] == '/' ? link : "/" + link);
      fund.url = link;
    }
  }
  for (const auto & [predicate, pointers] : request.attributes) {
    const json * v = firstOf(doc, pointers);
    if (!v) continue;
    const bool day = predicate.size() > 4 && predicate.compare(predicate.size() - 4, 4, "-day") == 0;
    auto at = std::find_if(fund.extra.begin(), fund.extra.end(), [&](const Attribute & a) { return a.predicate == predicate; });
    std::optional<Attribute> said;
    if (day) {
      if (auto d = looseDay(*v)) said = Attribute(predicate, *d);
    } else if (v->is_string()) said = Attribute(predicate, trimmed(v->get<std::string>()));
    else if (v->is_boolean()) said = Attribute(predicate, v->get<bool>());
    else if (v->is_number_integer()) said = Attribute(predicate, v->get<int64_t>());
    else if (v->is_number()) said = Attribute(predicate, v->get<double>());
    if (!said) continue;
    if (at != fund.extra.end()) *at = *said;
    else fund.extra.push_back(*said);
  }
}

// Morningstar's price history of a fund, `{id}` being the fund's Morningstar id (its secId, "F00000JTC7"): the answer is `[[ms, NAV], ...]`.
// `client` is the company's key in Morningstar's URLs and `universe` its number there (the part after FOFIN$$ALL_ of the id Morningstar's own
// pages for the company's funds use).
std::string morningstarHistoryUrl(const std::string & client, const std::string & universe, const std::string & currency, const std::string & startDate) {
  return "https://lt.morningstar.com/api/rest.svc/timeseries_price/" + client + "?currencyId=" + currency +
         "&idtype=Morningstar&frequency=daily&startDate=" + startDate + "&priceType=&outputType=COMPACTJSON&id={id}]2]1]FOFIN$$ALL_" + universe +
         "&applyTrackRecordExtension=true";
}

bool plainWord(const std::string & s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '-'; });
}

// A GET as a browser makes it, the schema's headers over the browser's: the token page as a page, the rest as its script's calls.
HttpResult browserGet(Http & http, const std::string & url, const HttpHeaders & own, BrowserRequest kind = BrowserRequest::Fetch) {
  return http.get(url, browserHeaders(url, own, kind));
}

std::string failure(const HttpResult & r) {
  return r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status);
}

}  // namespace

bool FundSchema::parse(const std::string & text, FundSchema & out, std::string & error) {
  json d = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (d.is_discarded() || !d.is_object()) {
    error = "not JSON, or not an object";
    return false;
  }
  FundSchema s;
  for (auto [key, target] :
       {std::pair<const char *, std::string *>{"name", &s.name},
        {"dataset", &s.dataset}}) {
    if (!d.contains(key) || !d[key].is_string() || d[key].get<std::string>().empty()) {
      error = std::string("no ") + key;
      return false;
    }
    *target = d[key].get<std::string>();
  }
  if (!datasetKey(s.dataset)) {
    error = "dataset is the code of the dataset, one to eleven capitals, digits, hyphens, periods or hashes (it is never changed), not \"" + s.dataset + "\"";
    return false;
  }
  if (auto title = d.find("title"); title != d.end() && title->is_string()) s.title = title->get<std::string>();
  if (auto age = d.find("maxAgeDays"); age != d.end() && age->is_number_integer()) s.maxAgeDays = age->get<int>();
  if (auto pause = d.find("pauseMs"); pause != d.end() && pause->is_number()) s.pause = std::chrono::milliseconds(pause->get<int64_t>());
  if (auto hours = d.find("refreshHours"); hours != d.end() && hours->is_number())
    s.refresh = std::chrono::milliseconds(static_cast<int64_t>(hours->get<double>() * 3600 * 1000));
  if (auto headers = d.find("headers"); headers != d.end() && headers->is_object())
    for (const auto & [key, v] : headers->items())
      if (v.is_string()) s.headers.push_back({key, v.get<std::string>()});
  if (auto token = d.find("token"); token != d.end()) {
    if (!token->is_object() || !token->contains("page") || !token->contains("regex") || !(*token)["page"].is_string() || !(*token)["regex"].is_string()) {
      error = "token needs a page and a regex";
      return false;
    }
    s.tokenPage = (*token)["page"].get<std::string>();
    s.tokenRegex = (*token)["regex"].get<std::string>();
    try {
      std::regex check(s.tokenRegex);
    } catch (const std::regex_error &) {
      error = "token: the regex is not one";
      return false;
    }
  }
  if (!d.contains("list") || !requestOf(d["list"], "list", s.list, error)) {
    if (error.empty()) error = "no list";
    return false;
  }
  if (!s.list.fields.count("id")) {
    error = "list: the fields have no id";
    return false;
  }
  if (auto detail = d.find("detail"); detail != d.end()) {
    s.detail.emplace();
    if (!requestOf(*detail, "detail", *s.detail, error)) return false;
  }
  const json history = d.contains("history") ? d["history"] : json();
  if (history.is_object() && history.contains("morningstar")) {
    // The company's history is Morningstar's: the schema says whose, and the URL is made here, so that only this adapter knows the endpoint.
    const json & m = history["morningstar"];
    auto field = [&](const char * key, const char * otherwise) -> std::string {
      auto it = m.is_object() ? m.find(key) : json::const_iterator();
      return m.is_object() && it != m.end() && it->is_string() ? it->get<std::string>() : std::string(otherwise);
    };
    const std::string client = field("client", ""), universe = field("universe", ""), currency = field("currency", "EUR"), start = field("startDate", "2000-01-01");
    if (!plainWord(client) || !plainWord(universe) || !plainWord(currency) || !looseDay(json(start))) {
      error = "history.morningstar needs a client and a universe (letters and digits), and may have a currency and a startDate (YYYY-MM-DD)";
      return false;
    }
    s.historyUrl = morningstarHistoryUrl(client, universe, currency, start);
  } else if (!history.is_object() || !history.contains("url") || !history["url"].is_string()) {
    error = "history: no url";
    return false;
  } else {
    s.historyUrl = history["url"].get<std::string>();
  }
  if (auto from = d.find("isinFromId"); from != d.end() && from->is_string()) {
    s.isinFromId = from->get<std::string>();
    try {
      std::regex check(s.isinFromId);
    } catch (const std::regex_error &) {
      error = "isinFromId: not a regex";
      return false;
    }
  }
  if (auto isins = d.find("isins"); isins != d.end() && isins->is_object())
    for (const auto & [key, v] : isins->items())
      if (v.is_string()) s.isins[key] = v.get<std::string>();
  out = std::move(s);
  return true;
}

bool FundSchema::load(const std::string & nameOrPath, FundSchema & out, std::string & error) {
  // A name is a file of the schema directory (data/funds: here, beside the program, installed or in the source tree); anything with a path or an
  // extension is a file as it is.
  std::filesystem::path path = nameOrPath;
  std::error_code ec;
  if (nameOrPath.find_first_of("/\\") == std::string::npos && path.extension() != ".json") {
    path.clear();
    for (const std::filesystem::path & root : dataRoots(executableDirectory()))
      if (std::filesystem::is_regular_file(root / "funds" / (nameOrPath + ".json"), ec)) {
        path = root / "funds" / (nameOrPath + ".json");
        break;
      }
    if (path.empty()) {
      error = "there is no schema \"" + nameOrPath + "\" (data/funds/" + nameOrPath + ".json), nor a file of that name";
      return false;
    }
  }
  std::ifstream in(path);
  if (!in) {
    error = "cannot read the schema file " + path.string();
    return false;
  }
  std::stringstream text;
  text << in.rdbuf();
  if (!parse(text.str(), out, error)) {
    error = path.string() + ": " + error;
    return false;
  }
  return true;
}

std::string FundSource::expand(const std::string & url, const std::string & id, const std::string & token) {
  std::string out = url;
  for (const auto & [key, value] : {std::pair<std::string, const std::string &>{"{id}", id}, {"{token}", token}})
    for (size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + value.size())) out.replace(at, key.size(), value);
  return out;
}

bool FundSource::parseToken(const std::string & page, const std::string & regex, std::string & token, std::string & error) {
  std::smatch m;
  const std::regex re(regex);
  if (!std::regex_search(page, m, re) || m.size() < 2 || trimmedToken(m[1].str()).empty()) {
    error = "no token on the page";
    return false;
  }
  token = trimmedToken(m[1].str());  // without the `&` or `?` that may start it, so that it joins to a query with one `&`
  return true;
}

bool parseFundList(const FundSchema & schema_, const std::string & body, std::vector<FundEntry> & entries, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  const json * rows = doc.is_discarded() ? nullptr : valueAt(doc, schema_.list.rows);
  if (!rows && !doc.is_discarded() && schema_.list.rows.empty()) rows = &doc;
  if (!rows || !rows->is_array()) {
    error = "not a fund list";
    return false;
  }
  const std::regex fromId(schema_.isinFromId.empty() ? "$^" : schema_.isinFromId);
  for (const json & row : *rows) {
    if (!row.is_object()) continue;
    FundEntry e;
    e.row = row;
    applyFields(schema_.list, row, e.fund, e.id);
    if (e.id.empty()) continue;
    if (!isValidIsin(e.fund.isin)) e.fund.isin.clear();
    std::smatch m;
    if (e.fund.isin.empty() && !schema_.isinFromId.empty() && std::regex_match(e.id, m, fromId) && m.size() > 1 && isValidIsin(m[1].str()))
      e.fund.isin = m[1].str();
    // The table: by id, or by ISIN. An empty entry says the fund is known and not wanted.
    for (const std::string & key : {e.id, e.fund.isin}) {
      auto known = schema_.isins.find(key);
      if (key.empty() || known == schema_.isins.end()) continue;
      if (known->second.empty()) e.wanted = false;
      else if (e.fund.isin.empty() && isValidIsin(known->second)) e.fund.isin = known->second;
    }
    if (e.wanted && e.fund.isin.empty())
      if (auto found = findIsin(row)) {
        e.fund.isin = found->second;
        e.note = "ISIN " + found->second + " found in the list at " + found->first;
      }
    if (e.fund.name.empty()) e.fund.name = e.id;
    if (e.fund.symbol.empty()) e.fund.symbol = e.fund.isin.empty() ? e.id : e.fund.isin;
    e.fund.identifier = e.id;
    entries.push_back(std::move(e));
  }
  return true;
}

bool parseFundDetail(const FundSchema & schema_, const std::string & body, FundEntry & e, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !schema_.detail) {
    error = "not a fund description";
    return false;
  }
  std::string id = e.id;
  const std::string had = e.fund.isin;
  applyFields(*schema_.detail, doc, e.fund, id);
  if (!isValidIsin(e.fund.isin)) e.fund.isin = had;
  if (e.fund.isin.empty())
    if (auto found = findIsin(doc)) {
      e.fund.isin = found->second;
      e.note = "ISIN " + found->second + " found in the details at " + found->first;
    }
  // A fund that has only its id for a symbol takes its ISIN, now that it has one.
  if (e.fund.symbol == e.id && !e.fund.isin.empty()) e.fund.symbol = e.fund.isin;
  return true;
}

FundSource::FundSource(FundSchema schema)
    : schema_(std::move(schema)), history_(std::make_shared<FundHistory>(schema_)), today_([] {
        return dayNumberOfInstant(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
      }) {}

void FundSource::load(Store & store, Http & http, SourceListener & listener) {
  const std::string who = schema_.name + ": ";
  const auto pause = [&] {  // false: cancelled, return now
    return schema_.pause.count() <= 0 || listener.pause(schema_.pause);
  };
  std::string error, token;
  if (!schema_.tokenPage.empty()) {
    HttpResult page = browserGet(http, schema_.tokenPage, schema_.headers, BrowserRequest::Page);
    if (!page.ok()) {
      listener.message(who + "the token page: " + failure(page));
      return;
    }
    if (!parseToken(page.body, schema_.tokenRegex, token, error)) {
      listener.message(who + error);
      return;
    }
    history_->setToken(token);
    if (!pause()) return;
  }
  HttpResult list = browserGet(http, expand(schema_.list.url, "", token), schema_.headers);
  if (!list.ok()) {
    listener.message(who + "the fund list: " + failure(list));
    return;
  }
  std::vector<Entry> entries;
  if (!parseList(list.body, entries, error)) {
    listener.message(who + "the fund list: " + error);
    return;
  }
  const int64_t today = today_();
  size_t shown = 0, stale = 0, unpriced = 0;
  LeftOut leftOut;  // the funds whose ISIN or other part of the key cannot be named
  std::vector<Entry *> pending;  // the funds with no ISIN yet
  size_t sinceChange = 0;
  const auto show = [&](Entry & e) {
    // Shown at once, from the list alone: the latest NAV is the bar, and the change tells the NAV before it. The history comes when it is wanted.
    const Candle bar{*e.fund.day, kUndefined, kUndefined, kUndefined, *e.fund.price, kUndefined};
    bool written = false;
    store.batch([&](Batch & b) {
      if (!schema_.title.empty()) b.addDataset(schema_.dataset, schema_.title);
      auto edge = writeFund(b, schema_.dataset, e.fund, {bar});
      if (!edge) return;
      if (e.fund.change && *e.fund.change > -1) {
        b.setPreviousClose(*edge, *e.fund.day, *e.fund.price / (1 + *e.fund.change));
      }
      Changes changes = e.fund.changes;
      if (e.fund.change) changes.pct[static_cast<size_t>(Period::Day)] = *e.fund.change * 100;  // a fraction in the list: per cent
      b.setChanges(*edge, changes);
      written = true;
    });
    if (!written) {  // the one reason there is: it could not be named
      leftOut.add(e.fund.isin.empty() ? e.id : e.fund.isin);
      return;
    }
    shown++;
    if (++sinceChange >= 50) {  // the first screenful need not wait for the rest
      listener.changed();
      sinceChange = 0;
    }
  };
  for (Entry & e : entries) {
    if (listener.cancelled()) return;
    if (!e.wanted) continue;
    if (!e.fund.price || !e.fund.day) {
      unpriced++;
      continue;
    }
    // The list is the latest: a fund whose NAV is old is closed, or priced less often than daily.
    if (today - *e.fund.day > schema_.maxAgeDays) {
      stale++;
      continue;
    }
    if (!e.note.empty()) listener.message(who + e.note);
    if (!e.fund.isin.empty()) show(e);
    else pending.push_back(&e);
  }
  if (sinceChange) listener.changed();
  sinceChange = 0;

  // The funds with no ISIN are looked for in their details, one request each, after the others are on the screen. The details say the fees too.
  size_t unnamed = 0;
  for (Entry * e : pending) {
    if (listener.cancelled()) return;
    if (schema_.detail && !unresolved_.count(e->id)) {
      if (!pause()) return;
      HttpResult r = browserGet(http, expand(schema_.detail->url, e->id, token), schema_.headers);
      if (r.status == 429 || r.status == 403) {
        listener.message(who + "refused, stopping");
        break;
      }
      if (!r.ok()) listener.message(who + e->id + " details: " + failure(r));
      else {
        e->note.clear();
        if (!parseDetail(r.body, *e, error)) listener.message(who + e->id + " details: " + error);
        else {
          history_->markDetailed(e->id);
          if (!e->note.empty()) listener.message(who + e->note);
        }
      }
    }
    if (e->fund.isin.empty()) {
      // Said once: the next load does not ask again, or say it again.
      if (unresolved_.insert(e->id).second) listener.message(who + "no ISIN for " + e->id + " (" + e->fund.name + "), left out");
      unnamed++;
      continue;
    }
    show(*e);
  }
  if (sinceChange) listener.changed();
  leftOut.say(listener, who, "fund", "funds");
  listener.message(who + std::to_string(shown) + " funds" + (stale ? ", " + std::to_string(stale) + " left out for an old NAV" : std::string()) +
                   (unnamed ? ", " + std::to_string(unnamed) + " for no ISIN" : std::string()) +
                   (unpriced ? ", " + std::to_string(unpriced) + " for no price" : std::string()));
}

std::optional<HistoryProvider::Need> FundHistory::need(const Snapshot & snapshot, const EdgeKey & edge, Period) const {
  auto listing = snapshot.listing(edge);
  if (!listing || listing->identifier.empty()) return std::nullopt;
  const Covered covered = periodCovered(snapshot, edge, Period::Quarter);
  return Need{0, covered.covered, covered.fetched};
}

void FundHistory::setToken(std::string token) {
  std::lock_guard lock(mutex_);
  token_ = std::move(token);
}

void FundHistory::markDetailed(const std::string & id) {
  std::lock_guard lock(mutex_);
  detailed_.insert(id);
}

HistoryProvider::Outcome FundHistory::load(Store & store, Http & http, const EdgeKey & edge, int, const Wait & wait) {
  auto snapshot = store.snapshot();
  auto listing = snapshot->listing(edge);
  if (!listing || listing->identifier.empty()) return Outcome::Missing;
  const std::string id = listing->identifier;
  std::string token;
  bool detailed;
  {
    std::lock_guard lock(mutex_);
    token = token_;
    detailed = detailed_.count(id) > 0;
  }
  const auto status = [](const HttpResult & r) -> std::optional<Outcome> {
    if (r.status == 403 || r.status == 429) return Outcome::Refused;
    if (r.status == 0 || r.status >= 500) return Outcome::Offline;
    return std::nullopt;
  };
  // The details: the fees, once. A fund without them still has its history. What they say is written with the bars, in one batch; where
  // no bars follow (the history had no answer or was refused, or the loader is stopping), alone. Answered, they are not asked again.
  std::vector<Attribute> said;
  bool answered = false;  // the details were asked for and answered, and are not kept yet
  const auto keep = [&](const std::vector<Candle> * bars) {
    // The 3 and 10 years read weekly bars: a week's is its last NAV, the NAV being one value a day.
    std::vector<Candle> weeks;
    if (bars)
      for (const Candle & c : *bars) {
        Candle w = c;
        w.time = weekStart(c.time);
        if (!weeks.empty() && weeks.back().time == w.time) weeks.back() = w;
        else weeks.push_back(w);
      }
    if (!said.empty() || bars)
      store.batch([&](Batch & b) {
        b.setAttributes(edge.base, said);
        if (bars) {
          b.appendCandles(edge, Resolution::Day, *bars);
          b.appendCandles(edge, Resolution::Week, weeks);
        }
      });
    if (answered) {
      std::lock_guard lock(mutex_);
      detailed_.insert(id);
    }
  };
  if (schema_.detail && !detailed) {
    HttpResult r = browserGet(http, FundSource::expand(schema_.detail->url, id, token), schema_.headers);
    if (auto s = status(r)) return *s;
    answered = true;
    FundEntry entry;
    std::string error;
    if (r.ok() && parseFundDetail(schema_, r.body, entry, error)) said = attributesOf(entry.fund);
    // A pause between the two requests; stopping, the loader is going: the details are kept, and the history is asked for next time.
    if (!wait()) {
      keep(nullptr);
      return Outcome::Offline;
    }
  }
  HttpResult r = browserGet(http, FundSource::expand(schema_.historyUrl, id, token), schema_.headers);
  if (auto s = status(r)) {
    keep(nullptr);
    return *s;
  }
  std::vector<Candle> bars;
  std::string error;
  const bool found = r.ok() && parseNavSeries(r.body, bars, error);
  keep(found ? &bars : nullptr);
  return found ? Outcome::Ok : Outcome::Missing;
}
