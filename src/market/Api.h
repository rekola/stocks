#pragma once

#include "model/Store.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

// The read-only HTTP API: JSON, no key, GET only (HEAD too), and no raw-bars endpoint. It reads the server's own Store (the facts, summaries
// and queues the ingest wrote, restored from the database) and says what the Store holds, as it holds it: the summary bars of each edge, under
// the dataset that wrote them. The server does not combine datasets and has no dataset of its own: the same symbol in two datasets is two
// instruments, and combining them (the median across the datasets that have a row) is the client's.
//
//   GET /health                    {"status": "ok", "instruments": N}
//   GET /instruments[?filters]     the instruments, without bars: what /summary says of each, and "first_bar_time"
//   GET /summary[?filters]         the instruments with their queues
//
// FILTERS (both are optional; together they are an intersection; no other parameter is accepted, a 400 names it):
//   edge=ID[,ID...]   these edges. A malformed id is a 400 and an id that is no instrument of the store a 404, either naming the id.
//   dataset=CODE      the edges the dataset wrote (the code as NodeKey.h writes it: capitals). A malformed code is a 400, a dataset with
//                     no instruments a 404.
// With no filter every instrument is listed, by the symbol of its listing and then by id, whatever the filters (not in the order asked).
//
// THE EDGE ID is what the API calls an instrument by, never its symbol (the same symbol is in several datasets): the texts of the four keys
// of the edge (model/NodeKey.h) joined by "/", BASE/QUOTE/MARKET/DATASET: the base and the quote an ISIN or a code, the market a MIC, the
// dataset a code (none of them has a "/" or a ","). "EUR/USD/XXXX/SOMEDS" is one unit of EUR in USD, in no market, as the dataset SOMEDS says it. It is
// returned as "id" and accepted in `edge` as it is written here and in no other spelling (a lower case "eur/usd/xxxx/someds" is a 400 that gives
// the id).
//
// AN INSTRUMENT, in /instruments and /summary:
//   id            the edge id
//   edge          {"base", "quote", "market", "dataset"}: the texts of the keys
//   dataset       {"code", "name"}: what the store has of the dataset (its name is its code until one is said)
//   listing       {"symbol", "identifier", "order_book", "type", "name"}: what the dataset said of the instrument on the market ("" where not)
//   base          what a client needs to make the base node as the server did: {"kind", "name", "isin"}. The kind is the model's
//                 (company, currency, commodity, index, etf, certificate, fund, unit, crypto, other); the name and the ISIN are given where the
//                 base is an instrument (its key is its ISIN) and null for a currency or a unit, which is its code alone.
//   last_bar_time UTC seconds: the start of the latest bar of any of its queues; null where it has none
//   first_bar_time  (/instruments only) the same for the earliest bar
//   queues        (/summary only) see below
//
// THE QUEUES are the ones the store keeps for its sparklines, each whole (its window as the store keeps it, cut no further), as the store
// holds them, whatever the app does with them: which resolution feeds which period of a sparkline is the app's business (model/Period.h),
// and a bar is sent once, at its resolution, not once for each period. An object by the model's name of the resolution ("5m", "1h", "1d",
// "1w"), for each resolution the edge has in memory; one that is not in memory is left out, not an error.
//
//   "1d": {"kind": "candle", "first_key": K, "last_key": K, "count": N, "fetched": MS,
//          "keys": [...], "open": [...], "high": [...], "low": [...], "close": [...], "volume": [...]}
//
// Columns of the same length (`count`), oldest first. A key is the model's own key of the bar (Candle.time: the slot number, 5 minutes or an
// hour since the epoch, or the day number of the day, or of the Monday of the week; see market/Bars.h and barStart). `fetched` is when the store
// last changed the queue, in milliseconds UTC. A value that is undefined is null, never NaN or a number made up: the volume always is, for a
// dataset that has none. A scalar queue (a fund's NAV, an index's value of the day: "kind": "scalar") has one value a bar, which is
// the "close"; the other four columns are null. Nothing in the answer is a quote (last, bid, ask), a previous close or a change.
//
// Times are UTC seconds, except `fetched`. A failure is {"error": "..."} with its status.
namespace market {

struct ApiResponse {
  int status = 200;
  std::string body;  // JSON
};

// Brings the queues of every edge of the store into memory (the store reads them from the database on a thread of its own) and waits for them,
// up to `timeoutSeconds`: what the API answers from. The store must have been restored.
void loadQueues(Store &, int timeoutSeconds = 60);

// The answer to a GET of `path` with the query parameters `params` (decoded). Never throws: a failure is a 503.
ApiResponse handleApi(Store &, const std::string & path, const std::map<std::string, std::string> & params);

// Serves `current()` (the store to answer from now, which a program renews as the ingest writes) until `stop()`.
class ApiServer {
 public:
  explicit ApiServer(std::function<std::shared_ptr<Store>()> current);
  ~ApiServer();
  // Binds the address; a port of 0 takes any free one (see port()). False, with `error`, where it cannot bind.
  bool bind(const std::string & host, int port, std::string & error);
  void serve();  // returns when stopped
  int port() const;
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace market
