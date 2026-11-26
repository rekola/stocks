#pragma once

#include <cstdint>
#include <optional>
#include <string>

// The key of a node: one 64-bit number that means the same in every run, so
// that the facts file, the edges and a server all name a node in the same way.
// A key is a positive int64 (bit 63 is never set), which leaves 63 bits: the low
// sixty are the identity, and bits 62-60 are the kind. Eight kinds, so adding one
// changes the enum and this comment and no key that exists:
//
//   0 (000) instrument   1 (001) currency or unit   2 (010) market   3 (011) dataset   built
//   4 (100) country      5 (101) portfolio                                              built
//   6 (110) attribute                                                                   planned, not built
//   7 (111) the escape: bits 59-52 are a second-level kind and the other 52 its identity (reserved)
//
// Zero is no key. See "Kinds and bits" in plans/data-model.md.
//
//   Instrument  its ISIN: two letters and nine letters or digits (26 x 26 x
//   36^9 values, under 2^56), plus one so that the
//               ISIN AA0000000000 is not zero. The check digit is not kept:
//               isinCheckDigit() gives it back
//   Currency    a currency's or a unit's code, up to eleven characters of
//   0-9A-Z, a hyphen, a period or a hash, the digits of a number of base
//               43 (0 ends it, 1 to 39 are the characters, 40 to 42 are kept
//               for others): 43^11 is under 2^60, and 43 is the largest base
//               that is. A lower case letter is its capital, so the case of a
//               code is not kept (`pts` is `PTS`)
//   Market      its MIC, the ISO 10383 market identifier code: four characters
//   of A-Z0-9, packed as a code. The special
//               ones are markets too: XOFF (listed instruments traded off an
//               exchange: funds) and XXXX (no market)
//   Dataset     its code, packed as a currency's
//   Country     its ISO 3166 alpha-2 code: two characters of A-Z, packed as a code
//   Portfolio   sixty random bits, minted when the user makes one (a portfolio has no
//               natural identity); in text a "P" and the fifteen hex digits
//
// A string that does not fit is refused (an empty optional), never changed: an
// ISIN must be valid, a code must be one to eleven characters of 0-9A-Za-z, a hyphen, a period or a hash (letters read as capitals), a MIC
// four of A-Z0-9.

enum class NodeKind : uint8_t { Instrument = 0,
                                Currency = 1,
                                Market = 2,
                                Dataset = 3,
                                Country = 4,
                                Portfolio = 5 };

struct NodeKey {
  uint64_t value = 0;
  bool valid() const { return value != 0; }
  NodeKind kind() const { return static_cast<NodeKind>(value >> 60); }
  uint64_t identity() const { return value & kIdentityMask; }
  friend bool operator==(NodeKey a, NodeKey b) { return a.value == b.value; }
  friend bool operator<(NodeKey a, NodeKey b) { return a.value < b.value; }

  static constexpr uint64_t kIdentityMask = (uint64_t{1} << 60) - 1;
};

// A portfolio's key from its identity (the low sixty bits; none for an identity of more, which would be another kind's).
std::optional<NodeKey> portfolioKey(uint64_t identity);
// A key of sixty random bits, for a portfolio the user makes. Two machines that mint do not meet (a chance of about one in
// 10^18 per pair), so nothing is asked of a server.
NodeKey mintPortfolioKey();
// The key of a portfolio's text ("P" and fifteen hex digits, as `text()` writes it).
std::optional<NodeKey> portfolioKey(const std::string & text);

constexpr size_t kMaxCodeLength = 11;

std::optional<NodeKey> instrumentKey(const std::string & isin);  // a valid ISIN
std::optional<NodeKey> currencyKey(const std::string & code);    // a currency's or a unit's code
std::optional<NodeKey> datasetKey(const std::string & code);
std::optional<NodeKey> countryKey(const std::string & code);  // exactly two characters of A-Z
std::optional<NodeKey>
marketKey(const std::string &mic); // exactly four characters of A-Z0-9

// What a key stands for, as text: the ISIN, the code, the MIC. Empty for no key
// or one with a kind and an identity that no key of that kind has.
std::string text(NodeKey);
inline std::string describe(NodeKey key) { return text(key); }
