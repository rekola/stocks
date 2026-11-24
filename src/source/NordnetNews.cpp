#include "source/NordnetNews.h"

#include "source/Countries.h"

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace nordnet {

namespace {

std::string text(const json & o, const char * key) {
  auto it = o.find(key);
  return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int64_t integer(const json & o, const char * key) {
  auto it = o.find(key);
  return it != o.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
}

}  // namespace

bool parseNews(const std::string & body, std::vector<NewsEntry> & items, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("news") || !doc["news"].is_array()) {
    error = "not a news list";
    return false;
  }
  for (const json & n : doc["news"]) {
    if (!n.is_object()) continue;
    NewsEntry item;
    item.id = text(n, "id");
    item.headline = text(n, "headline");
    item.intro = text(n, "intro");
    item.language = text(n, "language");
    item.type = text(n, "newsType");
    item.url = text(n, "originalArticleLink");
    item.published = integer(n, "published") / 1000;  // milliseconds in the answer
    if (auto agency = n.find("agency"); agency != n.end() && agency->is_object()) item.agency = text(*agency, "id");
    if (auto tags = n.find("tags"); tags != n.end() && tags->is_array())
      for (const json & t : *tags)
        if (t.is_string()) item.tags.push_back(t.get<std::string>());
    if (auto ins = n.find("instruments"); ins != n.end() && ins->is_array())
      for (const json & i : *ins)
        if (i.is_object() && !text(i, "instrumentId").empty()) item.instruments.push_back({text(i, "instrumentId"), text(i, "symbol"), text(i, "name")});
    if (item.id.empty() || item.headline.empty() || item.published <= 0) continue;
    items.push_back(std::move(item));
  }
  return true;
}

NewsItem toNews(const NewsEntry & e, const Snapshot & snapshot) {
  NewsItem n;
  n.id = e.id;
  n.time = e.published;
  n.headline = e.headline;
  n.body = e.intro;
  n.url = e.url;
  for (const NewsInstrument & i : e.instruments)
    if (auto node = snapshot.findByAttribute(NodeType::Instrument, kInstrumentId, i.uuid)) n.about.push_back(*node);
  // The news is of the countries its tags name (FI, SE, ...); the others are topics and regions (WORLD, TECH, ...), kept as the tags.
  for (const std::string & t : e.tags)
    if (isCountryCode(t))
      if (const auto key = countryKey(t)) n.about.push_back(nodeOf(*key));
  n.attributes.push_back({predicate::kind, e.type == "PRESS_RELEASE" ? kPressRelease : e.type == "ANALYSIS" ? kAnalysis : kNewsStory});
  if (!e.type.empty()) n.attributes.push_back({predicate::type, e.type});
  if (!e.agency.empty()) n.attributes.push_back({"agency", e.agency});
  if (!e.language.empty()) n.attributes.push_back({"language", e.language});
  std::string tags;
  for (const std::string & t : e.tags) tags += (tags.empty() ? "" : " ") + t;
  if (!tags.empty()) n.attributes.push_back({"tags", tags});
  return n;
}

}  // namespace nordnet
