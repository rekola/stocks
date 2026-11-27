#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "source/SuomenPankkiSource.h"

#include <clocale>
#include <fstream>
#include <sstream>

namespace {

std::string fixture() {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/suomenpankki/euribor.xml", std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// The edge of a rate by its ISIN, on the market XXXX of the dataset.
std::optional<EdgeKey> edgeOf(const Snapshot & s, const char * isin) {
  auto base = s.findInstrumentByIsin(isin);
  auto quote = s.findInstrument("pct");
  auto market = s.findMarket("XXXX");
  auto dataset = s.findDataset(kSuomenPankkiDataset);
  if (!base || !quote || !market || !dataset) return std::nullopt;
  return EdgeKey{*base, *quote, *market, *dataset};
}

constexpr const char * kEuribor3M = "EU0009652783", * kEuribor1W = "EU0009678507";

}  // namespace

TEST(the_report_gives_each_rate_as_daily_bars_in_per_cent_oldest_first) {
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(fixture().compare(0, 3, "\xEF\xBB\xBF") == 0);  // the real answer starts with a byte order mark
  CHECK(SuomenPankkiSource::parse(fixture(), *store, error, notes));
  CHECK(notes.empty());
  auto s = store->snapshot();
  for (const char * isin : {"EU0009678507", "EU0009659937", "EU0009652783", "EU0009652791", "EU0009652809"}) CHECK(edgeOf(*s, isin));

  auto edge = edgeOf(*s, kEuribor1W);
  CHECK(edge);
  CHECK(s->kind(edge->base) == InstrumentKind::Index);
  CHECK(s->name(edge->base) == "Euribor 1W" && s->symbol(*edge) == "EURIBOR1W");
  CHECK(s->kind(edge->quote) == InstrumentKind::Unit);
  const auto bars = s->candles(*edge, Resolution::Day).candles;
  CHECK(bars.size() == 4);
  // Newest first in the report, oldest first in the store; the comma is a decimal point and a rate can be negative.
  CHECK(bars[0].time == dayNumber(2021, 1, 4) && bars[0].close == -0.579);
  CHECK(bars[3].time == dayNumber(2026, 11, 20) && bars[3].close == 2.390);
  CHECK(std::isnan(bars[3].open) && std::isnan(bars[3].volume));
  CHECK(s->coverage(*edge, Resolution::Day)->kind == QueueKind::Scalar);  // a fixing: one value a day
}

TEST(a_day_without_a_rate_is_left_out_and_the_ended_eonia_is_not_a_rate_of_ours) {
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(SuomenPankkiSource::parse(fixture(), *store, error, notes));
  auto s = store->snapshot();
  // 2022-03-10 has no 3-month rate (the bank leaves it empty), and the other rates of that day are there.
  CHECK(s->candles(*edgeOf(*s, kEuribor3M), Resolution::Day).candles.size() == 3);
  CHECK(s->candles(*edgeOf(*s, "EU0009652809"), Resolution::Day).candles.size() == 4);
  CHECK(s->nodeCount(NodeType::Instrument) == 6);  // five rates and the unit
}

TEST(what_cannot_be_read_is_skipped_and_said_not_stored) {
  std::string xml = fixture();
  auto replace = [&](const std::string & from, const std::string & to) { xml.replace(xml.find(from), from.size(), to); };
  replace("1 kk (tod.pv/360)\"><intr value=\"2,484\"", "1 kk (tod.pv/360)\"><intr value=\"n/a\"");
  replace("6 kk (tod.pv/360)\"><intr value=\"3,071\"", "9 kk (tod.pv/360)\"><intr value=\"3,071\"");
  replace("period value=\"2026-11-19\"", "period value=\"yesterday\"");
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(SuomenPankkiSource::parse(xml, *store, error, notes));
  CHECK(notes.size() == 3);
  auto s = store->snapshot();
  CHECK(s->candles(*edgeOf(*s, "EU0009659937"), Resolution::Day).candles.size() == 2);  // 1 month: 4 days, minus the 2 bad
  CHECK(s->candles(*edgeOf(*s, "EU0009652791"), Resolution::Day).candles.size() == 2);  // 6 months: 3 days, minus the 9 kk, minus the bad day
}

TEST(an_answer_that_is_not_a_report_is_refused_and_writes_nothing) {
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  CHECK(!SuomenPankkiSource::parse("<html>maintenance</html>", *store, error, notes) && error == "no rates");
  CHECK(!SuomenPankkiSource::parse("not xml at all <", *store, error, notes) && error.rfind("not XML", 0) == 0);
  CHECK(store->snapshot()->nodeCount(NodeType::Instrument) == 0);
}

TEST(loading_asks_once_for_the_whole_history_and_again_changes_nothing_new) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture()); };
  Listener listener;
  SuomenPankkiSource source;
  source.load(*store, http, listener);
  CHECK(http.urls.size() == 1 && http.urls[0].rfind("https://reports.suomenpankki.fi/", 0) == 0);
  CHECK(listener.changes == 1 && listener.messages.empty());
  source.load(*store, http, listener);
  auto s = store->snapshot();
  CHECK(s->candles(*edgeOf(*s, kEuribor3M), Resolution::Day).candles.size() == 3);  // a bar of a day already there is replaced, not added
  CHECK(source.datasetCodes() == std::vector<std::string>{kSuomenPankkiDataset});
  CHECK(source.refreshInterval() > std::chrono::milliseconds(0));
}

TEST(a_refusal_stops_the_load_and_a_failure_is_said) {
  for (int status : {403, 429, 500, 0}) {
    auto store = makeMemoryStore();
    FakeHttp http;
    http.answer = [status](const std::string &) { return failed(status); };
    Listener listener;
    SuomenPankkiSource().load(*store, http, listener);
    CHECK(listener.changes == 0 && listener.messages.size() == 1);
    CHECK(store->snapshot()->nodeCount(NodeType::Instrument) == 0);
  }
}

// A window toolkit sets the locale; in one with a decimal comma strtod read "-0.549" as -0 and stopped. (Where no such locale is installed the
// test has nothing to prove and passes.)
TEST(the_rates_are_read_whatever_the_locale) {
  const char * original = std::setlocale(LC_NUMERIC, nullptr);
  const std::string saved = original ? original : "C";
  bool set = false;
  for (const char * name : {"fi_FI.UTF-8", "fi_FI.utf8", "de_DE.UTF-8", "de_DE.utf8"})
    if (std::setlocale(LC_NUMERIC, name)) {
      set = true;
      break;
    }
  auto store = makeMemoryStore();
  std::string error;
  std::vector<std::string> notes;
  const bool ok = SuomenPankkiSource::parse(fixture(), *store, error, notes);
  std::setlocale(LC_NUMERIC, saved.c_str());
  CHECK(ok);
  CHECK(notes.empty());
  (void)set;
}
