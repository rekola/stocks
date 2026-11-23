#pragma once

#include <cstdint>
#include <optional>
#include <string>

// The key of a node: one 64-bit number that means the same in every run, so that the facts file, the edges and a server all
// name a node in the same way. The kind of node is in bits 61-60 and its identity in the low sixty, so a key is below 2^62
// (a positive int64 in SQLite) and zero is no key.
//
//   Instrument  its ISIN: two letters and nine letters or digits (26 x 26 x 36^9 values, under 2^56), plus one so that the
//               ISIN AA0000000000 is not zero. The check digit is not kept: isinCheckDigit() gives it back
//   Currency    a currency's or a unit's code, up to ten characters of 0-9A-Za-z, six bits each (0 ends it, 1 to 62 are the
//               characters), case kept
//   Market      sixty random bits, made by the store
//   Dataset     its code, packed as a currency's
//
// A string that does not fit is refused (an empty optional), never changed: an ISIN must be valid, a code must be one to ten
// characters of 0-9A-Za-z.

enum class NodeKind : uint8_t { Instrument = 0,
                                Currency = 1,
                                Market = 2,
                                Dataset = 3 };

struct NodeKey {
  uint64_t value = 0;
  bool valid() const { return value != 0; }
  NodeKind kind() const { return static_cast<NodeKind>(value >> 60); }
  uint64_t identity() const { return value & kIdentityMask; }
  friend bool operator==(NodeKey a, NodeKey b) { return a.value == b.value; }
  friend bool operator<(NodeKey a, NodeKey b) { return a.value < b.value; }

  static constexpr uint64_t kIdentityMask = (uint64_t{1} << 60) - 1;
};

constexpr size_t kMaxCodeLength = 10;

std::optional<NodeKey> instrumentKey(const std::string & isin);  // a valid ISIN
std::optional<NodeKey> currencyKey(const std::string & code);    // a currency's or a unit's code
std::optional<NodeKey> datasetKey(const std::string & code);
// The market with these sixty bits (the low sixty of `random`; zero is made one).
NodeKey marketKey(uint64_t random);

// What a key stands for, as text: the ISIN, the code, the market's sixty bits in hex. Empty for no key or one with a kind and an
// identity that no key of that kind has.
std::string describe(NodeKey);
// The ISIN or the code of a key of those kinds; empty for a market or no key.
std::string text(NodeKey);
