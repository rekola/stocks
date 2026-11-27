#pragma once

#include "model/Store.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

// The read-only HTTP API for the desktop app: JSON, no key, GET only, and no raw-bars endpoint. It reads the application's own Store (the facts,
// summaries and queues the ingest wrote, restored from the database), so what it says of an instrument is what the application would show.
//
//   GET /health                 {"status": "ok", "instruments": N}
//   GET /instruments            [code, name, asset class, first and last bar time] for each edge
//   GET /summary[?codes=a,b]    for each instrument the latest bars of each sparkline period, by the periods of the application (Period.h):
//
//     period    resolution of the bars (the first of its feeds that has any)   covers, counted back from the instrument's last bar
//     day       5m         1 day
//     week      1h         7 days
//     month     1d         30 days
//     quarter   1d         90 days
//     year      1w         365 days
//     year3     1w         3 years
//     year10    1w         10 years
//
//   A period is columns, as plans/server.md proposes for bars: {"resolution": "1h", "keys": [...], "close": [...]}, the key of each bar (the
//   5-minute slot, the hour, the day number, or the day number of the Monday of a week; see market/Bars.h) and its close, oldest first. A
//   sparkline draws closes, and the change of a period is measured from the first one (the app's rule), so the bar's other values are not sent;
//   the day also has "previous_close" (the close of the last day before the day of the last bar, or null), which its change is measured from.
//   A resolution that has not been loaded gives an empty period, not an error.
//
//   Times are UTC seconds. `last_bar_time` is the start of the last bar of the instrument's finest series, which every period ends at (not at the
//   current time: the data lags, and markets close at weekends); null where nothing is loaded.
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
