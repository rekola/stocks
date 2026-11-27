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
//   GET /summary[?filters]         the instruments with their queues (and `resolution`, below)
//
// A client that loads in batches reads /instruments for the rows and asks /summary for the edges on screen, one resolution at a time:
// /summary?edge=ID,ID,...&resolution=1d. The unfiltered /summary is every queue of every instrument (about 6 MB for the 66 pairs of a forex
// dataset: plans/server.md), and no client is meant to ask for it.
//
// FILTERS (all optional; edge and dataset together are an intersection; no other parameter is accepted, a 400 names it):
//   edge=ID[,ID...]   these edges, at most 100 ids in a request (a 400 states the cap). The parameter may be repeated: `edge=A&edge=B` is
//                     `edge=A,B` (the values of a repeated parameter are joined with commas). An empty item (`edge=`, `edge=A,`, `edge=A,,B`) is
//                     skipped, and an `edge` with no ids left is no filter. A malformed id is a 400 naming it.
//   dataset=CODE      the edges the dataset wrote (the code as NodeKey.h writes it: capitals). One code: a repeated `dataset`, or a comma, is a 400
//                     naming the parameter; a malformed code is a 400.
//   resolution=R[,R...]  (/summary only) the queues of these resolutions only: the model's names ("5m", "1h", "1d", "1w", ...), repeatable and
//                     with empty items skipped as `edge` is. An unknown name is a 400 naming it. A resolution an edge does not have (or does not
//                     have in memory) is left out of that edge, not an error. On /instruments it is a 400: there are no queues there.
// The instruments come in the order of the symbol of their listing and then of their id, whatever the order of the filters.
//
// THE ANSWER of /instruments and /summary is {"instruments": [...], "missing": [...]}.
//   Empty results are one rule: a well-formed filter that matches nothing is a 200 with "instruments": [] (a dataset that has no instruments, an
//   `edge` and a `dataset` that do not meet). Only input that cannot be read is a 400.
//   An id that is well formed and is no instrument of the store is NOT an error: the answer lists the other instruments, and the id is in
//   "missing" (as the API spells it). Reason: a batch of 100 edges must not fail whole, and give nothing of the other 99, for one edge that
//   is gone; and a client can tell the cases apart. An id in "missing" is gone (the store has no such edge); an id that is in "instruments"
//   with no queue (or not the resolution asked for) under "queues" is there and not loaded yet (the queue is on the disk, not in memory), so
//   ask again later. An edge that is in the store but not in the dataset asked for is neither: it is left out, as the filter says.
//
// THE EDGE ID is what the API calls an instrument by, never its symbol (the same symbol is in several datasets): the texts of the four keys
// of the edge (model/NodeKey.h) joined by "/", BASE/QUOTE/MARKET/DATASET: the base and the quote an ISIN or a code, the market a MIC, the
// dataset a code (none of them has a "/" or a ","). "EUR/USD/XXXX/SOMEDS" is one unit of EUR in USD, in no market, as the dataset SOMEDS says
// it. It is returned as "id" and accepted in `edge` as it is written here and in no other spelling (a lower case "eur/usd/xxxx/someds" is a 400
// that gives the id).
//
// AN INSTRUMENT, in /instruments and /summary:
//   id            the edge id
//   edge          {"base", "quote", "market", "dataset"}: the texts of the keys
//   dataset       {"code", "name"}: what the store has of the dataset (its name is its code until one is said)
//   listing       {"symbol", "provider_identifier", "nordnet_order_book_id", "provider_type", "name"}: what the dataset said of the instrument on the market ("" where not)
//   base          what a client needs to make the base node as the server did: {"kind", "name", "isin"}. The kind is the model's
//                 (company, currency, commodity, index, etf, certificate, fund, unit, crypto, other); the name and the ISIN are given where the
//                 base is an instrument (its key is its ISIN) and null for a currency or a unit, which is its code alone.
//   last_bar_time UTC seconds, the start of the latest bar of ... (see below); null where there is none
//   first_bar_time  (/instruments only) the start of the earliest bar of any queue the store has of the edge
//   queues        (/summary only) see below
//
// `last_bar_time` is not the same on the two: on /instruments it is the store's view, the latest bar of any queue the store has of the edge,
// in memory or on the disk (from each queue's coverage). On /summary it is the latest bar of the queues sent in that answer: after the
// `resolution` filter, and only the queues in memory; null where none is sent.
//
// THE QUEUES are the ones the store keeps for its sparklines, each whole (its window as the store keeps it, cut no further), as the store
// holds them, whatever the app does with them: which resolution feeds which period of a sparkline is the app's business (model/Period.h),
// and a bar is sent once, at its resolution, not once for each period. An object by the model's name of the resolution ("5m", "1h", "1d",
// "1w"), for each resolution the edge has in memory (and the filter lets through):
//
//   "1d": {"kind": "candle", "first_key": K, "last_key": K, "count": N, "last_hash": "16 hex digits", "fetched": MS,
//          "keys": [...], "open": [...], "high": [...], "low": [...], "close": [...], "volume": [...]}
//
// Columns of the same length (`count`), oldest first. A key is the model's own key of the bar (Candle.time: the slot number, 5 minutes or an
// hour since the epoch, or the day number of the day, or of the Monday of the week; see market/Bars.h and barStart). A value that is
// undefined is null, never NaN or a number made up: the volume always is, for a dataset that has none; and so is the open of a bar whose
// source gave none. A scalar queue (a value a bar, as its writer says it: a fund's NAV; "kind": "scalar") has the value as "close"; the other
// four columns are null. Nothing in the answer is a quote (last, bid, ask), a previous close or a change.
//
// To tell whether a queue has changed since the client last read it, compare `last_key`, `count` and `last_hash` (a digest of the stored
// record of the last bar, which changes when that bar does). `fetched` is NOT a change marker: it is when the store last fetched or confirmed
// the queue (milliseconds UTC), and it moves when the same bars are said again.
//
// Times are UTC seconds, except `fetched`. A failure is {"error": "..."} with its status. The answers are gzip-compressed for a client that
// sends `Accept-Encoding: gzip`, where the server was built with zlib.
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
