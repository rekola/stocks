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
//     window    level  covers (counted back from the instrument's last bar)
//     day       5m     the stored day (288 bars)
//     week      1h     7 days
//     month     1h     30 days
//     3months   1d     90 days
//     1year     1d     365 days
//     3years    1d     1095 days
//     10years   1d     3650 days
//
//   A window is columns, as plans/server.md proposes for bars: {"level": "1h", "keys": [...], "close": [...]}, the key of each bar
//   (the 5-minute slot number, the hour number or the day number; see market/Bars.h) and its close, oldest first. A sparkline draws
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
