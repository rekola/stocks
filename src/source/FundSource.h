#pragma once

#include "source/FundRows.h"
#include "source/Source.h"

#include <chrono>
#include <map>
#include <optional>
#include <regex>

// Where in a company's answers the parts of a fund are: one fund company's way of publishing its funds, as data. Read from JSON (a file in data/funds, where the schemas that come
// with the program are), so that a company is a file and not code. Pointers are JSON pointers (RFC 6901); a field
// with several tries them in turn.
//
//   name, market, dataset     what the data is called in the store ("Some Funds" is a market, "some" a dataset)
//   pauseMs, refreshHours     the wait between requests (2000) and how often to load again (6)
//   headers                   sent with every request
//   token                     {page, regex}: a page that hands out a token (the first capture of the regex) that the other URLs have in `{token}`
//   list                      {url, rows, fields, scale, linkPrefix}: the list answer. `rows` points at the array of funds ("" for the answer itself).
//                             `fields` gives, of each row: id (what the details and the history are asked for by), symbol, name, isin, risk, rating, esg,
//                             managementFee, subscriptionFee, redemptionFee, link. `scale` multiplies a number (a fraction to per cent); `linkPrefix` is put
//                             before a link that is a path.
//   isinFromId                a regex whose first capture is the ISIN, where the id holds it
//   isins                     an ISIN by id or by ISIN: an empty one says that the fund is known and not wanted (closed, joined, priced less often than daily)
//   detail                    {url, fields}: a request for each fund (`{id}`) whose answer says more of it (the fees); asked for once
//   history                   {url}: the NAV history of a fund, `[[ms, value], ...]`, asked for with `{id}`
//
// The ISIN of a fund is found, in this order, in: its `isin` field, `isinFromId`, `isins`, any string of its list row that is an ISIN, and (when
// there is a `detail`) any string of the detail answer. The last two are said in a message, because a string that looks like an ISIN is not
// always the fund's. A fund with none is not stored; a message says which.
struct FundSchema {
  using Pointers = std::vector<std::string>;
  struct Request {
    std::string url;
    std::map<std::string, Pointers> fields;
    std::map<std::string, double> scale;
    std::string rows, linkPrefix;
  };
  std::string name, market, dataset;
  std::chrono::milliseconds pause{2000}, refresh{std::chrono::hours(6)};
  HttpHeaders headers;
  std::string tokenPage, tokenRegex;
  Request list;
  std::optional<Request> detail;
  std::string historyUrl, isinFromId;
  std::map<std::string, std::string> isins;

  // False, with `error` set, when the text is not a schema.
  static bool parse(const std::string & json, FundSchema &, std::string & error);
  // A schema by its name ("x": data/funds/x.json, here or in the source tree), or by the path of a file.
  static bool load(const std::string & nameOrPath, FundSchema &, std::string & error);
};

// One adapter for the funds of any company the schema describes: reads the list, each fund's details and its history, and writes the funds
// that have an ISIN and are priced daily into the store, with a market and a dataset of their own. No key. Priced less often than daily
// (monthly, quarterly, yearly) is judged from the history, not listed.
class FundSource : public Source {
 public:
  explicit FundSource(FundSchema schema) : schema_(std::move(schema)) {}

  std::string name() const override { return "funds:" + schema_.name; }
  std::string description() const override { return schema_.market + ": funds and their NAV history (no key)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return schema_.refresh; }

  // One fund of the list, before its ISIN is settled.
  struct Entry {
    FundRow fund;
    std::string id;
    nlohmann::json row;
    bool wanted = true;  // false: the schema says it is known and not wanted
    std::string note;    // how its ISIN was found, where it was neither a field nor in the schema
  };
  // The funds of a list answer. False, with `error` set, when it is not a list.
  bool parseList(const std::string & body, std::vector<Entry> &, std::string & error) const;
  // What a details answer says, onto the fund; an ISIN in it only where the fund has none. False, with `error` set, when it is not an object.
  bool parseDetail(const std::string & body, Entry &, std::string & error) const;

  static std::string expand(const std::string & url, const std::string & id, const std::string & token);
  static bool parseToken(const std::string & page, const std::string & regex, std::string & token, std::string & error);

 private:
  FundSchema schema_;
  std::map<std::string, std::string> details_;  // the answers, by id: they are asked for once
};
