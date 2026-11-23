#include "model/Isin.h"

#include <cctype>
#include <cstdint>

namespace {

int digitOf(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return -1;
}

}  // namespace

char isinCheckDigit(const std::string & first11) {
  if (first11.size() != 11 || !std::isupper(static_cast<unsigned char>(first11[0])) ||
      !std::isupper(static_cast<unsigned char>(first11[1])))
    return '\0';
  // The characters as one string of decimal digits, then Luhn from the right, the check digit still to come.
  std::string digits;
  for (char c : first11) {
    const int d = digitOf(c);
    if (d < 0) return '\0';
    digits += std::to_string(d);
  }
  int sum = 0;
  bool doubled = true;  // the digit next to the check digit is doubled
  for (size_t i = digits.size(); i-- > 0;) {
    int d = digits[i] - '0';
    if (doubled) {
      d *= 2;
      if (d > 9) d -= 9;
    }
    sum += d;
    doubled = !doubled;
  }
  return static_cast<char>('0' + (10 - sum % 10) % 10);
}

bool isValidIsin(const std::string & isin) {
  return isin.size() == 12 && isinCheckDigit(isin.substr(0, 11)) == isin[11];
}

std::string privateIsin(const std::string & name) {
  if (name.empty()) return {};
  // FNV-1a over the exact bytes: written out here because std::hash differs between platforms and runs of a library, and
  // the key must be the same wherever the song or the facts file is read.
  uint64_t hash = 14695981039346656037ull;
  for (char c : name) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  constexpr uint64_t kNumbers = 101559956668416ull;  // 36^9
  uint64_t number = hash % kNumbers;
  std::string code(9, '0');
  for (size_t i = code.size(); i-- > 0; number /= 36) code[i] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[number % 36];
  const std::string first11 = "ZZ" + code;
  const char check = isinCheckDigit(first11);
  return check == '\0' ? std::string() : first11 + check;
}
