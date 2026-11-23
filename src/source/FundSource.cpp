#include "source/FundSource.h"

#include "model/Isin.h"
#include "model/NodeKey.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

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
    auto scaled = [&](std::optional<double> n) -> std::optional<double> {
      auto s = request.scale.find(key);
      return n && s != request.scale.end() ? std::optional<double>(*n * s->second) : n;
    };
    if (key == "id") id = textOf(*v);
    else if (key == "symbol") fund.symbol = textOf(*v);
    else if (key == "name") fund.name = textOf(*v);
    else if (key == "isin") fund.isin = textOf(*v);
    else if (key == "risk") fund.risk = looseInteger(*v);
    else if (key == "rating") fund.rating = looseInteger(*v);
    else if (key == "esg") fund.esg = looseInteger(*v);
    else if (key == "managementFee") fund.managementFee = scaled(looseNumber(*v));
    else if (key == "subscriptionFee") fund.subscriptionFee = scaled(looseNumber(*v));
    else if (key == "redemptionFee") fund.redemptionFee = scaled(looseNumber(*v));
    else if (key == "link") {
      std::string link = textOf(*v);
      if (!link.empty() && link.rfind("http", 0) != 0) link = request.linkPrefix + (link[0] == '/' ? link : "/" + link);
      fund.url = link;
    }
  }
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
  for (auto [key, target] : {std::pair<const char *, std::string *>{"name", &s.name}, {"market", &s.market}, {"dataset", &s.dataset}}) {
    if (!d.contains(key) || !d[key].is_string() || d[key].get<std::string>().empty()) {
      error = std::string("no ") + key;
      return false;
    }
    *target = d[key].get<std::string>();
  }
  if (!datasetKey(s.dataset)) {
    error = "dataset is the code of the dataset, one to ten letters or digits (it is never changed), not \"" + s.dataset + "\"";
    return false;
  }
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
  if (!d.contains("history") || !d["history"].is_object() || !d["history"].contains("url") || !d["history"]["url"].is_string()) {
    error = "history: no url";
    return false;
  }
  s.historyUrl = d["history"]["url"].get<std::string>();
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
  // A name is a file of the schema directory (data/funds here, else the source tree's); anything with a path or an extension is a file as it is.
  std::filesystem::path path = nameOrPath;
  std::error_code ec;
  if (nameOrPath.find_first_of("/\\") == std::string::npos && path.extension() != ".json") {
    path.clear();
    for (const std::filesystem::path & dir : {std::filesystem::path("data") / "funds", std::filesystem::path(STOCKS_DATA_DIR) / "funds"})
      if (std::filesystem::is_regular_file(dir / (nameOrPath + ".json"), ec)) {
        path = dir / (nameOrPath + ".json");
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

bool FundSource::parseList(const std::string & body, std::vector<Entry> & entries, std::string & error) const {
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
    Entry e;
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

bool FundSource::parseDetail(const std::string & body, Entry & e, std::string & error) const {
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

void FundSource::load(Store & store, Http & http, SourceListener & listener) {
  const std::string who = schema_.name + ": ";
  const auto pause = [&] {
    if (schema_.pause.count() > 0) std::this_thread::sleep_for(schema_.pause);
  };
  std::string error, token;
  if (!schema_.tokenPage.empty()) {
    HttpResult page = http.get(schema_.tokenPage, schema_.headers);
    if (!page.ok()) {
      listener.message(who + "the token page: " + failure(page));
      return;
    }
    if (!parseToken(page.body, schema_.tokenRegex, token, error)) {
      listener.message(who + error);
      return;
    }
    pause();
  }
  HttpResult list = http.get(expand(schema_.list.url, "", token), schema_.headers);
  if (!list.ok()) {
    listener.message(who + "the fund list: " + failure(list));
    return;
  }
  std::vector<Entry> entries;
  if (!parseList(list.body, entries, error)) {
    listener.message(who + "the fund list: " + error);
    return;
  }
  size_t tried = 0, loaded = 0, sparse = 0;
  for (Entry & e : entries) {
    if (listener.cancelled()) return;
    if (!e.wanted) continue;
    if (!e.note.empty()) listener.message(who + e.note);
    if (schema_.detail) {
      // The details are asked for once, and say the fees (and an ISIN, where the list has none).
      auto have = details_.find(e.id);
      if (have == details_.end()) {
        pause();
        HttpResult r = http.get(expand(schema_.detail->url, e.id, token), schema_.headers);
        if (r.status == 429 || r.status == 403) {
          listener.message(who + "refused, stopping");
          return;
        }
        if (!r.ok()) listener.message(who + e.id + " details: " + failure(r));
        else have = details_.emplace(e.id, r.body).first;
      }
      if (have != details_.end()) {
        Entry before = e;
        e.note.clear();
        if (!parseDetail(have->second, e, error)) {
          listener.message(who + e.id + " details: " + error);
          details_.erase(have);
        } else if (!e.note.empty() && before.fund.isin.empty()) {
          listener.message(who + e.note);
        }
      }
    }
    if (e.fund.isin.empty()) {
      listener.message(who + "no ISIN for " + e.id + " (" + e.fund.name + "), left out");
      continue;
    }
    tried++;
    pause();
    HttpResult r = http.get(expand(schema_.historyUrl, e.id, token), schema_.headers);
    if (r.status == 429 || r.status == 403) {
      listener.message(who + "refused, stopping");
      return;
    }
    std::vector<Candle> bars;
    if (!r.ok()) listener.message(who + e.id + ": " + failure(r));
    else if (!parseNavSeries(r.body, bars, error)) listener.message(who + e.id + ": " + error);
    else if (tooSparse(bars)) sparse++;
    else if (writeFund(store, schema_.dataset, schema_.market, e.fund, bars)) {
      loaded++;
      listener.changed();
    }
  }
  listener.message(who + std::to_string(loaded) + " of " + std::to_string(tried) + " funds" +
                   (sparse ? ", " + std::to_string(sparse) + " left out for pricing less often than daily" : std::string()));
}
