#include "TestFramework.h"

#include "model/Isin.h"
#include "model/NodeKey.h"

#include <random>
#include <set>

namespace {

constexpr uint64_t kBelow62 = uint64_t{1} << 62;

// The key, or none (no key is zero, and no test wants zero).
NodeKey orNone(const std::optional<NodeKey> & key) { return key.value_or(NodeKey{}); }

// An ISIN of the two letters and the nine characters, with its check digit.
std::string isinOf(const std::string & first11) { return first11 + isinCheckDigit(first11); }

}  // namespace

TEST(real_isins_have_keys_that_give_them_back) {
  for (const char * isin : {"FI0009000681", "US0378331005", "GB0002634946", "DE0007164600", "SE0000108656", "ZZZINC000006"}) {
    CHECK(isValidIsin(isin));
    const auto key = instrumentKey(isin);
    CHECK(key && key->valid() && key->kind() == NodeKind::Instrument && key->value < kBelow62);
    CHECK(key && text(*key) == isin && describe(*key) == isin);
  }
}

TEST(the_smallest_and_the_largest_isins_fit_and_neither_is_zero) {
  const auto lowest = instrumentKey(isinOf("AA000000000"));
  const auto highest = instrumentKey(isinOf("ZZZZZZZZZZZ"));
  CHECK(lowest && highest);
  CHECK(lowest && lowest->valid() && lowest->identity() == 1);  // AA0000000000 is not the zero key
  CHECK(highest && highest->value < (uint64_t{1} << 57));       // under 2^56 values: well inside the sixty bits
  CHECK(lowest && highest && *lowest < *highest);
  CHECK(lowest && text(*lowest) == isinOf("AA000000000") && highest && text(*highest) == isinOf("ZZZZZZZZZZZ"));
}

TEST(a_hundred_thousand_isins_round_trip_and_keep_their_order) {
  std::mt19937_64 random(1234);
  const std::string alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::set<uint64_t> seen;
  for (int i = 0; i < 100000; i++) {
    std::string first11;
    first11 += static_cast<char>('A' + random() % 26);
    first11 += static_cast<char>('A' + random() % 26);
    for (int c = 0; c < 9; c++) first11 += alphabet[random() % 36];
    const std::string isin = isinOf(first11);
    const auto key = instrumentKey(isin);
    CHECK(key.has_value());
    if (!key) return;
    CHECK(text(*key) == isin);
    CHECK(key->kind() == NodeKind::Instrument && key->value < kBelow62);
    seen.insert(key->value);
  }
  CHECK(seen.size() > 99990);  // different ISINs, different keys (a handful of repeats in the random draw is allowed)
}

TEST(an_isin_that_is_not_valid_has_no_key) {
  CHECK(!instrumentKey("FI0009000680"));  // the check digit is wrong
  CHECK(!instrumentKey("fi0009000681"));  // lower case
  CHECK(!instrumentKey("FI000900068"));   // too short
  CHECK(!instrumentKey("FI00090006811"));
  CHECK(!instrumentKey(""));
  CHECK(!instrumentKey("1I0009000681"));  // a digit for a letter of the country
}

TEST(the_isins_we_make_ourselves_have_keys) {
  for (const char * name : {"Zinc", "Lead", "Gold", "OMXH25", "Brent"}) {
    const std::string isin = privateIsin(name);
    CHECK(!isin.empty() && isin.rfind("ZZ", 0) == 0);
    const auto key = instrumentKey(isin);
    CHECK(key && text(*key) == isin);
  }
}

TEST(codes_round_trip_with_their_case) {
  for (const char * code : {"EUR", "USD", "pts", "USDT", "BTC", "a", "Z", "0", "0123456789", "AbCdEfGhIj", "nordnet", "yahoo", "synthetic"}) {
    const auto currency = currencyKey(code);
    const auto dataset = datasetKey(code);
    CHECK(currency && dataset);
    if (!currency || !dataset) continue;
    CHECK(currency->kind() == NodeKind::Currency && dataset->kind() == NodeKind::Dataset);
    CHECK(currency->value < kBelow62 && dataset->value < kBelow62);
    CHECK(text(*currency) == code && text(*dataset) == code);
    CHECK(currency->identity() == dataset->identity() && !(*currency == *dataset));  // the same code, a different kind of node
  }
}

TEST(a_code_that_does_not_fit_has_no_key) {
  CHECK(!currencyKey(""));
  CHECK(!currencyKey("ABCDEFGHIJK"));  // eleven
  CHECK(!currencyKey("EU-R"));
  CHECK(!currencyKey("EU R"));
  CHECK(!currencyKey("EURO\xC3\xA9"));
  CHECK(!datasetKey("nord net"));
  CHECK(!datasetKey(std::string("a\0b", 3)));
  CHECK(currencyKey("ABCDEFGHIJ"));  // ten
}

