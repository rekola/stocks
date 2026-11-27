#pragma once

#include <cstddef>

// The ISINs the adapters supply themselves, where their source gives none or the thing has none: each is a name here, and an adapter names
// the one it needs, so a missing one does not compile, and two that are the same, or one that is not valid, do not compile either (the
// static_asserts below). Whoever adds one:
//   1. Looks through this file for the thing (its symbol, its name) and for the ISIN: neither may be here already.
//   2. If it has a real ISIN, adds it to `kReal` (the commodities and indices that no country numbers have ISINs under XC), with who it is.
//   3. Otherwise adds the next private one to `kPrivate`: ZZ (the user-assigned code element of ISO 3166, in which no numbering agency issues
//      ISINs), nine digits one above the last, and the check digit. The compiler says if the number or the check digit is wrong.
// A real one an adapter does not need to supply (the source gives it) does not belong here.
namespace known_isin {

// ---- Real ISINs, supplied where a source gives none or calls the thing something else.
inline constexpr const char * kGold = "XC0009655157";       // gold, per troy ounce
inline constexpr const char * kSilver = "XC0009653103";     // silver
inline constexpr const char * kPlatinum = "XC0009665545";   // platinum
inline constexpr const char * kPalladium = "XC0009665529";  // palladium
inline constexpr const char * kCopper = "XC0005705501";     // copper
inline constexpr const char * kZinc = "XC0005705584";       // zinc
inline constexpr const char * kLead = "XC0005705527";       // lead
inline constexpr const char * kBricIndia = "US26075E1047";  // Dow Jones BRIC 50 India Subindex (BRIC50D)
inline constexpr const char * kBitcoin = "XTV15WLZJMF0";    // Bitcoin
inline constexpr const char * kEthereum = "XTD5RG2FHH04";   // Ethereum
inline constexpr const char * kWti = "US12573F1084";         // WTI crude oil
inline constexpr const char * kBrent = "XC0009677409";       // Brent crude oil
inline constexpr const char * kSp500 = "US78378X1072";       // S&P 500
inline constexpr const char * kNasdaq100 = "US6311011026";   // Nasdaq 100
inline constexpr const char * kUsDollarIndex = "XD0009982303";  // US Dollar Index
inline constexpr const char * kDax = "DE0008469008";         // DAX
inline constexpr const char * kCac40 = "FR0003500008";       // CAC 40
inline constexpr const char * kEuroStoxx50 = "EU0009658145"; // Euro STOXX 50
inline constexpr const char * kFtse100 = "GB0001383545";     // FTSE 100
inline constexpr const char * kNikkei225 = "JP9010C00002";   // Nikkei 225
inline constexpr const char * kHangSeng = "HK0000004322";    // Hang Seng
inline constexpr const char * kAsx200 = "XC0006013624";      // S&P/ASX 200

inline constexpr const char * kReal[] = {kGold, kSilver, kPlatinum, kPalladium, kCopper, kZinc, kLead, kBricIndia, kBitcoin, kEthereum, kWti, kBrent, kSp500, kNasdaq100,
                                          kUsDollarIndex, kDax, kCac40, kEuroStoxx50, kFtse100, kNikkei225, kHangSeng, kAsx200};

// ---- Ours: private, ZZ and a number counted on from 1, with no gaps.
inline constexpr const char * kGoldBar = "ZZ0000000016";       //  1: a gold bar held by the gram of fine gold (the physical metals, estimated)
inline constexpr const char * kGoldCoin = "ZZ0000000024";      //  2: a gold coin held by the ounce of fine gold
inline constexpr const char * kPlatinumBar = "ZZ0000000032";   //  3: a platinum bar held by the gram of fine platinum

inline constexpr const char * kPrivate[] = {kGoldBar, kGoldCoin, kPlatinumBar};

// ---- What the compiler checks.

constexpr size_t length(const char * s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

constexpr bool same(const char * a, const char * b) {
  size_t i = 0;
  for (; a[i] && b[i]; i++)
    if (a[i] != b[i]) return false;
  return a[i] == b[i];
}

// Twelve characters, two letters first, the last a digit, and the check digit right (ISO 6166: Luhn over the letters as numbers, A = 10).
constexpr bool valid(const char * isin) {
  if (length(isin) != 12) return false;
  int digits[24] = {};
  int count = 0;
  for (size_t i = 0; i < 12; i++) {
    const char c = isin[i];
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : -1;
    if (d < 0 || (i < 2 && d < 10) || (i == 11 && d >= 10)) return false;
    if (d >= 10) digits[count++] = d / 10;
    digits[count++] = d % 10;
  }
  int sum = 0;
  bool doubled = false;  // the check digit itself is not doubled
  for (int k = count - 1; k >= 0; k--) {
    int d = digits[k];
    if (doubled) {
      d *= 2;
      if (d > 9) d -= 9;
    }
    sum += d;
    doubled = !doubled;
  }
  return sum % 10 == 0;
}

template <size_t N>
constexpr bool allValid(const char * const (&list)[N]) {
  for (size_t i = 0; i < N; i++)
    if (!valid(list[i])) return false;
  return true;
}

// No ISIN twice, in one list or in both.
template <size_t N, size_t M>
constexpr bool allDifferent(const char * const (&a)[N], const char * const (&b)[M]) {
  for (size_t i = 0; i < N; i++) {
    for (size_t j = i + 1; j < N; j++)
      if (same(a[i], a[j])) return false;
    for (size_t j = 0; j < M; j++)
      if (same(a[i], b[j])) return false;
  }
  for (size_t i = 0; i < M; i++)
    for (size_t j = i + 1; j < M; j++)
      if (same(b[i], b[j])) return false;
  return true;
}

// Ours are ZZ and the number i + 1 in nine digits, counted on without gaps.
template <size_t N>
constexpr bool numbered(const char * const (&list)[N]) {
  for (size_t i = 0; i < N; i++) {
    const char * s = list[i];
    if (s[0] != 'Z' || s[1] != 'Z') return false;
    size_t number = 0;
    for (size_t k = 2; k < 11; k++) {
      if (s[k] < '0' || s[k] > '9') return false;
      number = number * 10 + static_cast<size_t>(s[k] - '0');
    }
    if (number != i + 1) return false;
  }
  return true;
}

static_assert(allValid(kReal), "an ISIN in kReal is not valid: the check digit, the length or the letters");
static_assert(allValid(kPrivate), "an ISIN in kPrivate is not valid: the check digit, the length or the letters");
static_assert(allDifferent(kReal, kPrivate), "an ISIN is in the list twice");
static_assert(numbered(kPrivate), "the private ISINs are ZZ and the numbers 1, 2, 3 ... in order, with none left out");

}  // namespace known_isin
