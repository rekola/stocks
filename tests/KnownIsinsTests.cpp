#include "TestFramework.h"

#include "model/Isin.h"
#include "model/Store.h"
#include "source/GoldSource.h"
#include "source/KnownIsins.h"

#include <set>

// The static_asserts of KnownIsins.h are the real check: a wrong or repeated ISIN does not compile. These say that what the compiler checks is what
// the program checks at run time, and what the store does about an instrument that comes with none.

TEST(the_compile_time_check_of_an_isin_is_the_one_the_program_makes_at_run_time) {
  for (const char * isin : {"DE0008469008", "US78378X1072", "RU000A0JP7K5", "XC0009655157", "FI0009000681", "US12573F1084", "XC0005705584", "ZZ0000000016"}) {
    CHECK(known_isin::valid(isin) == isValidIsin(isin) && isValidIsin(isin));
  }
  for (const char * bad : {"DE0008469009", "DE000846900", "", "de0008469008", "1E0008469008", "DE000846900A", "XC0005705585", "ZZ0000000017"}) {
    CHECK(!known_isin::valid(bad) && !isValidIsin(bad));
  }
  CHECK(known_isin::same("A", "A") && !known_isin::same("A", "AB") && !known_isin::same("AB", "A") && known_isin::same("", ""));
}

TEST(every_isin_the_adapters_supply_is_valid_different_and_the_private_ones_are_numbered_from_one) {
  std::set<std::string> seen;
  for (const char * isin : known_isin::kReal) CHECK(isValidIsin(isin) && seen.insert(isin).second && std::string(isin).rfind("ZZ", 0) != 0);
  int number = 0;
  for (const char * isin : known_isin::kPrivate) {
    CHECK(isValidIsin(isin) && seen.insert(isin).second);
    CHECK(std::stoi(std::string(isin).substr(2, 9)) == ++number);
  }
  CHECK(number >= 5);
  // The ones the physical items name.
  CHECK(std::string(known_isin::kBitcoin) == "ZZ0000000016" && std::string(known_isin::kZinc) == "XC0005705584" && std::string(known_isin::kLead) == "XC0005705527");
  CHECK(std::string(known_isin::kBricIndia) == "US26075E1047");
}

TEST(a_store_told_not_to_invent_isins_refuses_what_has_none_and_says_so_once) {
  std::vector<std::string> told;
  StoreOptions options;
  options.invent_isins = false;
  options.notice = [&](const std::string & text) { told.push_back(text); };
  auto store = makeMemoryStore(std::move(options));
  const uint64_t version = store->snapshot()->version();
  CHECK(!store->addInstrument("NEWCOIN", "New coin", InstrumentKind::Other).valid() && !store->addInstrument("NEWCOIN", "New coin", InstrumentKind::Other).valid());
  CHECK(!store->snapshot()->findInstrument("NEWCOIN") && store->snapshot()->version() == version);
  CHECK(told.size() == 1 && told[0] == "instrument: NEWCOIN has no ISIN, so it is not kept");
  // One with an ISIN of its own, and a currency, are not affected.
  CHECK(store->addInstrument("", "Nokia", InstrumentKind::Company, "FI0009000681").valid() && store->addInstrument("EUR", "Euro", InstrumentKind::Currency).valid());
  CHECK(store->addInstrument("", "Zinc", InstrumentKind::Commodity, known_isin::kZinc).valid() && told.size() == 1);
}

TEST(a_store_that_may_invent_numbers_what_has_no_isin_from_the_top_of_the_private_namespace) {
  auto store = makeMemoryStore();
  const InstrumentId a = store->addInstrument("A", "Alpha", InstrumentKind::Company), b = store->addInstrument("B", "Beta", InstrumentKind::Company);
  const std::string first = store->snapshot()->isin(a), second = store->snapshot()->isin(b);
  CHECK(first.rfind("ZZ999999999", 0) == 0 && second.rfind("ZZ999999998", 0) == 0 && isValidIsin(first) && isValidIsin(second));
  CHECK(store->addInstrument("A", "Alpha again", InstrumentKind::Company) == a);  // the same text: the same number
}

TEST(a_physical_item_with_no_isin_in_the_header_is_refused_with_what_to_do) {
  GoldTable table;
  std::string error;
  CHECK(!parseGoldTable("spot,gold,XAU\nitem,gold,NEWBAR,New bar,1,1,1\n", table, error));
  CHECK(error.find("line 2: the item NEWBAR has no ISIN: add one to source/KnownIsins.h") == 0);
  CHECK(parseGoldTable("spot,gold,XAU\nitem,gold,GOLDBAR,Gold bar,1,1,1\nitem,platinum,PLATBAR,Platinum bar,1,1,1\n", table, error) == false);  // no spot for platinum yet
  CHECK(parseGoldTable("spot,gold,XAU\nspot,platinum,XPT\nitem,gold,GOLDBAR,Gold bar,1,1,1\nitem,platinum,PLATBAR,Platinum bar,1,1,1\n", table, error));
  CHECK(table.items.size() == 2 && table.items[0].isin == known_isin::kGoldBar && table.items[1].isin == known_isin::kPlatinumBar);
}
