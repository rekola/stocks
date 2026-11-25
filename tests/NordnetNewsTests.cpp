#include "TestFramework.h"

#include "FakeHttp.h"
#include "source/NordnetNews.h"
#include "source/NordnetSource.h"

#include <fstream>
#include <sstream>

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/nordnet/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// A store with the instruments two of the fixture's news items are about, as the search lists would have made them.
std::shared_ptr<Store> storeWithNewsInstruments(InstrumentId * alkane = nullptr, InstrumentId * oatly = nullptr) {
  std::shared_ptr<Store> store = makeMemoryStore();
  const InstrumentId a = store->addInstrument("", "Alkane Resources Ltd", InstrumentKind::Company, "AU000000ALK9", "ESH");
  const InstrumentId o = store->addInstrument("", "Oatly Group", InstrumentKind::Company, "US67421J1088", "ESH");
  store->setAttribute(a, {nordnet::kInstrumentId, "e66ae6a8-5427-4f91-94c1-c59a11eb1ff5"});
  store->setAttribute(o, {nordnet::kInstrumentId, "7fb9dda7-a3b1-4bf3-a266-75706e95b7f6"});
  if (alkane) *alkane = a;
  if (oatly) *oatly = o;
  return store;
}

NordnetSource newsOnly(int pageSize) {
  auto lists = NordnetSource::defaultLists();
  NordnetSource::List news = lists.back();
  news.pageSize = pageSize;
  return NordnetSource({news}, std::chrono::milliseconds(0));
}

}  // namespace

TEST(a_news_item_has_its_text_its_time_its_newswire_and_the_instruments_it_is_about) {
  std::vector<nordnet::NewsEntry> items;
  std::string error;
  CHECK(nordnet::parseNews(fixture("news-1.json"), items, error));
  CHECK(items.size() == 5);
  const nordnet::NewsEntry & first = items[0];
  CHECK(first.headline == "Stor sulfidfyndighet vid Rockley-projektet" && first.type == "PRESS_RELEASE");
  CHECK(first.agency == "GLOBE_NEWSWIRE" && first.language == "sv" && first.published == 1795214447);  // seconds, not milliseconds
  CHECK(first.url.find("globenewswire.com") != std::string::npos && !first.intro.empty() && !first.tags.empty());
  CHECK(first.instruments.size() == 1 && first.instruments[0].uuid == "e66ae6a8-5427-4f91-94c1-c59a11eb1ff5" && first.instruments[0].symbol == "ALKEF");
  CHECK(items[2].instruments.empty());  // a general item is about none
  CHECK(!nordnet::parseNews("{}", items, error) && error == "not a news list");
  CHECK(!nordnet::parseNews("", items, error));
}

TEST(news_is_about_the_instruments_the_search_lists_know_and_all_of_it_is_kept) {
  InstrumentId alkane, oatly;
  auto store = storeWithNewsInstruments(&alkane, &oatly);
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("news-1.json")); };
  Listener listener;
  NordnetSource source = newsOnly(50);
  source.load(*store, http, listener);
  CHECK(http.urls.size() == 1);  // fewer items than a page: the last page
  CHECK(http.urls[0].find("/news/v1/news/preview/main?includeInstruments=true&limit=50&agencies=GLOBAL_NEWSWIRE%2CFINWIRE_FUNDS%2CINDERES&offset=0") != std::string::npos);
  bool locale = false, cookie = false;
  for (const auto & [n, v] : http.headers[0]) {
    locale = locale || (n == "x-locale" && v == "fi-FI");
    cookie = cookie || n == "Cookie";
  }
  CHECK(locale && !cookie);

  auto s = store->snapshot();
  CHECK(s->newsCount() == 5);  // the general items too, and the ones about instruments we do not have
  NewsQuery q;
  q.about = NodeRef(alkane);
  auto a = s->news(q);
  CHECK(a.size() == 1 && a[0].headline == "Stor sulfidfyndighet vid Rockley-projektet" && a[0].time == 1795214447);
  CHECK(newsText(a[0], predicate::type) == "PRESS_RELEASE" && newsText(a[0], "agency") == "GLOBE_NEWSWIRE" && newsText(a[0], "language") == "sv");
  CHECK(a[0].url.find("globenewswire.com") != std::string::npos && !a[0].body.empty() && a[0].id == "14bb98f5-a35f-46a9-9891-cc0cdd8b2d98");
  CHECK(newsText(a[0], predicate::kind) == kPressRelease);  // Nordnet's PRESS_RELEASE, in the common vocabulary
  q.about = NodeRef(oatly);
  CHECK(s->news(q).size() == 1);
  // A tag that is a country's code makes the item about that country; the others (WORLD, TECH, ...) are topics, kept as the tags.
  NewsQuery sweden;
  sweden.about = nodeOf(*countryKey("SE"));
  CHECK(s->news(sweden).size() == 1 && newsText(s->news(sweden)[0], "tags").find("SE") != std::string::npos);
  CHECK(newsText(a[0], "tags").find("OCEANIA") != std::string::npos);
  size_t general = 0;
  for (const NewsItem & n : s->news())
    if (n.about.empty()) general++;
  CHECK(general == 3);
  CHECK(listener.changes == 1 && listener.messages.size() == 1 && listener.messages[0].find("news 5") != std::string::npos);

  // Loading again says nothing twice, and changes nothing.
  const uint64_t version = store->snapshot()->version();
  FakeHttp again;
  again.answer = http.answer;
  source.load(*store, again, listener);
  CHECK(again.urls.size() == 1 && store->snapshot()->newsCount() == 5 && store->snapshot()->version() == version);
}

TEST(the_first_load_reads_two_pages_of_news_and_a_later_one_the_newest_page) {
  auto store = storeWithNewsInstruments();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("news-1.json")); };
  Listener listener;
  NordnetSource source = newsOnly(5);  // a page as big as the fixture: there may be more
  source.load(*store, http, listener);
  CHECK(http.urls.size() == 2);
  CHECK(http.urls[0].find("&offset=0") != std::string::npos && http.urls[1].find("&offset=5") != std::string::npos);
  FakeHttp later;
  later.answer = http.answer;
  source.load(*store, later, listener);
  CHECK(later.urls.size() == 1 && later.urls[0].find("&offset=0") != std::string::npos);
}

TEST(a_refusal_stops_the_news_and_what_is_not_news_is_said_so) {
  auto store = storeWithNewsInstruments();
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(429); };
  Listener listener;
  newsOnly(50).load(*store, http, listener);
  CHECK(http.urls.size() == 1 && listener.messages.size() == 1 && listener.messages[0] == "nordnet: news: HTTP 429, stopping");
  FakeHttp junk;
  junk.answer = [](const std::string &) { return ok("<html>"); };
  Listener l2;
  newsOnly(50).load(*store, junk, l2);
  CHECK(l2.messages.size() == 1 && l2.messages[0] == "nordnet: news: not a news list");
}
