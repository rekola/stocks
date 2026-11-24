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
// what the thing is, not from who says it: there is one Zinc, whoever lists it, if two sources write the text exactly the same (the key
// is the hash of the exact text, so "Zinc" and "ZINC" are two things). An instrument with no ISIN is found by its symbol before its name:
// resolveInstrument tries the symbol first, and the name only where the symbol gives no key. The nine-character national number is a
// 64-bit hash (FNV-1a, the same on every platform) of the exact text, reduced to nine base-36 digits: "GC=F" and "GCF", or
// "abc" and "ABC", are different things, and any length has one. Nine base-36 digits hold about 46 bits, so two texts can
// share one; that is remote (one in 10^14 for a pair) and not guarded against. Empty only for an empty text.
std::string privateIsin(const std::string & name);
