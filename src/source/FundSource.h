#pragma once

#include "source/FundRows.h"
#include "source/Source.h"

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <optional>
#include <regex>

// Where in a company's answers the parts of a fund are: one fund company's way
// of publishing its funds, as data. Read from JSON (a file in data/funds, where
// the schemas that come with the program are), so that a company is a file and
// not code. Pointers are JSON pointers (RFC 6901); a field with several tries
// them in turn.
//
//   name, dataset             what the company is called and the code of its
//   dataset in the store ("SOME": one to eleven capitals, digits, hyphens, periods or hashes, and
//                             never changed, since what is kept is kept under
//                             it). A fund is priced by its company and on no
//                             venue: market XOFF
//   title                     the name the table shows for the dataset
//                             ("Säästöpankki"); the code, if there is none
//   pauseMs, refreshHours     the wait between requests (2000) and how often to
//   load again (6) headers                   sent with every request, over a Chrome's (browserHeaders: the Referer says
//   which page asks, an empty value leaves the browser's out) token
//   {page, regex}: a page that hands out a token (the first capture of the
//   regex) that the other URLs have in `{token}` list {url, rows, fields,
//   scale, linkPrefix}: the list answer. `rows` points at the array of funds
//   ("" for the answer itself).
//                             `fields` gives, of each row: id (what the details
//                             and the history are asked for by), symbol, name,
//                             isin, risk, rating, esg, managementFee,
//                             subscriptionFee, redemptionFee, link, and the
//                             latest NAV: price, date (a date or an instant)
//                             and change (from the NAV before it, as a
//                             fraction). A fund with a price and a date is
//                             shown at once, from the list alone. `attributes`
//                             keeps more of the row, by predicate: a number, a
//                             text or a truth value as it is, a date for a
//                             predicate that ends in -day. `scale` multiplies a
//                             number (a fraction to per cent); `linkPrefix` is
//                             put before a link that is a path.
//   isinFromId                a regex whose first capture is the ISIN, where
//   the id holds it isins                     an ISIN by id or by ISIN: an
//   empty one says that the fund is known and not wanted (closed, joined,
//   priced less often than daily) maxAgeDays                a fund whose latest
//   NAV is older than this (14) is not shown: it is closed, or priced less
//   often than daily detail                    {url, fields}: a request for
//   each fund (`{id}`) whose answer says more of it (the fees); asked for once
//   history                   {url}: the NAV history of a fund, `[[ms, value],
//   ...]`, asked for with `{id}`; or {morningstar: {client, universe, currency
//   (EUR), startDate (2000-01-01)}}: Morningstar's, where the fund's `id` is
//   its Morningstar id (secId), and this adapter makes the URL. It is not asked
//   for with the list but when the fund is wanted (on the screen), one fund at
//   a time, together with its details.
//
// The ISIN of a fund is found, in this order, in: its `isin` field,
// `isinFromId`, `isins`, any string of its list row that is an ISIN, and (when
// there is a `detail`) any string of the detail answer. The last two are said
// in a message, because a string that looks like an ISIN is not always the
// fund's. A fund with none is not stored; a message says which.
struct FundSchema {
  using Pointers = std::vector<std::string>;
  struct Request {
    std::string url;
    std::map<std::string, Pointers> fields;
    std::map<std::string, double> scale;
    std::map<std::string, Pointers> attributes;  // predicate to where it is: kept of the fund as it is (a predicate ending in -day is a date)
    std::string rows, linkPrefix;
  };
  std::string name, dataset, title;
  std::chrono::milliseconds pause{2000}, refresh{std::chrono::hours(6)};
  HttpHeaders headers;
  std::string tokenPage, tokenRegex;
  Request list;
  std::optional<Request> detail;
  int maxAgeDays = 14;
  std::string historyUrl, isinFromId;
  std::map<std::string, std::string> isins;

  // False, with `error` set, when the text is not a schema.
  static bool parse(const std::string & json, FundSchema &, std::string & error);
  // A schema by its name ("x": data/funds/x.json, here or in the source tree), or by the path of a file.
  static bool load(const std::string & nameOrPath, FundSchema &, std::string & error);
};

// One fund of the list, before its ISIN is settled.
struct FundEntry {
  FundRow fund;
  std::string id;
  nlohmann::json row;
  bool wanted = true;  // false: the schema says it is known and not wanted
  std::string note;    // how its ISIN was found, where it was neither a field nor in the schema
};

// The funds of a list answer. False, with `error` set, when it is not a list.
bool parseFundList(const FundSchema &, const std::string & body, std::vector<FundEntry> &, std::string & error);
// What a details answer says, onto the fund; an ISIN in it only where the fund has none. False, with `error` set, when it is not an object.
bool parseFundDetail(const FundSchema &, const std::string & body, FundEntry &, std::string & error);

// The history of a schema's funds, loaded when they are wanted: each fund's details (the fees) and its NAV history, which the list does not
// have. The source that found the funds shares it with the history loader.
class FundHistory : public HistoryProvider {
 public:
  explicit FundHistory(FundSchema schema) : schema_(std::move(schema)) {}
  std::string dataset() const override { return schema_.dataset; }
  // One request for a fund company's every edge, whatever the period: the details and the NAV history, once per session, unless the store has the quarter's bars (daily, and weekly for the 3 and 10 years).
  std::optional<Need> need(const Snapshot &, const EdgeKey &, Period) const override;
  Outcome load(Store &, Http &, const EdgeKey &, int request, const Wait &) override;
  std::chrono::milliseconds pause() const override { return schema_.pause; }
  // The token of the list's session, which the requests have in `{token}`; and a fund whose details are in the store already.
  void setToken(std::string);
  void markDetailed(const std::string & id);

 private:
  FundSchema schema_;
  mutable std::mutex mutex_;
  std::string token_;
  std::set<std::string> detailed_;
};

// One adapter for the funds of any company the schema describes: reads the list, each fund's details and its history, and writes the funds
// that have an ISIN and are priced daily into the store, with a market and a dataset of their own. No key. Priced less often than daily
// (monthly, quarterly, yearly) is judged from the history, not listed.
class FundSource : public Source {
 public:
  explicit FundSource(FundSchema schema);

  std::string name() const override { return "funds:" + schema_.name; }
  std::vector<std::string> datasetCodes() const override { return {schema_.dataset}; }
  std::string description() const override {
    return schema_.name + ": funds and their NAV history (no key)";
  }
  void load(Store &, Http &, SourceListener &) override;
  Timeliness timeliness() const override { return Timeliness::Daily; }  // one price a day
  std::chrono::milliseconds refreshInterval() const override { return schema_.refresh; }

  using Entry = FundEntry;
  bool parseList(const std::string & body, std::vector<Entry> & entries, std::string & error) const { return parseFundList(schema_, body, entries, error); }
  bool parseDetail(const std::string & body, Entry & e, std::string & error) const { return parseFundDetail(schema_, body, e, error); }
  std::shared_ptr<HistoryProvider> historyProvider() override { return history_; }
  // What day it is, as a day number (the clock, unless a test says); the NAVs older than the schema's `maxAgeDays` are not shown.
  void setToday(std::function<int64_t()> today) { today_ = std::move(today); }

  static std::string expand(const std::string & url, const std::string & id, const std::string & token);
  static bool parseToken(const std::string & page, const std::string & regex, std::string & token, std::string & error);

 private:
  FundSchema schema_;
  std::shared_ptr<FundHistory> history_;
  std::function<int64_t()> today_;
  std::set<std::string> unresolved_;  // the ids that have no ISIN, in the list and in the details: not asked for again
};
