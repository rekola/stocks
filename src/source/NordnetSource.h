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
};

// Nordnet's unofficial search endpoints: no key and no login (a cookie is not needed). Discovery of the ETFs for now:
// every ETF as an instrument (kind ETF, with its ISIN), its quote currency, its market and its edge, under the dataset
// "nordnet-1d". An ETF listed on two markets is one instrument and two edges. History is the next step.
class NordnetSource : public Source {
 public:
  // `pageSize` is how many a request asks for (the endpoint gave 500); `pause` is the wait between requests.
  explicit NordnetSource(int pageSize = 500, std::chrono::milliseconds pause = std::chrono::milliseconds(500))
      : pageSize_(pageSize), pause_(pause) {}

  std::string name() const override { return "nordnet"; }
  std::string description() const override { return "Nordnet: instrument discovery (ETFs for now), no key"; }
  void load(Store &, Http &, SourceListener &) override;

  // What load() found, for the history step. Valid once load() has returned.
  const std::vector<NordnetListing> & listings() const { return listings_; }

  // One page of the ETF list.
  struct Page {
    std::vector<NordnetListing> listings;  // the rows that have what a listing needs
    int rows = 0;                          // how many rows the page held, listed or not: where the next page starts
    int skipped = 0;                       // the rows left out for lacking an id, symbol, ISIN, currency or market
    int total = 0;                         // how many there are in all
  };
  // False, with `error` set, when `json` is not a page.
  static bool parsePage(const std::string & json, Page & page, std::string & error);

  // The market's name: the exchange code at the end of the slug (xeta, xsto, ...) as a name, else its Nordnet id.
  static std::string marketName(const NordnetListing &);

 private:
  int pageSize_;
  std::chrono::milliseconds pause_;
  std::vector<NordnetListing> listings_;
};