TEST(different_codes_make_different_keys_and_a_longer_code_is_not_the_shorter) {
  CHECK(orNone(currencyKey("EUR")).valid() && !(orNone(currencyKey("EUR")) == orNone(currencyKey("EURO"))));
  CHECK(!(orNone(currencyKey("EUR")) == orNone(currencyKey("eur"))));
  CHECK(orNone(currencyKey("A")).valid() && !(orNone(currencyKey("A")) == orNone(currencyKey("A0"))));  // 0 is a character, and not the padding
  std::set<uint64_t> keys;
  const std::string alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  for (char a : alphabet)
    for (char b : alphabet) keys.insert(orNone(currencyKey(std::string{a, b})).value);
  CHECK(keys.size() == alphabet.size() * alphabet.size());
}

TEST(a_market_key_is_its_mic) {
  const NodeKey a = orNone(marketKey("XHEL"));
  CHECK(a.kind() == NodeKind::Market && a.valid() && a.value < kBelow62);
  CHECK(text(a) == "XHEL" && describe(a) == "XHEL");
  CHECK(orNone(marketKey("XHEL")) == a && !(orNone(marketKey("XSTO")) == a));
  CHECK(orNone(marketKey("XXXX")).valid() &&
        orNone(marketKey("XOFF")).valid() && orNone(marketKey("A1B2")).valid());
  // Four characters of A-Z and 0-9, nothing else.
  for (const char *bad : {"", "XHE", "XHELS", "xhel", "XH L", "XH-L", "ÅHEL"})
    CHECK(!marketKey(bad));
}

TEST(keys_of_different_kinds_never_meet) {
  const auto isin = instrumentKey("FI0009000681");
  const auto currency = currencyKey("EUR");
  const auto dataset = datasetKey("EUR");
  const NodeKey market = orNone(marketKey("XHEL"));
  CHECK(isin && currency && dataset);
  const std::set<uint64_t> all = {orNone(isin).value, orNone(currency).value, orNone(dataset).value, market.value};
  CHECK(all.size() == 4);
  CHECK(orNone(isin).kind() == NodeKind::Instrument && orNone(currency).kind() == NodeKind::Currency && market.kind() == NodeKind::Market &&
        orNone(dataset).kind() == NodeKind::Dataset);
}

TEST(a_portfolio_is_a_minted_key_of_kind_five_with_sixty_bits_of_identity) {
  const auto key = portfolioKey(0x123456789abcdefull);
  CHECK(key && key->valid() && key->kind() == NodeKind::Portfolio && key->identity() == 0x123456789abcdefull);
  CHECK(key && (key->value >> 60) == 5 && key->value < (uint64_t{1} << 63));  // bits 62-60 are 101, and bit 63 is never set
  CHECK(!portfolioKey(uint64_t{1} << 60));  // more than sixty bits would be another kind
  CHECK(portfolioKey(0) && portfolioKey(0)->valid());  // kind 5 makes even identity zero a key
  CHECK(portfolioKey(NodeKey::kIdentityMask)->value == ((uint64_t{5} << 60) | NodeKey::kIdentityMask));
  std::set<uint64_t> seen;
  for (int i = 0; i < 1000; i++) {
    const NodeKey minted = mintPortfolioKey();
    CHECK(minted.valid() && minted.kind() == NodeKind::Portfolio && minted.value < (uint64_t{1} << 63));
    seen.insert(minted.value);
  }
  CHECK(seen.size() == 1000);
}

TEST(a_portfolio_is_written_as_a_p_and_fifteen_hex_digits_and_read_back) {
  const NodeKey key = *portfolioKey(0x0abc00000000001ull);
  CHECK(text(key) == "P0abc00000000001" && describe(key) == text(key));
  CHECK(portfolioKey(text(key)) && *portfolioKey(text(key)) == key);
  for (int i = 0; i < 100; i++) {
    const NodeKey minted = mintPortfolioKey();
    CHECK(text(minted).size() == 16 && portfolioKey(text(minted)) == std::optional<NodeKey>(minted));
  }
  CHECK(!portfolioKey("P0abc0000000000") && !portfolioKey("p0abc00000000001") && !portfolioKey("P0abc0000000000g") && !portfolioKey("FI0009000681"));
}

TEST(the_kinds_that_are_planned_and_the_escape_have_no_text_and_the_built_ones_are_as_they_were) {
  for (uint64_t kind : {4u, 6u, 7u}) CHECK(text(NodeKey{(kind << 60) | 1}).empty());  // country, attribute, the escape: not built
  CHECK(currencyKey("EUR")->value >> 60 == 1 && marketKey("XHEL")->value >> 60 == 2 && datasetKey("nordnet")->value >> 60 == 3);
  CHECK(instrumentKey("FI0009000681")->value >> 60 == 0);
}
