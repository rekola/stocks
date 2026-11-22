#pragma once

#include <string>

// ISIN: two letters, nine letters or digits (the national number) and a check digit (ISO 6166: Luhn over the letters
// replaced by their numbers, A = 10 ... Z = 35).

// The check digit that follows the first eleven characters, or '\0' if they are not two letters and nine letters or digits.
char isinCheckDigit(const std::string & first11);

// Whether `isin` is twelve characters with the right check digit.
bool isValidIsin(const std::string & isin);

// A thing that has no ISIN gets one of our own, in a namespace nobody else issues from: the prefix `ZZ`, which ISO 3166-1
// leaves for private use (the "user-assigned" code element; no numbering agency issues ISINs under it). It is made from
// what the thing is, not from who says it: there is one Zinc, whoever lists it. The nine-character national number is its
// name, upper case, letters and digits only, padded with zeros. Empty if the name does not fit in nine.
std::string privateIsin(const std::string & name);
