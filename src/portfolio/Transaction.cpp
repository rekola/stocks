#include "portfolio/Transaction.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <random>

namespace portfolio {

// The milliseconds since the epoch and twenty random bits: 41 + 20 bits, below 2^62. Ids made one after the other are in the order they
// were made, so that two sells of one day apply in the order they were typed, and two devices do not meet (the same millisecond
// and the same twenty bits).
uint64_t mintId() {
  static std::mt19937_64 random{std::random_device{}()};
  static std::mutex mutex;
  static uint64_t last = 0;
  std::lock_guard lock(mutex);
  const uint64_t ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  uint64_t id = (ms << 20) | (random() & ((uint64_t{1} << 20) - 1));
  if (id <= last) id = last + 1;
  last = id;
  return id & ((uint64_t{1} << 62) - 1);
}

std::string encodeBody(const Transaction & t) {
  nlohmann::json j;
  j["side"] = t.side == Side::Buy ? "buy" : "sell";
  j["instrument"] = t.instrument.value;
  j["market"] = t.market.value;
  j["quote"] = t.quote.value;
  j["quantity"] = t.quantity;
  j["price"] = t.price;
  j["fee"] = t.fee;
  if (!t.note.empty()) j["note"] = t.note;
  return j.dump();
}

std::optional<Transaction> decodeTransaction(uint64_t id, int64_t day, const std::string & body) {
  try {
    const nlohmann::json j = nlohmann::json::parse(body);
    Transaction t;
    t.id = id;
    t.day = day;
    const std::string side = j.at("side").get<std::string>();
    if (side == "buy") t.side = Side::Buy;
    else if (side == "sell") t.side = Side::Sell;
    else return std::nullopt;
    t.instrument = InstrumentId{j.at("instrument").get<uint64_t>()};
    t.market = MarketId{j.at("market").get<uint64_t>()};
    t.quote = InstrumentId{j.at("quote").get<uint64_t>()};
    t.quantity = j.at("quantity").get<double>();
    t.price = j.at("price").get<double>();
    t.fee = j.at("fee").get<double>();
    if (j.contains("note")) t.note = j.at("note").get<std::string>();
    return t;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

}  // namespace portfolio
