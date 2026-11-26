#pragma once

#include "market/Store.h"

#include <map>
#include <memory>
#include <string>

// The read-only HTTP API for the desktop app: JSON, no key, GET only, and no raw-bars endpoint.
//
//   GET /health                 {"status": "ok"}, or 503 where the store does not answer
//   GET /instruments            [code, name, asset class, first and last bar time]
//   GET /summary[?codes=a,b]    for each instrument the latest bars of each sparkline window:
//
//     window    level   bars (the level)        keeps
//     day       day     5 minutes                  1 day
//     week      week    1 hour                     7 days
//     month     month   4 hours                    1 month
//     3months   year    1 day                      the last 90 days of the year
//     year      year    1 day                      1 year
//     3years    3years  1 week                     3 years
//     10years   10years 1 week                     10 years
//
//   A window is columns, as plans/server.md proposes for bars: {"level": "1h", "keys": [...], "close": [...]}, the size of the bars and the key of each
//   (the 5-minute slot, the hour, the 4-hour slot, the day number, or the day number of the Monday of a week; see market/Bars.h) and its close, oldest first. A sparkline draws
//   closes, and the change of a period is measured from the first one (the app's rule), so the bar's other values are not sent;
//   the day also has "previous_close" (the close of the last day before the day of the last bar, or null), which its change is
//   measured from. A level that has not been loaded gives an empty window, not an error.
//
//   Times are UTC seconds. `last_bar_time` is the start of the last bar of the source data, which every window ends at (not at the
//   current time: the data lags, and markets close at weekends); null where nothing is loaded.
namespace market {

struct ApiResponse {
  int status = 200;
  std::string body;  // JSON
};

// The answer to a GET of `path` with the query parameters `params` (decoded). Never throws: a store that fails is a 503.
ApiResponse handleApi(MarketStore &, const std::string & path, const std::map<std::string, std::string> & params);

class ApiServer {
 public:
  explicit ApiServer(MarketStore &);
  ~ApiServer();
  // Binds the address and serves until stop(); a port of 0 takes any free one (see port()). False, with `error`, where it cannot bind.
  bool listen(const std::string & host, int port, std::string & error);
  // Binds, and serves later on `serve()`: for the tests.
  bool bind(const std::string & host, int port, std::string & error);
  void serve();
  int port() const;
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace market
