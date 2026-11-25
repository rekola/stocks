#pragma once

#include "model/Store.h"

#include <string>
#include <vector>

// Nordnet's news preview: press releases, analyses and news items of the newswires it shows for the Nordic markets, each with
// the instruments it is about (none for a general one). The text is the preview's intro, not the article: that is at `url`.
// Only the Nordnet adapter's files include this.
namespace nordnet {

// What an instrument is called by the news (a uuid): said of the instrument by the search lists, so that the news finds it.
inline constexpr const char * kInstrumentId = "nordnet-instrument";

struct NewsInstrument {
  std::string uuid;  // the instrument's id for the news: nnx_info.nnx_instrument_id of the search lists
  std::string symbol, name;
};
struct NewsEntry {
  std::string id, headline, intro, language, agency, type, url;
  int64_t published = 0;  // UTC seconds
  std::vector<std::string> tags;
  std::vector<NewsInstrument> instruments;
};

// The items of a news preview; false, with `error` set, when `json` is not one. An item with no id, no headline or no time is left out.
bool parseNews(const std::string & json, std::vector<NewsEntry> & items, std::string & error);

// The item as news: its type, newswire, language and tags are attributes (`type`, `agency`, `language`, `tags`), and it is about the
// instruments of the store that Nordnet's id for them finds; one about none we have is general news.
NewsItem toNews(const NewsEntry &, const Snapshot &);

}  // namespace nordnet
