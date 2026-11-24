#pragma once

#include "model/Types.h"

#include <cstdint>
#include <optional>
#include <string>

namespace portfolio {

enum class Side : uint8_t { Buy, Sell };

// One thing the user did: bought or sold some of an instrument on a market, at a price in a currency. Its `id` is minted when
// it is made (`mintId()`), so that it can be corrected or deleted; the transactions of a portfolio are events on it
// (Store.h), never summed from several datasets.
struct Transaction {
  uint64_t id = 0;
  int64_t day = 0;  // the day it was done, as a day number (Dates.h)
  Side side = Side::Buy;
  InstrumentId instrument;
  MarketId market;      // the listing it was done on: a real venue, or XOFF for a fund
  InstrumentId quote;   // the currency the price and the fee are in
  double quantity = 0;  // always positive; the side says which way
  double price = 0;     // per unit, in `quote`
  double fee = 0;       // in `quote`, never negative
  std::string note;
  friend bool operator==(const Transaction &, const Transaction &) = default;
};

// A number below 2^62 and not zero, made of the time and random bits: nothing to coordinate, two devices that mint do not meet, and ids
// made one after the other are in the order they were made.
uint64_t mintId();

// How a transaction is kept in a file: the id and the day are columns of their own, the rest is this text.
std::string encodeBody(const Transaction &);
std::optional<Transaction> decodeTransaction(uint64_t id, int64_t day, const std::string & body);

}  // namespace portfolio
