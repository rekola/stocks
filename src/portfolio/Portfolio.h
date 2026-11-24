#pragma once

#include "model/Store.h"
#include "portfolio/Transaction.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What a person holds, worked out from what she did. Pure functions of plain values and a snapshot: nothing here touches the
// screen or the disk (plans/portfolios.md).
namespace portfolio {

// The currency a portfolio's value is shown in: its `currency` fact, a link to a currency node. None where it says none.
std::optional<InstrumentId> currencyOf(const Snapshot &, PortfolioId);
// The dataset whose transactions its holdings are derived from: its `ledger` fact, a link to a dataset node, else the user's own
// (`kUserDataset`). Only that one, never several added together, which would count a trade twice.
DatasetId ledgerOf(const Snapshot &, PortfolioId);

// One step of how many were held: after the transactions of a day.
struct QuantityPoint {
  int64_t day = 0;
  double quantity = 0;
};

// What is held of an instrument on a market in a currency: the position edge `portfolio -> instrument`, whose series is
// `history`, the quantity held over time (it goes to zero when the position is closed, and starts again when it is opened
// again).
struct Position {
  InstrumentId instrument;
  MarketId market;
  InstrumentId quote;
  double quantity = 0;
  // Average cost by the unit, in `quote`: the quantity-weighted mean of what the buys cost, their fees included. A sell
  // leaves it as it is. A position closed and opened again starts again.
  double average_cost = 0;
  std::vector<QuantityPoint> history;
  double cost() const { return quantity * average_cost; }
};

struct Refusal {
  uint64_t id = 0;  // of the transaction
  std::string why;
};

struct Derived {
  std::vector<Position> positions;  // the open ones (quantity above zero), in the order they were first opened
  std::vector<Refusal> refused;     // what was left out, and why
};

// The positions of a list of transactions in the order they were done (by day; on one day the buys first, then by id, so that
// a buy and a sell of one day do not depend on their ids). A sell of more than is held is refused, and so is a transaction with
// no quantity, a negative price or fee, or a part that is none; the rest are applied.
Derived derivePositions(std::vector<Transaction> transactions);

// Whether what is shown on this edge can be bought: its market is a real venue or XOFF (a fund priced by its company), never
// XXXX (`kNoMarket`), the market of what is priced and not traded: indices, spot prices, currency pairs, and the rate sources
// (Euribor, the €STR). The buttons, the commands and the model's write all ask this.
bool tradable(const Snapshot &, const EdgeKey &);
// The same by the market alone.
bool tradable(MarketId);

// The price of an instrument on a market in a currency: the last price of the quote of the edge with the newest quote
// (the first by key if two have the same), and the edge.
struct Price {
  EdgeKey edge;
  double close = 0;
  int64_t day = 0;  // of the quote (a day number)
  Timestamp time = 0;  // of the quote (seconds): the newest says
};
std::optional<Price> latestPrice(const Snapshot &, InstrumentId instrument, MarketId, InstrumentId quote);

// How many `to` one `from` costs: the latest close of an edge between the two currencies, or its inverse, or through one
// other currency by two such edges. Only currencies are converted (not a unit such as points or a rate); none where the store
// has no such edge. No rate is invented.
std::optional<double> rate(const Snapshot &, InstrumentId from, InstrumentId to);

// What a portfolio holds: derived from the transactions of the dataset it names, nothing else. The position edges
// `portfolio -> instrument`, each with its quantity series.
Derived holdings(const Snapshot &, PortfolioId);

struct PositionValue {
  Position position;
  std::optional<Price> price;  // in the position's own currency; none where no edge has bars
  double market_value = kUndefined;  // quantity * price, in the position's own currency
  double profit = kUndefined;        // market value less cost, in the position's own currency
  double profit_percent = kUndefined;
  std::optional<double> rate;    // to the portfolio's currency
  std::optional<double> value;   // market value in the portfolio's currency: none without a price or a rate
  std::optional<double> weight;  // its share of the total, 0 to 1
};

struct Valuation {
  InstrumentId currency;  // of the portfolio
  std::vector<PositionValue> rows;
  double total = 0;         // of the rows that have a value, in `currency`
  bool incomplete = false;  // a row has no value (no price, or no rate): the total is only of the others
};

// The positions at the prices the store has, in the portfolio's currency. A position with no rate is shown in its own
// currency, left out of the total, and the total is marked incomplete.
Valuation value(const std::vector<Position> &, const Snapshot &, InstrumentId currency);

}  // namespace portfolio
