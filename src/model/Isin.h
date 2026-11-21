#pragma once

#include <string>

// ISIN: two letters, nine letters or digits (the national number) and a check digit (ISO 6166: Luhn over the letters
// replaced by their numbers, A = 10 ... Z = 35).

// The check digit that follows the first eleven characters, or '\0' if they are not two letters and nine letters or digits.
char isinCheckDigit(const std::string & first11);

// Whether `isin` is twelve characters with the right check digit.
bool isValidIsin(const std::string & isin);

// An instrument that has no ISIN gets one of our own, in a namespace nobody else issues from: the prefix `ZZ`, which
// ISO 3166-1 leaves for private use (it is the "user-assigned" code element, and no numbering agency issues ISINs
// under it). The nine-character national number is a two-letter code for who it is from ("NN" Nordnet, "YF" Yahoo's
// continuous futures) and the provider's symbol, upper case, letters and digits only, up to seven characters, padded
// with zeros. Empty if the symbol does not fit or the prefix is not two letters.
std::string privateIsin(const std::string & source, const std::string & symbol);
