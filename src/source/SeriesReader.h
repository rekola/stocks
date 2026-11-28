#pragma once

#include "model/Types.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

// Reads what a provider answers with a time series, whatever its shape, into series of bars: one class that is a function of the answer's text, with no
// network and no store, so that every shape there is can be tested with an answer in a string. The adapters that read histories (a fund company's NAVs,
// a metals dealer's bars, a provider with every metal in one answer) share it, and a schema only says what the heuristics cannot tell.
//
// What it works out, in this order, and says in `Result::notes` where it had to choose:
//   - The envelope: `{"statusCode":200,"data":[...]}`, `{"symbol":"XAU/USD","bars":[...],"meta":{...}}`: the one member that holds the series (a lone one, the one
//     named like a series: data, result, bars, prices, ..., or the only one with rows beside lists that are empty or of text). An answer that says it is an error (an `error` member that is not empty, a
//     `statusCode` or `status` of 400 and over) is no series, and its message is the `error`.
//   - The shape of the rows:
//       rows of arrays        [[1700000000000, 41.5], [t, o, h, l, c], [t, o, h, l, c, v]]
//       an array of structs   [{"startDateTime": "...", "open": ..., "close": ...}, ...], or one value a row
//       struct of arrays      {"t": [...], "o": [...], "h": [...], "l": [...], "c": [...]}
//       series by name        {"XAU": [...], "XAG": [...]}, each member read as one of the above, the name its label
//       series in a struct    [{"Code": "AU", "Prices": [{"Currency": "EUR", "TradeTime": ..., "Bid": ...}, ...]}, ...]: a struct that holds an array of rows
//                             is a series (or several), and its other members that are text (Code, RateType) are its labels
//       joined columns        {"timestamp": [...], "indicators": {"quote": [{"open": [...], "close": [...]}]}}: a time that is an array, with arrays of its length
//                             anywhere under the same object (a chart's), which are its columns (a name used twice keeps the first)
//   - What each field is, by its name (time, start, date, bar_start ...; open, high, low, close, value, price, last, rate, nav ...; volume; bid, ask), and
//     where a name does not say, by its value (the one field of ISO times). A struct with a time and one number is a value (kept as `close`).
//   - What labels the series: the fields of text that are not times, and are the same within a series and differ between series (a `Currency` in
//     the rows of one array splits it into a series for each currency). One series in the answer has no labels, or the ones that do not differ.
//   - Time: a number is seconds or milliseconds (by its size), a text an ISO 8601 date or instant (no zone is UTC), a text of digits a number.
//   - The length of a bar: the median of end - start where the rows have an end, else the median step between starts. The length that was asked
//     for is only a hint (an answer may give finer or coarser bars than the request says), so it is what the answer says that counts.
// It never guesses between things it cannot tell apart: an answer with two candidate series, or rows whose columns it cannot name, is an error that says
// what is ambiguous and which `Hints` say it.
namespace series {

struct Bar {
  Timestamp time = 0;  // the start of the bar, UTC seconds
  Timestamp end = 0;   // its end, where the rows say it (else 0)
  double open = kUndefined, high = kUndefined, low = kUndefined, close = kUndefined, volume = kUndefined;
  double bid = kUndefined, ask = kUndefined;
};

struct Series {
  // The fields of text that label this series: ("Code", "AU"), ("Currency", "EUR"); a series that has its name as a key of the answer has ("key", name).
  std::vector<std::pair<std::string, std::string>> labels;
  std::vector<Bar> bars;     // oldest first, one for each start (the later row of two with the same start stands)
  int64_t barSeconds = 0;    // the length of a bar, 0 where it cannot be told (fewer than two bars and no end)
  // The value of a label, empty where it has none.
  std::string label(const std::string & key) const;
};

enum class Shape { None, RowsOfArrays, Structs, Columns, Named, Nested };

// Where the heuristics are overridden. Empty says nothing.
struct Hints {
  std::string rows;  // a JSON pointer to what holds the rows (or the series), "" for the heuristics' choice
  // Which key is which role: "time", "end", "open", "high", "low", "close", "value", "volume", "bid", "ask".
  std::map<std::string, std::string> fields;
};

struct Result {
  bool ok = false;
  std::string error;                 // what is wrong with the answer, where it is not ok
  Shape shape = Shape::None;         // of the first series
  std::vector<Series> series;        // in the order of the answer
  std::vector<std::string> notes;    // choices made, and rows left out ("3 rows that are not bars")
};

// Reads an answer. `ok` is false, with `error`, where it is not a series (or says it is an error, or is ambiguous).
Result read(const std::string & body, const Hints & hints = {});

// The instant, in UTC seconds, of a time as an answer writes it, or none: a number of seconds or of milliseconds, an ISO 8601 date or instant (no zone: UTC),
// a text of digits.
bool parseTime(const std::string & text, Timestamp & out);

}  // namespace series
