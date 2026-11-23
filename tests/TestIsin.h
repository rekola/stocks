#pragma once

#include "model/Isin.h"

#include <string>

// A valid ISIN for a test that wants many of them: ZZ (private use) and the number as nine digits, with the check digit.
inline std::string testIsin(int n) {
  std::string digits = std::to_string(n);
  const std::string first11 = "ZZ" + std::string(9 - digits.size(), '0') + digits;
  return first11 + isinCheckDigit(first11);
}
