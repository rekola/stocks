#include "TestFramework.h"

#include "TestRegistry.h"
#include "model/Isin.h"
#include "model/IsinRegistry.h"
#include "model/Store.h"

#include <set>

namespace {

IsinRegistry parsed(const std::string & text, std::string * why = nullptr) {
  IsinRegistry r;
  std::string error;
  const bool ok = IsinRegistry::parse(text, r, error);
  if (why) *why = ok ? "" : error;
  return r;
}

std::string whyNot(const std::string & text) {
  std::string why;
  parsed(text, &why);
  return why;
}

}  // namespace

TEST(a_registry_is_read_and_a_thing_is_found_by_its_symbol_and_else_by_its_name) {
  const IsinRegistry r = parsed("# a comment\n\nXC0005705584,ZINC,,Zinc, a commodity; with a comma\nZZ0000000016,,Some thing,only a name\n");
  CHECK(r.entries().size() == 2 && r.entries()[0].note == "Zinc, a commodity; with a comma" && r.entries()[0].line == 3);
  CHECK(r.find("ZINC", "") == "XC0005705584" && r.find("ZINC", "anything") == "XC0005705584");  // the symbol first
  CHECK(r.find("", "Some thing") == "ZZ0000000016" && r.find("OTHER", "Some thing") == "ZZ0000000016");  // else the name
  CHECK(r.find("zinc", "").empty() && r.find("", "").empty() && r.find("NONE", "none").empty());  // written exactly
}

TEST(a_row_that_is_wrong_or_that_collides_is_refused_and_the_line_is_said) {
  CHECK(whyNot("XC0005705584,ZINC,,a\nXC0005705585,LEAD,,b\n").find("line 2: XC0005705585 is not a valid ISIN (its check digit should be 4)") == 0);
  CHECK(whyNot("XC0005705584,ZINC,,a\nXC0005705584,LEAD,,b\n").find("line 2: the ISIN XC0005705584 is already in the registry, at line 1") == 0);
  CHECK(whyNot("XC0005705584,ZINC,,a\nXC0005705527,ZINC,,b\n").find("line 2: the symbol ZINC is already in the registry, at line 1") == 0);
  CHECK(whyNot("XC0005705584,ZINC,Zinc,a\nXC0005705527,LEAD,Zinc,b\n").find("line 2: the name Zinc is already in the registry, at line 1") == 0);
  CHECK(whyNot("XC0005705584,ZINC,LEAD,a\nXC0005705527,LEAD,,b\n").find("line 2: the symbol LEAD is the name of the row at line 1") == 0);
  CHECK(whyNot("XC0005705584,ZINC,,a\nXC0005705527,,ZINC,b\n").find("line 2: the name ZINC is the symbol of the row at line 1") == 0);
  CHECK(whyNot("XC0005705584,,,a\n").find("line 1: a row names nothing") == 0);
  CHECK(whyNot("XC0005705584,ZINC\n").find("line 1: a row is isin,symbol,name,note") == 0);
  CHECK(whyNot("XC0005705584,ZINC,,a\r\nXC0005705527,LEAD,,b\r\n").empty());  // a file with Windows line ends
  CHECK(whyNot("").empty());
}

TEST(the_registry_the_program_ships_with_is_valid_and_its_private_isins_are_numbered_without_gaps_or_repeats) {
  std::string error;
  IsinRegistry r;
  CHECK(loadIsinRegistry(r, error));  // valid ISINs, no duplicate ISIN, symbol or name, no symbol that is a name
  if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
  CHECK(r.entries().size() >= 8);
  std::set<int> numbers;
  for (const IsinRegistry::Entry & e : r.entries()) {
    if (e.isin.rfind("ZZ", 0) != 0) continue;
    // Ours: ZZ, nine digits, the check digit. The numbers go from 1 and none is left out, so the next is one more than the last.
    bool digits = true;
    for (size_t i = 2; i < 11; i++) digits = digits && e.isin[i] >= '0' && e.isin[i] <= '9';
    CHECK(digits);
    numbers.insert(std::stoi(e.isin.substr(2, 9)));
  }
  CHECK(!numbers.empty() && *numbers.begin() == 1 && *numbers.rbegin() == static_cast<int>(numbers.size()));
  // What the notes say there is: zinc and lead have their real ISINs, which Nordnet does not give.
  CHECK(r.find("ZINC", "") == "XC0005705584" && r.find("LEAD", "") == "XC0005705527");
  for (const IsinRegistry::Entry & e : r.entries()) CHECK(!e.note.empty());
}

TEST(a_store_with_a_registry_keeps_what_has_no_isin_by_it_and_refuses_the_rest_and_says_so) {
  std::vector<std::string> told;
  StoreOptions options;
  options.isins = shippedRegistry();
  options.notice = [&](const std::string & text) { told.push_back(text); };
  auto store = makeMemoryStore(std::move(options));
  const InstrumentId zinc = store->resolveInstrument({}, {{predicate::symbol, "ZINC"}, {predicate::name, "Sinkki"}, {predicate::kind, "commodity"}});
  const InstrumentId bitcoin = store->addInstrument("BTC", "Bitcoin", InstrumentKind::Other);
  CHECK(zinc.valid() && store->snapshot()->isin(zinc) == "XC0005705584" && store->snapshot()->findInstrumentByIsin("XC0005705584"));
  CHECK(bitcoin.valid() && store->snapshot()->isin(bitcoin) == "ZZ0000000024");
  CHECK(told.empty());
  // Not in the registry: not kept, and told once for each.
  const uint64_t version = store->snapshot()->version();
  CHECK(!store->addInstrument("NEWCOIN", "New coin", InstrumentKind::Other).valid() && !store->addInstrument("NEWCOIN", "New coin", InstrumentKind::Other).valid());
  CHECK(!store->snapshot()->findInstrument("NEWCOIN") && store->snapshot()->version() == version);
  CHECK(told.size() == 1 && told[0] == "instrument: NEWCOIN has no ISIN and none is registered for it (data/isins/registry.csv), so it is not kept");
  // One with an ISIN of its own, and a currency, are not looked up.
  CHECK(store->addInstrument("", "Nokia", InstrumentKind::Company, "FI0009000681").valid() && store->addInstrument("EUR", "Euro", InstrumentKind::Currency).valid());
  CHECK(told.size() == 1);
}

TEST(a_store_with_no_registry_numbers_what_has_no_isin_from_the_top_of_the_private_namespace) {
  auto store = makeMemoryStore();
  const InstrumentId a = store->addInstrument("A", "Alpha", InstrumentKind::Company), b = store->addInstrument("B", "Beta", InstrumentKind::Company);
  const std::string first = store->snapshot()->isin(a), second = store->snapshot()->isin(b);
  CHECK(first.rfind("ZZ999999999", 0) == 0 && second.rfind("ZZ999999998", 0) == 0 && isValidIsin(first) && isValidIsin(second));
  CHECK(store->addInstrument("A", "Alpha again", InstrumentKind::Company) == a);  // the same text: the same number
}
