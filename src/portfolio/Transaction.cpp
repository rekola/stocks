#include "portfolio/Transaction.h"

#include <nlohmann/json.hpp>

#include <random>

namespace portfolio {

uint64_t mintId() {
  static std::mt19937_64 random{std::random_device{}()};
  uint64_t id = 0;
  while (id == 0) id = random() & ((uint64_t{1} << 62) - 1);
  return id;
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
