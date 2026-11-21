#pragma once

#include "source/Source.h"

#include <chrono>

// One listing of an instrument on a market, as Nordnet's instrument search gives it. The ids are what the history
// requests need.
struct NordnetListing {
  int64_t instrumentId = 0;
  int marketId = 0, marketSubId = 0;
  std::string identifier;  // the instrument's identifier on that market
  std::string symbol, name, isin, currency, type, issuer, slug, clearingPlace, country;
  std::string list;    // the list it was found in
  std::string bookId;  // market_data_order_book_id: what the price history is asked for by
  // Today's bar from the listing's price block: what discovery gives besides the instrument. Absent where the listing
  // has not traded (no last price or no tick time). Daily bars start at midnight UTC of their date, as the history's do.
  bool hasBar = false;
  Candle bar;
};

// Nordnet's unofficial search endpoints: no key and no login (a cookie is not needed). Discovery loads every instrument
// of every list it knows (ETFs, certificates, ...) in the same way, whatever their type. Each is an instrument (a node,
// with its ISIN and its own type) and its quote currency; and from the price block of its listing, today's daily bar,
// under the dataset "nordnet", which is what makes the edge: an instrument with no bar has no edge and is not seen. An
// instrument listed on two markets is one instrument and two edges. The results go to the store a page at a time, as they
// arrive, and discovery is repeated (every five minutes by default), each time replacing today's bar. History is the next
// step.
class NordnetSource : public Source {
 public:
  // A list the search can give: its URL without the paging (the page size and offset are added), sorted by a key that
  // does not move between pages.
  struct List {
    std::string name, url;
    int pageSize;
  };
  // The lists loaded: the ETFs, and the certificates of type PTC.
  static std::vector<List> defaultLists();

  // `pause` is the wait between requests; they are made one at a time.
  explicit NordnetSource(std::vector<List> lists = defaultLists(),
                         std::chrono::milliseconds pause = std::chrono::milliseconds(500),
                         std::chrono::milliseconds refresh = std::chrono::minutes(5))
      : lists_(std::move(lists)), pause_(pause), refresh_(refresh) {}

  std::string name() const override { return "nordnet"; }
  std::string description() const override { return "Nordnet: instrument discovery (ETFs, certificates), no key"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }

  // What load() found, for the history step. Valid once load() has returned.
  const std::vector<NordnetListing> & listings() const { return listings_; }

  // One page of a list.
  struct Page {
    std::vector<NordnetListing> listings;  // the rows that have what a listing needs
    int rows = 0;                          // how many rows the page held, listed or not: where the next page starts
    int skipped = 0;                       // the rows left out for lacking an id, symbol, ISIN, currency or market
    int total = 0;                         // how many there are in all
  };
  // False, with `error` set, when `json` is not a page.
  static bool parsePage(const std::string & json, Page & page, std::string & error);

  // The market's name: from its Nordnet id where that is known, else "Nordnet market <id>". (The slug's exchange code
  // is not used: a listing can carry the code of its home exchange while it trades on another market.)
  static std::string marketName(const NordnetListing &);

  // The instrument's kind from its own type: the ETF types are ETFs, anything else is Other.
  static InstrumentKind kindOf(const std::string & type);

 private:
  std::vector<List> lists_;
  std::chrono::milliseconds pause_;
  std::chrono::milliseconds refresh_;
  std::vector<NordnetListing> listings_;
};
