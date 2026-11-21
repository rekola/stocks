#include "TestFramework.h"

#include "source/InstrumentTypes.h"

TEST(nordnet_types_are_mapped_onto_the_one_vocabulary_and_certificates_are_named) {
  CHECK(kindOfNordnetType("UETF") == InstrumentKind::Etf && kindOfNordnetType("KETF") == InstrumentKind::Etf);
  CHECK(kindOfNordnetType("PTC") == InstrumentKind::Certificate);
  CHECK(kindOfNordnetType("") == InstrumentKind::Other && kindOfNordnetType("SOMETHING_NEW") == InstrumentKind::Other);
}

TEST(yahoo_types_are_mapped_onto_the_same_vocabulary) {
  CHECK(kindOfYahooType("EQUITY") == InstrumentKind::Company && kindOfYahooType("ETF") == InstrumentKind::Etf);
  CHECK(kindOfYahooType("INDEX") == InstrumentKind::Index && kindOfYahooType("FUTURE") == InstrumentKind::Commodity);
  CHECK(kindOfYahooType("CURRENCY") == InstrumentKind::Currency);
  CHECK(kindOfYahooType("") == InstrumentKind::Other && kindOfYahooType("MUTUALFUND") == InstrumentKind::Other);
}

TEST(an_etf_is_an_etf_from_either_provider) {
  CHECK(kindOfNordnetType("UETF") == kindOfYahooType("ETF"));
}
