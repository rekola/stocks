#include "model/NodeKey.h"

#include "model/Isin.h"

#include <cstdio>
#include <random>

namespace {

constexpr uint64_t kNsinValues = 101'559'956'668'416;  // 36^9

uint64_t withKind(NodeKind kind, uint64_t identity) { return (static_cast<uint64_t>(kind) << 60) | identity; }

int base36(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return -1;
}
char fromBase36(uint64_t v) { return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10)); }

// The symbol of a character of a code: 1 to 10 for 0-9, 11 to 36 for A-Z, 37 for a hyphen, 38 for a period and 39 for a hash, 0 for any other (40 to 42 are reserved). A lower case letter is its capital: the
// case of a code is not kept, so that `nordnet` and `NORDNET` are one node.
uint64_t symbolOf(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint64_t>(c - '0') + 1;
  if (c >= 'A' && c <= 'Z') return static_cast<uint64_t>(c - 'A') + 11;
  if (c >= 'a' && c <= 'z') return static_cast<uint64_t>(c - 'a') + 11;
  if (c == '-') return 37;
  if (c == '.') return 38;
  if (c == '#') return 39;
  return 0;
}
char charOf(uint64_t symbol) {
  if (symbol >= 1 && symbol <= 10) return static_cast<char>('0' + (symbol - 1));
  if (symbol >= 11 && symbol <= 36) return static_cast<char>('A' + (symbol - 11));
  if (symbol == 37) return '-';
  if (symbol == 38) return '.';
  if (symbol == 39) return '#';
  return '\0';  // 0 and the reserved 40 to 42
}

// A code is kMaxCodeLength digits of base 43 (0 ends it and pads it, 1 to 39 are the characters, 40 to 42 are kept for others), the first
// character the most significant: 43^11 is under 2^60, so eleven characters fit the sixty bits of the identity, where six bits for each would
// have left room for ten. 43 is the largest base that fits (44^11 is over 2^60), so a character is added to the alphabet from 40 on and
// no key changes.
constexpr uint64_t kBase = 43;
constexpr uint64_t kCodeValues = 929'293'739'471'222'707;  // 43^11

std::optional<uint64_t> packCode(const std::string & code) {
  if (code.empty() || code.size() > kMaxCodeLength) return std::nullopt;
  uint64_t packed = 0;
  for (size_t i = 0; i < kMaxCodeLength; i++) {
    uint64_t symbol = 0;
    if (i < code.size()) {
      symbol = symbolOf(code[i]);
      if (symbol == 0) return std::nullopt;
    }
    packed = packed * kBase + symbol;
  }
  return packed;
}

std::string unpackCode(uint64_t packed) {
  if (packed >= kCodeValues) return {};
  uint64_t symbols[kMaxCodeLength];
  for (size_t i = kMaxCodeLength; i-- > 0;) {
    symbols[i] = packed % kBase;
    packed /= kBase;
  }
  std::string code;
  for (size_t i = 0; i < kMaxCodeLength; i++) {
    if (symbols[i] == 0) {
      for (size_t j = i; j < kMaxCodeLength; j++)
        if (symbols[j] != 0) return {};  // a character after the end
      break;
    }
    const char c = charOf(symbols[i]);
    if (c == '\0') return {};  // a symbol that is kept for another character
    code += c;
  }
  return code;  // empty for the identity zero
}

}  // namespace

std::optional<NodeKey> instrumentKey(const std::string & isin) {
  if (!isValidIsin(isin)) return std::nullopt;
  uint64_t country = static_cast<uint64_t>(isin[0] - 'A') * 26 + static_cast<uint64_t>(isin[1] - 'A');
  uint64_t national = 0;
  for (size_t i = 2; i < 11; i++) national = national * 36 + static_cast<uint64_t>(base36(isin[i]));
  return NodeKey{withKind(NodeKind::Instrument, country * kNsinValues + national + 1)};
}

std::optional<NodeKey> currencyKey(const std::string & code) {
  const auto packed = packCode(code);
  if (!packed) return std::nullopt;
  return NodeKey{withKind(NodeKind::Currency, *packed)};
}

std::optional<NodeKey> datasetKey(const std::string & code) {
  const auto packed = packCode(code);
  if (!packed) return std::nullopt;
  return NodeKey{withKind(NodeKind::Dataset, *packed)};
}

std::optional<NodeKey> countryKey(const std::string & code) {
  if (code.size() != 2) return std::nullopt;
  for (char c : code)
    if (c < 'A' || c > 'Z') return std::nullopt;
  const auto packed = packCode(code);
  if (!packed) return std::nullopt;
  return NodeKey{withKind(NodeKind::Country, *packed)};
}

std::optional<NodeKey> marketKey(const std::string &mic) {
  if (mic.size() != 4)
    return std::nullopt;
  for (char c : mic)
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      return std::nullopt;
  const auto packed = packCode(mic);
  if (!packed)
    return std::nullopt;
  return NodeKey{withKind(NodeKind::Market, *packed)};
}

std::optional<NodeKey> portfolioKey(uint64_t identity) {
  if (identity > NodeKey::kIdentityMask) return std::nullopt;
  return NodeKey{withKind(NodeKind::Portfolio, identity)};
}

NodeKey mintPortfolioKey() {
  static std::mt19937_64 random{std::random_device{}()};
  return *portfolioKey(random() & NodeKey::kIdentityMask);
}

std::optional<NodeKey> portfolioKey(const std::string & text) {
  if (text.size() != 16 || text[0] != 'P') return std::nullopt;
  uint64_t identity = 0;
  for (size_t i = 1; i < text.size(); i++) {
    const char c = text[i];
    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (digit < 0) return std::nullopt;
    identity = identity * 16 + static_cast<uint64_t>(digit);
  }
  return portfolioKey(identity);
}

std::string text(NodeKey key) {
  if (!key.valid()) return {};
  switch (key.kind()) {
    case NodeKind::Instrument: {
      uint64_t v = key.identity();
      if (v == 0) return {};
      v -= 1;
      if (v >= 26 * 26 * kNsinValues) return {};
      std::string first11;
      first11 += static_cast<char>('A' + (v / kNsinValues) / 26);
      first11 += static_cast<char>('A' + (v / kNsinValues) % 26);
      uint64_t national = v % kNsinValues;
      std::string digits(9, '0');
      for (size_t i = 9; i-- > 0;) {
        digits[i] = fromBase36(national % 36);
        national /= 36;
      }
      first11 += digits;
      const char check = isinCheckDigit(first11);
      return check == '\0' ? std::string() : first11 + check;
    }
    case NodeKind::Currency:
    case NodeKind::Dataset:
    case NodeKind::Market:
    case NodeKind::Country:
      return unpackCode(key.identity());
    case NodeKind::Portfolio: {
      std::string out = "P";
      for (int shift = 56; shift >= 0; shift -= 4) out += "0123456789abcdef"[(key.identity() >> shift) & 15];
      return out;
    }
  }
  return {};  // kinds that are planned and not built (attribute) and the escape have no text
}
