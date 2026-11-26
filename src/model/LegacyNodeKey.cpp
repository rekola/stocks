#include "model/LegacyNodeKey.h"

#include "model/NodeKey.h"

namespace {

constexpr size_t kLegacyLength = 10;

// The six-bit symbols of schema 1: 0 ends a code, 1 to 10 are 0-9, 11 to 36 A-Z and 37 to 62 a-z.
char legacyChar(uint64_t symbol) {
  if (symbol >= 1 && symbol <= 10) return static_cast<char>('0' + (symbol - 1));
  if (symbol >= 11 && symbol <= 36) return static_cast<char>('A' + (symbol - 11));
  if (symbol >= 37 && symbol <= 62) return static_cast<char>('a' + (symbol - 37));
  return '\0';
}

}  // namespace

std::string legacyKeyCode(uint64_t key) {
  const NodeKey node{key};
  if (!node.valid()) return {};
  const NodeKind kind = node.kind();
  if (kind != NodeKind::Currency && kind != NodeKind::Market && kind != NodeKind::Dataset && kind != NodeKind::Country) return {};
  const uint64_t packed = node.identity();
  std::string code;
  for (size_t i = 0; i < kLegacyLength; i++) {
    const uint64_t symbol = (packed >> (6 * (kLegacyLength - 1 - i))) & 63;
    if (symbol == 0) break;
    const char c = legacyChar(symbol);
    if (c == '\0') return {};
    code += c;
  }
  return code;
}

uint64_t upgradeLegacyKey(uint64_t key) {
  std::string code = legacyKeyCode(key);
  if (code.empty()) return key;
  for (char & c : code)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  std::optional<NodeKey> upgraded;
  switch (NodeKey{key}.kind()) {
    case NodeKind::Currency: upgraded = currencyKey(code); break;
    case NodeKind::Market: upgraded = marketKey(code); break;
    case NodeKind::Dataset: upgraded = datasetKey(code); break;
    case NodeKind::Country: upgraded = countryKey(code); break;
    default: break;
  }
  return upgraded ? upgraded->value : key;
}
