#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "source/EcbSource.h"

#include <fstream>
#include <sstream>

namespace {

std::string fixture() {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/ecb/est.xml", std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// The edge of a series by its ISIN and the unit it is quoted in, on the market XXXX of the dataset.
std::optional<EdgeKey> edgeOf(const Snapshot & s, const char * isin, const char * unit) {
  auto base = s.findInstrumentByIsin(isin);
  auto quote = s.findInstrument(unit);
  auto market = s.findMarket("XXXX");
  auto dataset = s.findDataset(kEcbDataset);
  if (!base || !quote || !market || !dataset) return std::nullopt;
  return EdgeKey{*base, *quote, *market, *dataset};
}

constexpr const char * kEstr = "EU000A2X2A25", * kIndex = "EU000A2QQF08";

}  // namespace

TEST(the_euro_short_term_rate_is_a_rate_in_per_cent_and_its_compounded_index_is_in_points) {
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(EcbSource::parse(fixture(), *store, error, notes));
  CHECK(notes.empty());
  auto s = store->snapshot();

  auto rate = edgeOf(*s, kEstr, "pct");
  CHECK(rate);
  CHECK(s->kind(rate->base) == InstrumentKind::Index && s->name(rate->base) == "Euro short-term rate (€STR)" && s->symbol(*rate) == "ESTR");
  const auto rates = s->candles(*rate, Resolution::Day).candles;
  CHECK(rates.size() == 4);
  CHECK(rates[0].time == dayNumber(2019, 10, 1) && rates[0].close == -0.549);  // negative: the rate began below zero
  CHECK(rates[3].time == dayNumber(2026, 11, 20) && rates[3].close == 2.444);
  CHECK(std::isnan(rates[3].open) && std::isnan(rates[3].volume));

  auto index = edgeOf(*s, kIndex, "pts");
  CHECK(index);
  CHECK(s->symbol(*index) == "ESTRINDEX");
  const auto points = s->candles(*index, Resolution::Day).candles;
  CHECK(points.size() == 5);
  CHECK(points[0].close == 100 && points[0].time == dayNumber(2019, 10, 1));
  CHECK(points[4].time == dayNumber(2026, 11, 23) && points[4].close == 110.2938007);  // a day the rate has not been published for yet
}

TEST(a_series_that_was_not_asked_for_is_ignored) {
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(EcbSource::parse(fixture(), *store, error, notes));  // the fixture holds a third series, the 1-week compounded average
  auto s = store->snapshot();
  CHECK(!s->findInstrumentByIsin("EU000A2QQF16"));
  CHECK(s->nodeCount(NodeType::Instrument) == 4);  // the two series and their two units
}

TEST(an_observation_that_cannot_be_read_is_skipped_and_said) {
  std::string xml = fixture();
  auto replace = [&](const std::string & from, const std::string & to) { xml.replace(xml.find(from), from.size(), to); };
  replace("TIME_PERIOD=\"2019-10-02\" OBS_VALUE=\"-0.551\"", "TIME_PERIOD=\"2019-10-02\" OBS_VALUE=\"\"");
  replace("TIME_PERIOD=\"2026-11-19\" OBS_VALUE=\"2.441\"", "TIME_PERIOD=\"soon\" OBS_VALUE=\"2.441\"");
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(EcbSource::parse(xml, *store, error, notes));
  CHECK(notes.size() == 1 && notes[0].rfind("2 observations", 0) == 0);
  auto s = store->snapshot();
  CHECK(s->candles(*edgeOf(*s, kEstr, "pct"), Resolution::Day).candles.size() == 2);
}

TEST(an_answer_with_none_of_the_series_is_refused_and_writes_nothing) {
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(!EcbSource::parse("<message:StructureSpecificData xmlns:message=\"urn:x\"/>", *store, error, notes) && error == "no series");
  CHECK(!EcbSource::parse("not xml at all <", *store, error, notes) && error.rfind("not XML", 0) == 0);
  CHECK(store->snapshot()->nodeCount(NodeType::Instrument) == 0);
}

TEST(loading_asks_once_for_both_series_and_again_adds_nothing) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture()); };
  Listener listener;
  EcbSource source;
  source.load(*store, http, listener);
  CHECK(http.urls.size() == 1 && http.urls[0].rfind("https://data-api.ecb.europa.eu/service/data/EST/", 0) == 0);
  CHECK(http.urls[0].find(kEstr) != std::string::npos && http.urls[0].find(kIndex) != std::string::npos);
  CHECK(listener.changes == 1 && listener.messages.empty());
  source.load(*store, http, listener);
  auto s = store->snapshot();
  CHECK(s->candles(*edgeOf(*s, kEstr, "pct"), Resolution::Day).candles.size() == 4);  // a bar of a day already there is replaced
  CHECK(source.datasetCodes() == std::vector<std::string>{kEcbDataset});
  CHECK(source.refreshInterval() > std::chrono::milliseconds(0));
}

TEST(a_refusal_stops_the_load_and_a_failure_is_said) {
  for (int status : {403, 429, 500, 0}) {
    auto store = makeMemoryStore();
    FakeHttp http;
    http.answer = [status](const std::string &) { return failed(status); };
    Listener listener;
    EcbSource().load(*store, http, listener);
    CHECK(listener.changes == 0 && listener.messages.size() == 1);
    CHECK(store->snapshot()->nodeCount(NodeType::Instrument) == 0);
  }
}
