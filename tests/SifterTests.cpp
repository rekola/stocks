#include "TestFramework.h"

#include "model/Dates.h"
#include "source/SifterSource.h"

#include <fstream>
#include <sstream>

namespace {

std::string fixture() {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/sifter/dataservice.json", std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

std::vector<Candle> navs(const Snapshot & s, const char * isin) {
  auto base = s.findInstrumentByIsin(isin);
  auto quote = s.findInstrument("EUR");
  auto market = s.findMarket("XOFF");
  auto dataset = s.findDataset(kSifterDataset);
  if (!base || !quote || !market || !dataset) return {};
  return s.candles(EdgeKey{*base, *quote, *market, *dataset}, Resolution::Day).candles;
}

}  // namespace

TEST(each_share_class_is_a_fund_with_the_navs_of_its_member) {
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(SifterSource::parse(fixture(), *store, error, notes));
  CHECK(notes.empty());
  auto s = store->snapshot();
  const auto pa = navs(*s, "LU0168736675"), pb = navs(*s, "LU0168577939"), pi = navs(*s, "LU1194076995");
  CHECK(pa.size() == 3 && pa[0].time == dayNumber(2026, 11, 18) && pa[2].close == 102.0);
  CHECK(pb.size() == 3 && pb[0].close == 98.5);  // a number in a string is read
  CHECK(pi.size() == 2 && pi[1].time == dayNumber(2026, 11, 20));  // a day it has none is not a bar
  CHECK(s->kind(*s->findInstrumentByIsin("LU0168736675")) == InstrumentKind::Fund);
  CHECK(s->name(*s->findInstrumentByIsin("LU1194076995")) == "Sifter Fund Global PI");
}

TEST(a_value_that_cannot_be_read_is_skipped_and_said) {
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(SifterSource::parse(R"([{"d":"2026-11-18","cr":0,"c":99.5},{"d":"soon","c":1},{"d":"2026-11-19","cr":"x"}])", *store, error, notes));
  CHECK(notes.size() == 1 && notes[0].rfind("3 values", 0) == 0);
  auto s = store->snapshot();
  CHECK(navs(*s, "LU0168577939").size() == 1);
  CHECK(navs(*s, "LU0168736675").empty());
}

TEST(an_answer_that_is_not_a_list_of_days_is_refused) {
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(!SifterSource::parse("<html>", *store, error, notes) && error == "not a list of days");
  CHECK(!SifterSource::parse("[]", *store, error, notes) && error == "no prices");
}
