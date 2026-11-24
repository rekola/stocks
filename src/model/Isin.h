#pragma once

#include <string>

// ISIN: two letters, nine letters or digits (the national number) and a check digit (ISO 6166: Luhn over the letters
// replaced by their numbers, A = 10 ... Z = 35).

// The check digit that follows the first eleven characters, or '\0' if they are not two letters and nine letters or digits.
char isinCheckDigit(const std::string & first11);

// Whether `isin` is twelve characters with the right check digit.
bool isValidIsin(const std::string & isin);
