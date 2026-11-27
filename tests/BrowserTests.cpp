#include "TestFramework.h"

#include "AttributeView.h"
#include "Browser.h"
#include "model/Dates.h"
#include "News.h"
#include "Quotes.h"
#include "Positions.h"
#include "portfolio/Portfolio.h"
#include "source/DatasetCodes.h"
#include "Synthetic.h"
#include "TransactionsView.h"
#include "model/Dates.h"
#include "source/NordnetHistory.h"
#include "source/Source.h"
#include "source/InstrumentLists.h"

#include <fstream>
#include <sstream>
#include <cellui/Application.h>
#include <cellui/TestBackend.h>

#include <clocale>

using namespace cellui;

TEST(a_trace_is_as_bright_as_its_bars_volume_against_the_periods_usual_and_medium_with_no_volume) {
  Seq<Candle> bars;
  for (double volume : {100.0, 100.0, 100.0, 0.0, 800.0, kUndefined}) bars = std::move(bars).push_back({1, 1, 1, 1, 1, volume});
  const std::vector<float> light = activity(bars);
  CHECK(light.size() == 6);
  CHECK(light[0] == 0.25f);       // the usual volume: bright, not past white
  CHECK(light[3] == 0.0f);        // none: medium
  CHECK(light[4] == 1.0f);        // abnormal: the brightest, lit past white
  CHECK(light[5] == 0.0f);        // not defined counts as none
  const Seq<Candle> silent = Seq<Candle>().push_back({1, 1, 1, 1, 1, kUndefined});
  CHECK(activity(silent).empty());
}

namespace {

// A row that rose over the three months and fell today: daily bars from 10 to 21 up to 30 November, today's 1 December
// bar at 20, and today's three 5-minute bars after a close of 21 yesterday. Both are on the Currencies preset.
constexpr int64_t kToday = dayNumber(2026, 12, 1);
int64_t fiveMinuteSlot(int64_t day, int k) { return slotOfInstant(Resolution::Min5, (day - kUnixEpochDayNumber) * 86400 + 7 * 3600 + k * 300); }
struct Rows {
  EdgeKey riseFall, flat;
};
Rows periodWorld(Store & store) {
  InstrumentId usd = store.addInstrument("USD", "US dollar", InstrumentKind::Currency);
  InstrumentId eur = store.addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId gbp = store.addInstrument("GBP", "British pound", InstrumentKind::Currency);
  MarketId currencies = store.addMarket("XXXX", "Currencies");
  DatasetId a = store.addDataset("providera");
  Rows rows{{eur, usd, currencies, a}, {gbp, usd, currencies, a}};
  std::vector<Candle> days;
  for (int d = 70; d >= 1; d--) days.push_back({kToday - d, 1, 1, 1, 21.0 - d * 11.0 / 70.0 + (d == 1 ? 0.0 : 0.0), 1});
  days.back().close = 21;  // yesterday
  days.push_back({kToday, 1, 1, 1, 20, 1});
  store.appendCandles(rows.riseFall, Resolution::Day, days);
  store.appendCandles(rows.riseFall, Resolution::Min5, {{fiveMinuteSlot(kToday, 0), 1, 1, 1, 20.5, 1}, {fiveMinuteSlot(kToday, 1), 1, 1, 1, 20.2, 1}, {fiveMinuteSlot(kToday, 2), 1, 1, 1, 20, 1}});
  // A second row with a single daily bar: no period has bars for it yet but today's price.
  store.appendCandles(rows.flat, Resolution::Day, {{kToday, 1, 1, 1, 5, 1}});
  return rows;
}

// The lists on disk, which the made-up data follows.
const InstrumentList & instrumentList() {
  static const InstrumentList u = [] {
    std::string error;
    InstrumentList loaded = loadInstrumentLists(std::filesystem::path(STOCKS_DATA_DIR) / "lists", error);
    if (loaded.empty()) std::fprintf(stderr, "  lists: %s\n", error.c_str());
    return loaded;
  }();
  return u;
}

size_t countOf(const std::string &mic,
               std::optional<InstrumentKind> kind = std::nullopt) {
  size_t n = 0;
  for (const ListEntry &e : instrumentList())
    n += e.mic == mic && (!kind || e.kind == *kind);
  return n;
}

struct Fixture {
  test::TestBackend * backend;
  std::unique_ptr<Application> app;
  std::shared_ptr<Store> store = makeMemoryStore();
  std::unique_ptr<Browser> browser;
  // Over the made-up data, or over a store the test has filled.
  explicit Fixture(bool synthetic = true, int cols = 110, KeymapProfile profile = KeymapProfile::TERMINAL) {
    if (synthetic) fillSyntheticData(*store, instrumentList());
    DisplayInfo display;
    display.keymap_profile = profile;
    auto owned = std::make_unique<test::TestBackend>(24, cols, display);
    backend = owned.get();
    app = Application::adopt(std::move(owned));
    browser = std::make_unique<Browser>(*app, store);
    browser->setMinRefreshInterval(0);  // a test posts one change after another, and looks at once
  }
  void press(int key, unsigned mods = 0) {
    app->handle(BackendEvent::makeInput(keyInput(key, mods, Input::Kind::PRESS)));
    app->present();
  }
  void type(const std::string & s) {
    for (char c : s) press(c);
  }
  void click(int row, int col) {
    for (auto kind : {Input::Kind::PRESS, Input::Kind::RELEASE}) {
      app->handle(BackendEvent::makeInput(Input(Key::Button1, row, col, false, false, false, false, kind)));
      app->present();
    }
  }
  std::string text() {
    app->present();
    return backend->text();
  }
  // Runs `open` and, inside the modal it opens, one of `steps` a turn. True if the modal ended on its own (Escape) before
  // the steps ran out; the loop is quit under it otherwise.
  bool inModal(const std::function<void()> & open, std::vector<std::function<void()>> steps) {
    EventLoop & loop = backend->eventLoop();
    loop.setBefore([] { return 0; });
    size_t turn = 0;
    bool ran_out = false;
    auto frame = loop.onFrame([&] {
      if (turn < steps.size()) {
        steps[turn++]();
      } else {
        ran_out = true;
        loop.quit();
      }
    });
    open();
    return !ran_out;
  }
};

// The menu button at the end of a row: the hamburger.
constexpr const char * kMenuGlyphText = "\xE2\x89\xA1";

// Where `needle` first is on the screen, in cells: {row, column}, or {-1, -1}.
std::pair<int, int> cellOf(const std::string & screen, const std::string & needle) {
  int row = 0, col = 0;
  for (size_t i = 0; i < screen.size(); i++) {
    if (screen.compare(i, needle.size(), needle) == 0) return {row, col};
    if (screen[i] == '\n') row++, col = 0;
    else if ((static_cast<unsigned char>(screen[i]) & 0xC0) != 0x80) col++;  // not a continuation byte
  }
  return {-1, -1};
}

// A company with an ISIN, a listing, a real number and a day number, on the Nasdaq Helsinki preset.
EdgeKey addNokia(Store & store) {
  InstrumentId eur = store.addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId nokia = store.addInstrument("", "Nokia Oyj", InstrumentKind::Company, "FI0009000681");
  MarketId helsinki = store.addMarket("XHEL", "Nasdaq Helsinki");
  DatasetId data = store.addDataset(kNordnetDataset);
  EdgeKey key{nokia, eur, helsinki, data};
  store.appendCandles(key, Resolution::Day, {{dayNumber(2026, 1, 2), 1, 1, 1, 4.2, 1}, {dayNumber(2026, 1, 5), 1, 1, 1, 4.3, 1}});
  store.setListing({key, "NOKIA", "1234", "5678", "STOCK", "Nokia"});
  store.setAttributes(nokia, {{"market-cap", 12345.675}, {"report-day", dayNumber(2026, 3, 5)}, {"owners", int64_t{250000}}});
  return key;
}

}  // namespace

TEST(every_command_has_a_description) {
  Fixture f;
  const CommandRegistry & commands = f.app->commands();
  CHECK(commands.names().size() > 1u);
  for (const std::string & name : commands.names()) {
    if (commands.description(name).empty()) std::fprintf(stderr, "  no description: %s\n", name.c_str());
    CHECK(!commands.description(name).empty());
  }
}

TEST(meta_x_opens_the_palette_and_a_chosen_command_runs) {
  Fixture f;
  CHECK(!f.app->commandPaletteOpen());
  f.press('x', kModAlt);
  CHECK(f.app->commandPaletteOpen());
  CHECK(f.text().find("focus-tags") != std::string::npos);  // the list is as tall as the screen allows
  f.type("show-curr");
  CHECK(f.text().find("show-currencies") != std::string::npos && f.text().find("show-commodities") == std::string::npos);
  f.press(Key::Enter);
  CHECK(!f.app->commandPaletteOpen());
  CHECK(f.browser->tags().selected() == 2);
}

TEST(ctrl_shift_p_opens_the_palette_on_a_desktop) {
  Fixture f(true, 100, KeymapProfile::DESKTOP);
  f.press('p', kModCtrl | kModShift);
  CHECK(f.app->commandPaletteOpen());
}

TEST(the_palette_takes_every_key_while_open_and_escape_closes_it_at_once_without_running) {
  Fixture f;
  f.press('x', kModAlt);
  f.press('q');  // typed into the query, not a quit
  CHECK(f.app->commandPaletteOpen());
  f.press(Key::Escape);  // at once, though a terminal's bare Escape otherwise waits for the key after it
  CHECK(!f.app->commandPaletteOpen());
  CHECK(f.browser->tags().selected() == 0);  // nothing ran
  f.press('x', kModAlt);
  f.press('g', kModCtrl);  // Emacs's cancel
  CHECK(!f.app->commandPaletteOpen());
}

TEST(the_palette_and_the_help_line_name_the_bindings_of_the_commands) {
  Fixture f(true, 200);  // wide: the info line shares its row with the link text, and the help must fit in its share
  CHECK(f.text().find("M-x: commands") != std::string::npos && f.text().find("q: quit") != std::string::npos);
  f.press('x', kModAlt);
  std::string text = f.text();
  // The shortest binding of each: M-x opens the palette, q quits.
  auto row = [&](const std::string & name) {
    size_t at = text.find("\n " + name);
    CHECK(at != std::string::npos);
    size_t end = text.find('\n', at + 1);
    return text.substr(at + 1, end - at - 1);
  };
  CHECK(row("command-palette").find("M-x") != std::string::npos);
  f.type("quit");  // the list is longer than the screen: look for it
  text = f.text();
  std::string quit = row("quit");
  CHECK(quit.back() == 'q');
}

TEST(preset_commands_choose_the_tag_and_fill_the_table) {
  Fixture f;
  CHECK(f.browser->tags().selected() == 0);
  CHECK(f.app->execute("show-currencies"));
  CHECK(f.browser->tags().selected() == 2);
  bool euro = false;  // the rows come in the store's order, not a hand-written one
  for (int r = 0; r < f.browser->table().model()->rowCount(); r++)
    euro = euro || f.browser->table().model()->text(f.browser->table().rowAt(r), Quotes::NAME) == "Euro";
  CHECK(euro);
  CHECK(f.app->execute("previous-preset"));
  CHECK(f.browser->tags().selected() == 1);
  CHECK(!f.app->execute("no-such-command"));
}

TEST(a_click_on_a_heading_sorts_and_prices_sort_by_value) {
  Fixture f;
  // The Price heading, on the table's first row (below the presets, which take as many lines as they need).
  const auto [heading_row, col] = cellOf(f.text(), "Price");
  CHECK(heading_row >= 2 && col > 0);
  f.click(heading_row, col);
  const Table & table = f.browser->table();
  CHECK(table.sortColumn() == Quotes::PRICE && table.sortOrder() == SortOrder::ASCENDING);
  auto & quotes = static_cast<const Quotes &>(*table.model());
  for (int p = 1; p < quotes.rowCount(); p++) CHECK(*quotes.number(table.rowAt(p - 1), Quotes::PRICE) <= *quotes.number(table.rowAt(p), Quotes::PRICE));
}

TEST(numbers_are_written_with_a_point_and_rounded) {
  CHECK(fixed(1.005, 2) == "1.00" || fixed(1.005, 2) == "1.01");
  CHECK(fixed(1234.567, 2) == "1234.57");
  CHECK(fixed(0.05, 2) == "0.05");
  CHECK(fixed(-2.5, 1) == "-2.5");
  CHECK(fixed(-0.001, 2) == "0.00");
  CHECK(fixed(0.5, 2, true) == "+0.50");
  CHECK(fixed(-0.5, 2, true) == "-0.50");
  CHECK(fixed(7, 0) == "7");
  CHECK(fixed(-0.004, 2) == "0.00");  // no "-0.00"
  CHECK(fixed(-0.004, 2, true) != "-0.00");
}

TEST(a_value_with_no_number_or_too_large_a_one_is_written_without_breaking) {
  const double inf = std::numeric_limits<double>::infinity();
  CHECK(fixed(std::nan(""), 2) == "" && fixed(inf, 2) == "" && fixed(-inf, 2, true) == "");
  CHECK(fixed(1e20, 2) == "100000000000000000000.00");
  CHECK(fixed(-1e20, 0) == "-100000000000000000000");
  CHECK(fixed(1e300, 2).size() == 301 + 3 && fixed(1e300, 2).substr(0, 4) == "1000");
  CHECK(fixed(std::numeric_limits<double>::max(), 2).size() == 309 + 3);
  // Where only some of the decimals fit the integer, the rest are zeros.
  CHECK(fixed(1e17, 2) == "100000000000000000.00");
  CHECK(fixed(123456789012345.5, 4) == "123456789012345.5000");
  CHECK(fixed(9007199254740993.0, 1) == "9007199254740992.0");
}

TEST(q_and_ctrl_x_ctrl_c_quit) {
  {
    Fixture f;
    f.backend->pushKey('q');
    CHECK(f.app->run() == 0);  // ends because the key quit; a hang here is the failure
  }
  {
    Fixture f;
    f.backend->pushKey('x', kModCtrl);
    f.backend->pushKey('c', kModCtrl);
    CHECK(f.app->run() == 0);
  }
}

TEST(a_prefix_says_it_waits_and_an_unbound_sequence_says_so) {
  Fixture f;
  f.press('x', kModCtrl);
  CHECK(f.text().find("C-x-") != std::string::npos);
  f.press('z', kModCtrl);
  CHECK(f.text().find("C-x C-z is undefined") != std::string::npos);
}

TEST(the_status_line_says_what_a_preset_shows) {
  Fixture f;
  f.app->execute("show-nasdaq-helsinki");
  CHECK(f.text().find("Showing Nasdaq Helsinki") != std::string::npos);
}

TEST(the_keyboard_moves_between_the_table_and_the_tags_by_command_and_tab_is_the_contexts) {
  Fixture f;
  CHECK(f.app->input().focused() == &f.browser->table());
  f.app->execute("focus-tags");
  CHECK(f.app->input().focused() == &f.browser->tags());
  f.press(Key::Right);
  CHECK(f.browser->tags().selected() == 1);
  f.app->execute("focus-table");
  CHECK(f.app->input().focused() == &f.browser->table());
  f.press(Key::Tab);  // not the focus: the next context
  CHECK(f.app->input().focused() == &f.browser->table() && f.browser->context() == Context::Portfolio);
}

TEST(a_preset_shows_one_row_per_edge_with_its_dataset) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = f.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId currencies = f.store->addMarket("XXXX", "Currencies");
  DatasetId a = f.store->addDataset("providera"), b = f.store->addDataset("providerb");
  f.store->appendCandles({eur, usd, currencies, a}, Resolution::Day, {{900, 1, 1, 1, 1.00, 1}, {1000, 1, 1, 1, 1.10, 1}});
  f.store->appendCandles({eur, usd, currencies, b}, Resolution::Day, {{900, 1, 1, 1, 2.00, 1}, {1000, 1, 1, 1, 1.00, 1}});
  f.app->execute("show-currencies");
  f.app->execute("show-period-day");  // the bars are a hundred days apart: the day is the change since the previous bar
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() == 2);  // the same pair on the same market from two datasets: two rows
  t.sortBy(Quotes::DATASET, SortOrder::ASCENDING);
  CHECK(t.model()->text(t.rowAt(0), Quotes::DATASET) == "PROVIDERA" && t.model()->text(t.rowAt(1), Quotes::DATASET) == "PROVIDERB");
  CHECK(t.model()->text(t.rowAt(0), Quotes::BASE) == "EUR" && t.model()->text(t.rowAt(0), Quotes::QUOTE) == "USD");
  CHECK(t.model()->text(t.rowAt(0), Quotes::PRICE) == "1.10" && t.model()->text(t.rowAt(0), Quotes::CHANGE) == "+10.00%");
  CHECK(t.model()->text(t.rowAt(1), Quotes::PRICE) == "1.00" && t.model()->text(t.rowAt(1), Quotes::CHANGE) == "-50.00%");
  CHECK(t.model()->trace(t.rowAt(1), Quotes::HISTORY).empty());  // no intraday bars: the day has no trace
}

TEST(a_preset_whose_market_has_no_data_shows_no_rows_and_a_book_alone_is_not_a_row) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = f.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId currencies = f.store->addMarket("XXXX", "Currencies");
  DatasetId a = f.store->addDataset("providera");
  CHECK(f.browser->table().model()->rowCount() == 0);  // Commodities: no such market
  f.store->appendBooks({eur, usd, currencies, a}, {{0, {{1, 1}}, {{2, 1}}}});
  f.app->execute("show-currencies");
  CHECK(f.browser->table().model()->rowCount() == 0);
}

TEST(the_made_up_data_fills_every_preset_with_what_the_lists_hold) {
  Fixture f;
  // The commodities come from discovery, not the lists. Currencies and indices
  // are on no market (XXXX), told apart by their kind.
  const std::pair<const char *, size_t> expected[] = {
      {"show-nasdaq-helsinki", countOf("XHEL")},
      {"show-currencies", countOf("XXXX", InstrumentKind::Currency)},
      {"show-indices", countOf("XXXX", InstrumentKind::Index)}};
  for (const auto &[command, count] : expected) {
    f.app->execute(command);
    CHECK(f.browser->table().model()->rowCount() > 0);
    CHECK(static_cast<size_t>(f.browser->table().model()->rowCount()) == count);
    CHECK(f.browser->table().model()->text(f.browser->table().rowAt(0), Quotes::DATASET) == "Synthetic");
  }
}

TEST(the_funds_preset_shows_the_funds_of_every_market_and_nothing_else_and_follows_the_store) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId alpha = f.store->addInstrument("", "Alpha Fund", InstrumentKind::Fund, "FI0008810569");
  InstrumentId beta = f.store->addInstrument("", "Beta Fund", InstrumentKind::Fund, "FI0008800511");
  InstrumentId nokia = f.store->addInstrument("", "Nokia Oyj", InstrumentKind::Company, "FI0009000681");
  MarketId alphaFunds = f.store->addMarket("FUNA", "Alpha Funds"), helsinki = f.store->addMarket("XHEL", "Nasdaq Helsinki");
  DatasetId a = f.store->addDataset("providera");
  f.store->appendCandles({alpha, eur, alphaFunds, a}, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}, {1000, 1, 1, 1, 1.1, 1}});
  f.store->appendCandles({nokia, eur, helsinki, a}, Resolution::Day, {{0, 1, 1, 1, 4.0, 1}, {1000, 1, 1, 1, 4.1, 1}});
  f.app->execute("show-funds");
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() == 1);  // not the company
  // A fund of another market, on the same dataset or another, comes into the table when the store changes; the company does not.
  f.store->appendCandles({beta, eur, helsinki, a}, Resolution::Day, {{0, 1, 1, 1, 2.0, 1}, {1000, 1, 1, 1, 2.1, 1}});
  f.store->appendCandles({nokia, eur, alphaFunds, a}, Resolution::Day, {{0, 1, 1, 1, 4.0, 1}, {1000, 1, 1, 1, 4.1, 1}});
  f.app->widgets().postToRoot(StoreChanged{});
  CHECK(t.model()->rowCount() == 2);
  f.app->execute("show-all");
  CHECK(t.model()->rowCount() == 4);
}

TEST(the_table_follows_the_store_when_told_it_changed_and_keeps_the_sort_and_the_selection) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = f.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  InstrumentId gbp = f.store->addInstrument("GBP", "British pound", InstrumentKind::Currency);
  MarketId currencies = f.store->addMarket("XXXX", "Currencies");
  DatasetId a = f.store->addDataset("providera");
  EdgeKey eurusd{eur, usd, currencies, a}, gbpusd{gbp, usd, currencies, a};
  f.store->appendCandles(eurusd, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}, {1000, 1, 1, 1, 1.1, 1}});
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() == 1);
  t.sortBy(Quotes::PRICE, SortOrder::ASCENDING);

  f.store->appendCandles(gbpusd, Resolution::Day, {{0, 1, 1, 1, 1.2, 1}, {1000, 1, 1, 1, 1.3, 1}});
  f.app->widgets().postToRoot(StoreChanged{});  // what the source thread's post turns into
  CHECK(t.model()->rowCount() == 2);
  CHECK(t.sortColumn() == Quotes::PRICE);
  CHECK(t.model()->text(t.rowAt(0), Quotes::BASE) == "EUR" && t.model()->text(t.rowAt(1), Quotes::BASE) == "GBP");

  t.select(t.rowAt(1));  // GBP; new data for EUR must not move the selection
  f.store->appendCandles(eurusd, Resolution::Day, {{2000, 1, 1, 1, 1.5, 1}});
  f.app->widgets().postToRoot(StoreChanged{});
  CHECK(t.model()->text(t.selectedRow(), Quotes::BASE) == "GBP");
  CHECK(t.model()->text(t.rowAt(1), Quotes::BASE) == "EUR");  // 1.5 sorts after 1.3

  f.app->widgets().postToRoot(SourceMessage{"yahoo: NOKIA.HE: HTTP 404"});
  CHECK(f.text().find("yahoo: NOKIA.HE: HTTP 404") != std::string::npos);
}

TEST(without_a_sort_the_rows_stay_where_they_are_and_a_new_row_goes_last) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = f.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  InstrumentId aud = f.store->addInstrument("AUD", "Australian dollar", InstrumentKind::Currency);
  MarketId currencies = f.store->addMarket("XXXX", "Currencies");
  DatasetId a = f.store->addDataset("providera");
  EdgeKey eurusd{eur, usd, currencies, a}, audusd{aud, usd, currencies, a};
  f.store->appendCandles(eurusd, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}, {1000, 1, 1, 1, 1.1, 1}});
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.sortColumn() == -1);

  f.store->appendCandles(audusd, Resolution::Day, {{0, 1, 1, 1, 0.6, 1}, {1000, 1, 1, 1, 0.7, 1}});
  f.app->widgets().postToRoot(StoreChanged{});
  CHECK(t.model()->rowCount() == 2);
  CHECK(t.model()->text(t.rowAt(0), Quotes::BASE) == "EUR");  // AUD comes before EUR by symbol, but EUR was there first
  CHECK(t.model()->text(t.rowAt(1), Quotes::BASE) == "AUD");
}

TEST(a_new_row_in_another_market_is_not_shown_and_one_in_the_shown_market_goes_last) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = f.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  InstrumentId aud = f.store->addInstrument("AUD", "Australian dollar", InstrumentKind::Currency);
  InstrumentId gbp = f.store->addInstrument("GBP", "British pound", InstrumentKind::Currency);
  MarketId currencies = f.store->addMarket("XXXX", "Currencies");
  MarketId other = f.store->addMarket("XOFF", "Other");
  DatasetId a = f.store->addDataset("providera");
  EdgeKey eurusd{eur, usd, currencies, a}, audusd{aud, usd, currencies, a}, gbpusd{gbp, usd, other, a};
  const std::vector<Candle> bars = {{0, 1, 1, 1, 1.0, 1}, {1000, 1, 1, 1, 1.1, 1}};
  f.store->appendCandles(eurusd, Resolution::Day, bars);
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() == 1);

  f.store->appendCandles(gbpusd, Resolution::Day, bars);  // another market: nothing to show
  f.store->appendCandles(eurusd, Resolution::Day, {{2000, 1, 1, 1, 1.2, 1}});  // a new bar for a row listed: no new row
  f.app->widgets().postToRoot(StoreChanged{});
  CHECK(t.model()->rowCount() == 1);

  f.store->appendCandles(audusd, Resolution::Day, bars);
  f.app->widgets().postToRoot(StoreChanged{});
  CHECK(t.model()->rowCount() == 2);
  CHECK(t.model()->text(t.rowAt(0), Quotes::BASE) == "EUR" && t.model()->text(t.rowAt(1), Quotes::BASE) == "AUD");
}

TEST(a_row_shows_what_the_dataset_calls_the_listing_so_the_rows_of_one_base_are_told_apart) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = f.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  InstrumentId gbp = f.store->addInstrument("GBP", "British pound", InstrumentKind::Currency);
  MarketId currencies = f.store->addMarket("XXXX", "Currencies");
  DatasetId a = f.store->addDataset("providera");
  EdgeKey eurusd{eur, usd, currencies, a}, eurgbp{eur, gbp, currencies, a};
  for (const EdgeKey & k : {eurusd, eurgbp}) f.store->appendCandles(k, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  f.store->setListing({eurusd, "EURUSD", "", "", "CURRENCY", "EUR/USD"});
  f.store->setListing({eurgbp, "EURGBP", "", "", "CURRENCY", "EUR/GBP"});
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  std::set<std::string> names;
  for (int r = 0; r < t.model()->rowCount(); r++) names.insert(t.model()->text(r, Quotes::NAME));
  CHECK((names == std::set<std::string>{"EUR/USD", "EUR/GBP"}));  // not "Euro" on both

  // An instrument listed on two markets: each market's own name for it; one with no name of its own shows the instrument's.
  InstrumentId fund = f.store->addInstrument("", "Fund (instrument)", InstrumentKind::Etf, "IE00BGV5VN51");
  MarketId hel = f.store->addMarket("XHEL", "Nasdaq Helsinki"),
           sto = f.store->addMarket("XSTO", "Nasdaq Stockholm");
  EdgeKey onHel{fund, eur, hel, a}, onSto{fund, usd, sto, a};
  for (const EdgeKey & k : {onHel, onSto}) f.store->appendCandles(k, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  f.store->setListing({onHel, "XAIX", "", "", "UETF", "Fund (Helsinki)"});
  f.store->setListing({onSto, "XAIX", "", "", "UETF", ""});
  auto snapshot = f.store->snapshot();
  CHECK(makeRows(*snapshot, hel)[0].name == "Fund (Helsinki)");
  CHECK(makeRows(*snapshot, sto)[0].name == "Fund (instrument)");
}

TEST(the_table_keeps_its_scroll_position_when_the_store_changes) {
  Fixture f;  // the made-up data: more currency pairs than rows on the screen
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() > t.visibleRows() * 2);
  // Scrolled with the wheel, which leaves the selection at the top: a refresh that rebuilt the table would put the view
  // back where the selected row is.
  Wheel wheel;
  wheel.dy = -20;
  t.post(wheel);
  f.app->present();
  const int first = t.firstVisiblePosition(), selected = t.selectedRow();
  CHECK(first > 0 && selected >= 0);

  auto snapshot = f.store->snapshot();
  const EdgeKey k = snapshot->edges(*snapshot->findMarket("XXXX"))[0];
  f.store->appendCandles(k, Resolution::Day, {{99999, 1, 1, 1, 2.0, 1}});
  f.app->widgets().postToRoot(StoreChanged{});  // what the source thread's post turns into
  CHECK(t.firstVisiblePosition() == first);      // not back at the first row
  CHECK(t.selectedRow() == selected);
}

#include "FakeHttp.h"
#include "TestIsin.h"

TEST(the_rows_on_screen_have_their_history_loaded_and_the_table_follows) {
  std::shared_ptr<Store> store = makeMemoryStore();
  InstrumentId usd = store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId market = store->addMarket("XHEL", "Nasdaq Helsinki");
  DatasetId nordnet = store->addDataset(kNordnetDataset);
  std::vector<EdgeKey> edges;
  for (int i = 0; i < 40; i++) {  // more rows than the screen holds
    InstrumentId inst = store->addInstrument("", "Stock " + std::to_string(i), InstrumentKind::Company, testIsin(100000 + i));
    EdgeKey k{inst, usd, market, nordnet};
    store->appendCandles(k, Resolution::Day, {{dayNumber(2026, 12, 1), kUndefined, 1, 1, 1, kUndefined}});
    store->setListing({k, std::string(i < 10 ? "S0" : "S") + std::to_string(i), "", "book-" + std::to_string(i), "ESH", "Stock " + std::to_string(i)});  // in the order of the symbols
    edges.push_back(k);
  }
  FakeHttp http;
  http.answer = [](const std::string &) {
    std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/nordnet/history-stock-day.json");
    std::stringstream s;
    s << in.rdbuf();
    return ok(s.str());
  };
  struct Borrowed : Http {
    FakeHttp * inner = nullptr;
    HttpResult get(const std::string & url, const HttpHeaders & h) override { return inner->get(url, h); }
  };
  auto lend = std::make_unique<Borrowed>();
  lend->inner = &http;
  HistoryLoader::Options quick;
  quick.pause = std::chrono::milliseconds(0);
  auto history = std::make_shared<HistoryLoader>(store, std::move(lend), quick, [] {});
  history->addProvider(std::make_shared<NordnetHistory>());

  auto owned = std::make_unique<test::TestBackend>(24, 100);
  auto app = Application::adopt(std::move(owned));
  Browser browser(*app, store, nullptr, history);
  app->execute("show-nasdaq-helsinki");
  app->present();  // a frame: the browser tells the loader what is on screen
  history->waitIdle();
  const size_t requested = http.urls.size();
  CHECK(requested > 0 && requested < 40);  // the screen and a margin, not all of them
  CHECK(http.urls[0].find("identifier/book-0?") != std::string::npos);  // the first row first
  // What arrived is in the store: the history of the first row.
  CHECK(store->snapshot()->candles(edges[0], Resolution::Day).candles.size() == 8);
  CHECK(store->snapshot()->candles(edges[39], Resolution::Day).candles.size() == 1);  // not on screen: not asked for

  // Nothing changed on screen: the loader is not told again, and asks for nothing more.
  app->present();
  history->waitIdle();
  CHECK(http.urls.size() == requested);
}

TEST(a_sort_tells_the_history_loader_the_edges_it_brings_on_screen) {
  std::shared_ptr<Store> store = makeMemoryStore();
  InstrumentId usd = store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId market = store->addMarket("XHEL", "Nasdaq Helsinki");
  DatasetId nordnet = store->addDataset(kNordnetDataset);
  for (int i = 0; i < 100; i++) {  // more than the screens the loader is told of
    // The first row, which is selected, sorts first by name either way, so the sort does not scroll to it.
    const std::string name = i == 0 ? "Stock 999" : "Stock " + std::to_string(100 + i);
    InstrumentId inst = store->addInstrument("", name, InstrumentKind::Company, testIsin(100000 + i));
    EdgeKey k{inst, usd, market, nordnet};
    store->appendCandles(k, Resolution::Day, {{dayNumber(2026, 12, 1), kUndefined, 1, 1, 1, kUndefined}});
    store->setListing({k, "S" + std::to_string(100 + i), "", "book-" + std::to_string(100 + i), "ESH", name});
  }
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(404); };  // no bars: each edge is asked for once, and stays as it was
  struct Borrowed : Http {
    FakeHttp * inner = nullptr;
    HttpResult get(const std::string & url, const HttpHeaders & h) override { return inner->get(url, h); }
  };
  auto lend = std::make_unique<Borrowed>();
  lend->inner = &http;
  HistoryLoader::Options quick;
  quick.pause = std::chrono::milliseconds(0);
  auto history = std::make_shared<HistoryLoader>(store, std::move(lend), quick, [] {});
  history->addProvider(std::make_shared<NordnetHistory>());
  auto app = Application::adopt(std::make_unique<test::TestBackend>(24, 100));
  Browser browser(*app, store, nullptr, history);
  app->execute("show-nasdaq-helsinki");
  app->present();
  history->waitIdle();
  auto asked = [&](const std::string & book) {
    for (const std::string & url : http.urls)
      if (url.find("identifier/" + book + "?") != std::string::npos) return true;
    return false;
  };
  CHECK(asked("book-100") && !asked("book-199"));

  // The last rows come to the top: the positions, the row count and the period are what they were, the edges are not.
  browser.table().sortBy(Quotes::NAME, SortOrder::DESCENDING);
  CHECK(browser.table().firstVisiblePosition() == 0 && browser.table().rowAt(0) == 0);
  app->present();
  history->waitIdle();
  CHECK(asked("book-199") && asked("book-190"));
  const size_t requested = http.urls.size();
  app->present();  // nothing changed since: nothing more
  history->waitIdle();
  CHECK(http.urls.size() == requested);
}

TEST(a_sort_still_tells_the_history_loader_the_edges_where_the_day_is_asked_again_every_minute) {
  std::shared_ptr<Store> store = makeMemoryStore();
  InstrumentId usd = store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId market = store->addMarket("XHEL", "Nasdaq Helsinki");
  DatasetId nordnet = store->addDataset(kNordnetDataset);
  for (int i = 0; i < 100; i++) {  // more than the screens the loader is told of
    // The first row, which is selected, sorts first by name either way, so the sort does not scroll to it.
    const std::string name = i == 0 ? "Stock 999" : "Stock " + std::to_string(100 + i);
    InstrumentId inst = store->addInstrument("", name, InstrumentKind::Company, testIsin(100000 + i));
    EdgeKey k{inst, usd, market, nordnet};
    store->appendCandles(k, Resolution::Day, {{dayNumber(2026, 12, 1), kUndefined, 1, 1, 1, kUndefined}});
    store->setListing({k, "S" + std::to_string(100 + i), "", "book-" + std::to_string(100 + i), "ESH", name});
  }
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(404); };  // no bars: each edge is asked for once, and stays as it was
  struct Borrowed : Http {
    FakeHttp * inner = nullptr;
    HttpResult get(const std::string & url, const HttpHeaders & h) override { return inner->get(url, h); }
  };
  auto lend = std::make_unique<Borrowed>();
  lend->inner = &http;
  HistoryLoader::Options quick;
  quick.pause = std::chrono::milliseconds(0);
  auto history = std::make_shared<HistoryLoader>(store, std::move(lend), quick, [] {});
  NordnetHistory::Options lifetimes;
  lifetimes.day_lifetime = std::chrono::hours(1);  // the loader is told again every minute for its expiry: a sort must still get through
  history->addProvider(std::make_shared<NordnetHistory>(lifetimes));
  auto app = Application::adopt(std::make_unique<test::TestBackend>(24, 100));
  Browser browser(*app, store, nullptr, history);
  app->execute("show-period-day");
  app->execute("show-nasdaq-helsinki");
  app->present();
  history->waitIdle();
  auto asked = [&](const std::string & book) {
    for (const std::string & url : http.urls)
      if (url.find("identifier/" + book + "?") != std::string::npos) return true;
    return false;
  };
  CHECK(asked("book-100") && !asked("book-199"));

  // The last rows come to the top: the positions, the row count and the period are what they were, the edges are not.
  browser.table().sortBy(Quotes::NAME, SortOrder::DESCENDING);
  CHECK(browser.table().firstVisiblePosition() == 0 && browser.table().rowAt(0) == 0);
  app->present();
  history->waitIdle();
  CHECK(asked("book-199") && asked("book-190"));
  const size_t requested = http.urls.size();
  app->present();  // nothing changed since: nothing more
  history->waitIdle();
  CHECK(http.urls.size() == requested);
}

TEST(the_all_preset_shows_every_market_and_the_market_column_tells_the_rows_apart) {
  Fixture f;
  f.app->execute("show-all");
  Table & t = f.browser->table();
  auto snapshot = f.store->snapshot();
  CHECK(static_cast<size_t>(t.model()->rowCount()) == allQuoteEdges(*snapshot).size());
  CHECK(t.model()->rowCount() > 100);
  std::set<std::string> markets;
  for (int r = 0; r < t.model()->rowCount(); r++) markets.insert(t.model()->text(r, Quotes::MARKET));
  CHECK(markets.count("No market") && markets.count("Nasdaq Helsinki") &&
        markets.count("Nasdaq Stockholm")); // the made-up data's markets

  // A market with no name said is told by its code.
  Fixture g(false);
  InstrumentId eur = g.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = g.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId unnamed = g.store->addMarket("XNYS");
  DatasetId a = g.store->addDataset(kNordnetDataset);
  g.store->appendCandles({eur, usd, unnamed, a}, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  g.app->execute("show-all");
  CHECK(g.browser->table().model()->text(0, Quotes::MARKET) == "XNYS");
}

TEST(a_currency_pair_shows_its_first_currency_as_the_base_and_not_the_listing_symbol) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = f.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId currencies = f.store->addMarket("XXXX", "Currencies");
  DatasetId a = f.store->addDataset(kNordnetDataset);
  EdgeKey k{eur, usd, currencies, a};
  f.store->appendCandles(k, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  f.store->setListing({k, "EURUSD", "", "", "CURRENCY", "EUR/USD"});
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.model()->text(0, Quotes::BASE) == "EUR" && t.model()->text(0, Quotes::QUOTE) == "USD");
  CHECK(t.model()->text(0, Quotes::NAME) == "EUR/USD");

  // What a market calls anything else stays the base: its symbol there.
  InstrumentId fund = f.store->addInstrument("", "Fund", InstrumentKind::Etf, "IE00BGV5VN51");
  MarketId hel = f.store->addMarket("XHEL", "Nasdaq Helsinki");
  EdgeKey onHel{fund, eur, hel, a};
  f.store->appendCandles(onHel, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  f.store->setListing({onHel, "XAIX", "", "", "UETF", "Fund"});
  f.app->execute("show-nasdaq-helsinki");
  CHECK(t.model()->text(0, Quotes::BASE) == "XAIX");
}

TEST(a_busy_source_is_shown_at_most_so_often_and_the_last_change_is_shown_when_it_is_time) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = f.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  InstrumentId gbp = f.store->addInstrument("GBP", "British pound", InstrumentKind::Currency);
  MarketId currencies = f.store->addMarket("XXXX", "Currencies");
  DatasetId a = f.store->addDataset("providera");
  f.store->appendCandles({eur, usd, currencies, a}, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() == 1);

  f.browser->setMinRefreshInterval(3600);  // not for an hour
  f.store->appendCandles({eur, gbp, currencies, a}, Resolution::Day, {{0, 1, 1, 1, 0.9, 1}});
  f.app->widgets().postToRoot(StoreChanged{});
  CHECK(t.model()->rowCount() == 1);  // asked for, and not yet shown

  f.browser->setMinRefreshInterval(0);
  f.app->present();  // the next frame shows it
  CHECK(t.model()->rowCount() == 2);
}

TEST(show_info_from_the_keyboard_shows_the_isin_the_symbol_a_number_and_a_date_and_escape_closes_it) {
  Fixture f(false);
  addNokia(*f.store);
  f.app->execute("show-nasdaq-helsinki");
  CHECK(f.browser->table().selectedRow() == 0);
  std::string shown;
  std::string middle;
  std::string end;
  const bool closed_by_escape = f.inModal([&] { f.press('i'); },
                                          {[&] { shown = f.text(); }, [&] { f.press(Key::PageDown); }, [&] { middle = f.text(); }, [&] { f.press(Key::End); }, [&] { end = f.text(); },
                                           [&] { f.press(Key::Escape); }});
  CHECK(shown.find("FI0009000681") != std::string::npos);  // the instrument's ISIN
  CHECK(shown.find("NOKIA") != std::string::npos);         // the listing's symbol
  CHECK(shown.find("12345.675") != std::string::npos);     // a real number, with a point
  CHECK(middle.find("2026-03-05") != std::string::npos || shown.find("2026-03-05") != std::string::npos);  // a day number, as a date (below, where the keys scrolled to)
  CHECK(end.find("2 bars, 2026-01-02 to 2026-01-05") != std::string::npos);
  CHECK(closed_by_escape);
  CHECK(f.text().find("Esc: close") == std::string::npos);
  // The Emacs key for it, from the terminal keymap.
  shown.clear();
  f.inModal([&] { f.press('x', kModCtrl), f.press('='); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(shown.find("FI0009000681") != std::string::npos);
}

TEST(a_click_on_the_menu_button_opens_the_menu_of_its_row_whose_first_entry_is_its_info_and_leaves_the_selection) {
  Fixture f(false);
  addNokia(*f.store);
  f.app->execute("show-nasdaq-helsinki");
  f.browser->table().select(-1);
  auto [row, col] = cellOf(f.text(), kMenuGlyphText);
  CHECK(row >= 0);
  std::string menu, shown;
  const bool closed = f.inModal([&] { f.click(row, col); },
                                {[&] { menu = f.text(); }, [&] { f.press(Key::Enter); }, [&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(menu.find("Show info") != std::string::npos && menu.find("NOKIA") != std::string::npos);  // titled by the row
  CHECK(f.browser->table().selectedRow() == -1);  // the click chose nothing
  CHECK(shown.find("FI0009000681") != std::string::npos && shown.find("NOKIA") != std::string::npos);
  CHECK(closed);
}

TEST(a_click_on_a_trace_cell_opens_its_chart_with_dates_and_a_reading_and_a_click_on_text_does_not) {
  Fixture f;
  f.app->execute("show-nasdaq-helsinki");
  auto [heading_row, heading_col] = cellOf(f.text(), "3 months");  // the trace column's heading
  CHECK(heading_row >= 0);
  Table & table = f.browser->table();
  const int expected = table.rowAt(0);
  const auto & quotes = static_cast<const Quotes &>(*table.model());
  const auto view = periodView(*f.store->snapshot(), quotes.edge(expected), Period::Quarter, true);
  CHECK(view.closes.size() > 2 && view.closes.size() == view.times.size() && view.dated);

  std::string shown;
  bool closed = f.inModal([&] { f.click(heading_row + 1, heading_col + 1); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(closed);
  CHECK(table.selectedRow() == expected);  // the click chose the row too
  CHECK(shown.find("3 months") != std::string::npos);  // the title says which period
  const Date first = dateOfDayNumber(dayNumberOfInstant(view.times.front()));
  char date[16];
  std::snprintf(date, sizeof date, "%04d-%02u-%02u", first.year, first.month, first.day);
  CHECK(shown.find(date) != std::string::npos);  // the axis starts at the first bar's date
  CHECK(shown.find(fixed(view.closes.back(), 2)) != std::string::npos);  // the cursor starts at the latest value

  // A cell of text is no chart.
  auto [name_row, name_col] = cellOf(f.text(), "Name");
  shown.clear();
  f.inModal([&] { f.click(name_row + 2, name_col + 1); }, {[&] { shown = f.text(); }});
  CHECK(shown.empty());  // no modal was opened, so the loop its step was to run in never ran
}

TEST(enter_on_a_trace_cell_and_the_show_chart_command_open_the_chart) {
  Fixture f;
  f.app->execute("show-nasdaq-helsinki");
  Table & table = f.browser->table();
  table.select(0);
  f.app->execute("focus-table");
  table.setCursor(Quotes::HISTORY);
  std::string shown;
  bool closed = f.inModal([&] { f.press(Key::Enter); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(closed && shown.find("3 months") != std::string::npos);
  Fixture g;
  g.app->execute("show-nasdaq-helsinki");
  g.browser->table().select(1);
  shown.clear();
  closed = g.inModal([&] { g.press('c'); }, {[&] { shown = g.text(); }, [&] { g.press(Key::Escape); }});
  CHECK(closed && shown.find("3 months") != std::string::npos);
  Fixture h(false);
  h.app->execute("show-chart");
  CHECK(h.text().find("No row selected") != std::string::npos);
}

TEST(a_periods_trace_has_a_time_for_each_value_in_order_and_none_when_it_is_not_asked_for) {
  Fixture f(false);
  const EdgeKey edge = addNokia(*f.store);
  for (const Period period : {Period::Day, Period::Week, Period::Month, Period::Quarter, Period::Year}) {
    const PeriodView v = periodView(*f.store->snapshot(), edge, period, true);
    CHECK(v.times.size() == v.closes.size());
    for (size_t i = 1; i < v.times.size(); i++) CHECK(v.times[i] >= v.times[i - 1]);
    const PeriodView bare = periodView(*f.store->snapshot(), edge, period, false);
    CHECK(bare.times.empty() && bare.closes.empty());
  }
}

TEST(the_command_without_a_selected_row_says_so) {
  Fixture f(false);
  f.app->execute("show-info");
  CHECK(f.text().find("No row selected") != std::string::npos);
}

TEST(the_menu_buttons_follow_a_sort_and_leave_the_scroll_position_alone) {
  Fixture f;
  f.app->execute("show-nasdaq-helsinki");
  Table & table = f.browser->table();
  const auto & quotes = static_cast<const Quotes &>(*table.model());
  CHECK(table.model()->rowCount() > table.visibleRows());
  f.press(Key::End);  // the last row selected, the view scrolled down to it
  const int first = table.firstVisiblePosition();
  CHECK(first > 0);
  const int selected_before = table.selectedRow();
  auto open_first_visible = [&] {
    auto [row, col] = cellOf(f.text(), kMenuGlyphText);  // the first row in view
    CHECK(row >= 0);
    const int expected = table.rowAt(table.firstVisiblePosition());
    std::string shown;
    f.inModal([&] { f.click(row, col); }, {[&] { f.press(Key::Enter); }, [&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
    CHECK(table.selectedRow() == selected_before);
    auto isin = f.store->snapshot()->isin(quotes.edge(expected).base);
    CHECK(!isin.empty() && shown.find(isin) != std::string::npos);  // the row clicked, not another
  };
  open_first_visible();
  CHECK(table.firstVisiblePosition() == first);  // the click did not move the view
  auto [heading_row, heading_col] = cellOf(f.text(), "Name");
  f.click(heading_row, heading_col);
  CHECK(table.sortColumn() == Quotes::NAME);  // the column of buttons did not stop the sort
  open_first_visible();  // and its buttons are on the rows that are there now
}

TEST(a_node_is_shown_as_the_node_it_names_and_is_a_link) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  DatasetId data = f.store->addDataset(kNordnetDataset, "Nordnet");
  const auto snapshot = f.store->snapshot();
  const ShownValue currency = showValue(*snapshot, "currency", Value(NodeKey{eur.value}));
  CHECK(currency.text == "Euro" && currency.link && *currency.link == NodeRef(eur));
  const ShownValue dataset = showValue(*snapshot, "ledger", Value(NodeKey{data.value}));
  CHECK(dataset.text == "Nordnet" && dataset.link && *dataset.link == NodeRef(data));
  const ShownValue unknown = showValue(*snapshot, "x", Value(*currencyKey("ABC")));
  CHECK(unknown.text == "ABC" && unknown.link);  // a node nothing is said of is shown by its key
}

TEST(values_are_shown_by_what_they_are_without_the_locale) {
  const Snapshot none;
  CHECK(showValue(none, "report-day", int64_t{dayNumber(2026, 3, 5)}).text == "2026-03-05");
  CHECK(showValue(none, "first-trade-day", int64_t{dayNumber(1999, 12, 31)}).text == "1999-12-31");
  CHECK(showValue(none, "owners", int64_t{250000}).text == "250000");  // not a day
  CHECK(showValue(none, "tradable", true).text == "yes");
  CHECK(showValue(none, "name", std::string("Nokia")).text == "Nokia");
  CHECK(showValue(none, "x", std::monostate{}).text.empty());
  const std::string before = std::setlocale(LC_NUMERIC, nullptr);
  for (const char * name : {"de_DE.UTF-8", "fi_FI.UTF-8", "de_DE", "fi_FI"})
    if (std::setlocale(LC_NUMERIC, name)) break;  // a decimal comma, where the system has one
  CHECK(realText(0.35) == "0.35");
  CHECK(realText(1234.5) == "1234.5");
  CHECK(realText(42) == "42");
  CHECK(showValue(none, "dividend-amount", 0.7).text == "0.7");
  std::setlocale(LC_NUMERIC, before.c_str());
}

TEST(the_viewer_is_drawn_by_the_loop_without_another_input) {
  Fixture f(false);
  addNokia(*f.store);
  f.app->execute("show-nasdaq-helsinki");
  f.text();
  f.backend->setFrameHandler([&](int64_t) { f.app->present(); });  // as run() sets it
  size_t before = 0, during = 0;
  f.inModal([&] { before = f.backend->frameCount(), f.press('i'); },
            {[&] { during = f.backend->frameCount(); }, [&] { f.press(Key::Escape); }});
  CHECK(during > before);  // the input that opened it has not returned, so no frame follows it: the dialog asks for one
}

TEST(the_period_command_changes_the_change_column_and_the_trace_and_the_headings_say_which) {
  Fixture f(false, 130);  // wide enough for the seven period tags
  Rows rows = periodWorld(*f.store);
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  auto row = [&](const EdgeKey & k) {
    for (int r = 0; r < t.model()->rowCount(); r++)
      if (static_cast<Quotes &>(*t.model()).edge(r) == k) return r;
    return -1;
  };
  const int r = row(rows.riseFall);
  CHECK(f.browser->period() == Period::Quarter);  // not the day: on a weekend the day has no bars
  CHECK(f.text().find("Change 3m") != std::string::npos && f.text().find("3 months") != std::string::npos);
  CHECK(t.model()->text(r, Quotes::CHANGE) == "+100.00%");  // 10 to 20 over the three months
  CHECK(t.model()->trace(r, Quotes::HISTORY).size() == 71 && t.model()->trace(r, Quotes::HISTORY).back() == 20.0f);

  f.app->execute("show-period-day");
  CHECK(f.browser->period() == Period::Day);
  CHECK(f.text().find("Change 1d") != std::string::npos && f.text().find("1 day") != std::string::npos);
  CHECK(t.model()->text(r, Quotes::CHANGE) == "-4.76%");  // 20 against yesterday's 21
  CHECK(t.model()->trace(r, Quotes::HISTORY).size() == 3 && t.model()->trace(r, Quotes::HISTORY)[0] == 20.5f);  // today's bars

  f.app->execute("next-period");
  CHECK(f.browser->period() == Period::Week && f.text().find("Change 1w") != std::string::npos);
  f.app->execute("previous-period");
  f.app->execute("previous-period");
  CHECK(f.browser->period() == Period::Year10);  // round the end of the list
  f.app->execute("next-period");
  CHECK(f.browser->period() == Period::Day);
  f.app->execute("show-period-month");
  CHECK(f.browser->period() == Period::Month && f.browser->periodTags().selected() == static_cast<int>(Period::Month));
}

TEST(a_key_cycles_the_period_and_a_click_on_a_tag_chooses_it) {
  Fixture f(false, 130);
  periodWorld(*f.store);
  f.app->execute("show-currencies");
  f.press(']');
  CHECK(f.browser->period() == Period::Year);
  f.press('[');
  CHECK(f.browser->period() == Period::Quarter);
  f.press('c', kModCtrl);
  f.press('d');
  CHECK(f.browser->period() == Period::Day);
  // The tag of "1w" is on the top row.
  auto [line, col] = cellOf(f.text(), "1w");
  CHECK(line == 0);
  f.click(line, col);
  CHECK(f.browser->period() == Period::Week);
}

TEST(the_change_and_the_trace_are_one_colour_that_follows_the_sign_of_the_periods_change) {
  Fixture f(false);
  Rows rows = periodWorld(*f.store);
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  auto & quotes = static_cast<Quotes &>(*t.model());
  int r = -1;
  for (int i = 0; i < quotes.rowCount(); i++)
    if (quotes.edge(i) == rows.riseFall) r = i;
  auto up = t.model()->color(r, Quotes::CHANGE);  // three months: up
  CHECK(up && up == t.model()->color(r, Quotes::HISTORY));
  f.app->execute("show-period-day");
  auto down = t.model()->color(r, Quotes::CHANGE);  // today: down
  CHECK(down && down == t.model()->color(r, Quotes::HISTORY));
  CHECK(up != down);
}

TEST(a_period_the_bars_do_not_cover_shows_no_number_and_no_colour_but_the_trace_there_is) {
  Fixture f(false);
  Rows rows = periodWorld(*f.store);
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  auto & quotes = static_cast<Quotes &>(*t.model());
  int r = -1;
  for (int i = 0; i < quotes.rowCount(); i++)
    if (quotes.edge(i) == rows.flat) r = i;
  for (const char * command : {"show-period-day", "show-period-week", "show-period-month", "show-period-quarter", "show-period-year"}) {
    f.app->execute(command);
    CHECK(t.model()->text(r, Quotes::CHANGE).empty());  // not "+0.00%"
    CHECK(!t.model()->color(r, Quotes::CHANGE) && !t.model()->color(r, Quotes::HISTORY));  // the trace may be there (what bars there are), with no colour
    CHECK(t.model()->text(r, Quotes::PRICE) == "5.00");  // the price is there
  }
  // Three months of bars are not a year: the first row has no change for the year either, and its trace is the three months it has.
  f.app->execute("show-period-year");
  int first = -1;
  for (int i = 0; i < quotes.rowCount(); i++)
    if (quotes.edge(i) == rows.riseFall) first = i;
  CHECK(t.model()->text(first, Quotes::CHANGE).empty() && !t.model()->trace(first, Quotes::HISTORY).empty());
}

TEST(sorting_by_change_sorts_by_the_periods_change_and_a_store_change_or_a_scroll_keeps_the_period) {
  Fixture f(false);
  Rows rows = periodWorld(*f.store);
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  auto base = [&](int position) { return t.model()->text(t.rowAt(position), Quotes::BASE); };
  t.sortBy(Quotes::CHANGE, SortOrder::DESCENDING);
  CHECK(base(0) == "EUR");  // +100% over the quarter, and the other row has no number: it sorts last
  f.app->execute("show-period-day");
  // The sort follows the period: EUR has -4.76% today and GBP none.
  CHECK(t.sortColumn() == Quotes::CHANGE);
  CHECK(base(0) == "EUR" && base(1) == "GBP");
  t.sortBy(Quotes::CHANGE, SortOrder::ASCENDING);
  CHECK(base(0) == "GBP" && base(1) == "EUR");  // no number first

  // Today's bars come in: the new number is in the sort.
  f.store->appendCandles(rows.flat, Resolution::Day, {{kToday - 1, 1, 1, 1, 4, 1}});
  f.app->widgets().postToRoot(StoreChanged{});
  CHECK(f.browser->period() == Period::Day);
  CHECK(t.model()->text(t.rowAt(0), Quotes::CHANGE) == "-4.76%" && t.model()->text(t.rowAt(1), Quotes::CHANGE) == "+25.00%");
  // A change of preset keeps it as well, and a scroll.
  f.app->execute("show-all");
  f.app->execute("show-currencies");
  Wheel wheel;
  wheel.dy = -2;
  t.post(wheel);
  f.app->present();
  CHECK(f.browser->period() == Period::Day);
  CHECK(f.text().find("Change 1d") != std::string::npos);
}

TEST(the_info_viewer_lists_the_intraday_series_of_a_row) {
  Fixture f(false);
  Rows rows = periodWorld(*f.store);
  const std::vector<InfoSection> info = buildInfo(*f.store->snapshot(), rows.riseFall);
  bool found = false;
  for (const InfoSection & section : info)
    for (const InfoRow & r : section.rows)
      if (r.predicate == "5m") found = r.value.text.find("3 bars, 2026-12-01 07:00 to 2026-12-01 07:10") != std::string::npos;
  CHECK(found);
}

TEST(a_long_value_wraps_at_whitespace_and_a_long_word_is_broken_between_characters) {
  CHECK(wrapText("", 10) == std::vector<std::string>{""});
  CHECK(wrapText("short", 10) == std::vector<std::string>{"short"});
  CHECK((wrapText("one two three four", 9) == std::vector<std::string>{"one two", "three", "four"}));
  CHECK((wrapText("  spaced \n out  ", 20) == std::vector<std::string>{"spaced out"}));
  const std::vector<std::string> url = wrapText("see https://example.com/a/b", 10);
  CHECK((url == std::vector<std::string>{"see", "https://ex", "ample.com/", "a/b"}));
  CHECK((wrapText("ab\xE6\x97\xA5\xE6\x97\xA5", 3) == std::vector<std::string>{"ab", "\xE6\x97\xA5", "\xE6\x97\xA5"}));  // wide characters are not split
}

TEST(a_value_wraps_by_the_width_the_display_gives_the_text_not_by_its_count_of_cells) {
  // A proportional face: every character half a cell wide, so twice as much fits a line.
  const TextWidth half = [](const std::string & s) { return 0.5 * utf8::displayWidth(s); };
  CHECK((wrapText("aaaa bbbb cccc", 5, half) == std::vector<std::string>{"aaaa bbbb", "cccc"}));
  CHECK((wrapText("aaaa bbbb cccc", 5) == std::vector<std::string>{"aaaa", "bbbb", "cccc"}));
  CHECK((wrapText("abcdefghij", 3, half) == std::vector<std::string>{"abcdef", "ghij"}));
}

TEST(the_attribute_dialog_wraps_a_long_value_under_its_predicate_and_asks_for_the_displays_selection) {
  Fixture f(false);
  Rows rows = periodWorld(*f.store);
  const std::string value = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega alpha beta gamma delta epsilon zeta";
  AttributeModal modal(f.app->widgets(), f.app->root(), f.app->backend().eventLoop(), "t", f.store->snapshot(), {{"S", {{"note", {value, std::nullopt}}}}});
  CHECK(modal.wantsNativeSelection());
  const std::vector<std::string> lines = modal.lines();
  CHECK(lines.size() >= 3);  // the heading and the value over two lines
  CHECK(lines[1].rfind("note  alpha", 0) == 0);
  CHECK(lines[2].rfind("      ", 0) == 0);
  std::string joined;
  for (size_t i = 1; i < lines.size(); i++) joined += (joined.empty() ? "" : " ") + lines[i].substr(lines[i].find_first_not_of(' ') + (i == 1 ? 6 : 0));
  CHECK(joined == value);
}

TEST(the_days_base_is_the_previous_close_of_that_session_never_one_left_from_an_earlier_one) {
  Fixture f(false);
  Rows rows = periodWorld(*f.store);
  auto snapshot = [&] { return f.store->snapshot(); };
  CHECK(periodView(*snapshot(), rows.riseFall, Period::Day, false).base == 21);  // the daily bar before today
  f.store->setPreviousClose(rows.riseFall, kToday, 25);  // the provider's own, for today
  CHECK(periodView(*snapshot(), rows.riseFall, Period::Day, false).base == 25);
  // A newer session's bars: a close said for the one before is not its base.
  f.store->appendCandles(rows.riseFall, Resolution::Day, {{kToday + 1, 1, 1, 1, 22, 1}});
  f.store->appendCandles(rows.riseFall, Resolution::Min5, {{fiveMinuteSlot(kToday + 1, 0), 1, 1, 1, 23, 1}});
  // The quote (22, said at noon) is newer than the session's one five-minute bar (23): it is the last price, and the trace ends with it.
  f.store->setQuote(rows.riseFall, {22, kUndefined, kUndefined, (kToday + 1 - kUnixEpochDayNumber) * 86400 + 12 * 3600});
  PeriodView v = periodView(*snapshot(), rows.riseFall, Period::Day, true);
  CHECK(v.base == 20 && v.last == 22 && v.closes.size() == 2 && v.closes[0] == 23.0f && v.closes[1] == 22.0f);
}

namespace {
// One edge with a period's own series ending in a close of 10 on kToday, and the daily bars `daily` (the newest last).
EdgeKey seriesWorld(Store & store, Period period, std::vector<Candle> daily) {
  InstrumentId usd = store.addInstrument("USD", "US dollar", InstrumentKind::Currency);
  InstrumentId eur = store.addInstrument("EUR", "Euro", InstrumentKind::Currency);
  MarketId market = store.addMarket("XXXX", "Currencies");
  EdgeKey k{eur, usd, market, store.addDataset("providera")};
  if (period == Period::Day) {
    store.appendCandles(k, Resolution::Min5, {{fiveMinuteSlot(kToday, 0), 1, 1, 1, 9, 1}, {fiveMinuteSlot(kToday, 1), 1, 1, 1, 10, 1}});
  } else if (period == Period::Week) {
    std::vector<Candle> hours;
    for (int d = 6; d >= 0; d--)
      hours.push_back({slotOfInstant(Resolution::Hour1, (kToday - d - kUnixEpochDayNumber) * 86400 + 7 * 3600), 1, 1, 1, d == 0 ? 10.0 : 8.0, 1});
    store.appendCandles(k, Resolution::Hour1, hours);
  } else {
    std::vector<Candle> weeks;
    for (int64_t w = weekStart(kToday - 364); w <= weekStart(kToday); w += 7) weeks.push_back({w, 1, 1, 1, w == weekStart(kToday) ? 10.0 : 8.0, 1});
    store.appendCandles(k, Resolution::Week, weeks);
  }
  store.appendCandles(k, Resolution::Day, daily);
  return k;
}
}  // namespace

TEST(a_newer_quote_is_the_last_price_of_the_day_week_and_year_and_ends_their_trace) {
  const auto noon = [](int64_t day) { return (day - kUnixEpochDayNumber) * 86400 + 12 * 3600; };
  for (Period period : {Period::Day, Period::Week, Period::Year}) {
    auto store = makeMemoryStore();
    // The price of today has moved on since the series was loaded (what the list says, with its time: a bar is dated by its day, which is
    // no time of it).
    EdgeKey k = seriesWorld(*store, period, {{kToday - 1, 1, 1, 1, 9, 1}, {kToday, 1, 1, 1, 12, 1}});
    store->setQuote(k, {12, kUndefined, kUndefined, noon(kToday)});
    PeriodView v = periodView(*store->snapshot(), k, period, true);
    CHECK(v.last == 12 && !v.closes.empty() && v.closes.back() == 12.0f);
    CHECK(v.change() > 0);
    // The same close as the series' last bar adds no point.
    PeriodView same = periodView(*store->snapshot(), k, period, false);
    CHECK(same.last == 12 && same.base == v.base);
    store->setQuote(k, {10, kUndefined, kUndefined, noon(kToday) + 60});
    PeriodView still = periodView(*store->snapshot(), k, period, true);
    CHECK(still.last == 10 && still.closes.back() == 10.0f && still.closes.size() == v.closes.size() - 1);
  }
}

TEST(a_daily_bar_older_than_the_series_changes_nothing) {
  for (Period period : {Period::Day, Period::Week, Period::Year}) {
    auto store = makeMemoryStore();
    // The newest daily bar is of the day before the series' last bar (the week's first day for the year's weekly bars).
    const int64_t before = (period == Period::Year ? weekStart(kToday) : kToday) - 1;
    EdgeKey k = seriesWorld(*store, period, {{before - 1, 1, 1, 1, 9, 1}, {before, 1, 1, 1, 12, 1}});
    PeriodView v = periodView(*store->snapshot(), k, period, true);
    CHECK(v.last == 10 && v.closes.back() == 10.0f);
  }
}

TEST(a_narrow_screen_keeps_the_presets_and_the_headings_still_name_the_period) {
  Fixture f(true, 60);
  const std::string text = f.text();
  CHECK(text.find("Commodities") != std::string::npos);  // the presets first
  CHECK(cellOf(text, "1w").first != 0);                   // no room for the period tags beside them
  f.app->execute("show-period-week");
  CHECK(f.browser->period() == Period::Week);
  CHECK(f.text().find("1 week") != std::string::npos || f.text().find("Change 1w") != std::string::npos);
  Fixture wide(true, 120);
  CHECK(cellOf(wide.text(), "1w").first == 0);  // room: the tags are at the right end of the top row
}

TEST(the_presets_can_still_be_chosen_by_a_click_and_by_the_keyboard) {
  Fixture f(true, 100);
  auto [line, col] = cellOf(f.text(), "Currencies");
  f.click(line, col + 2);
  CHECK(f.browser->tags().selected() == 2);
  f.app->execute("focus-tags");
  f.press(Key::Left);
  CHECK(f.browser->tags().selected() == 1);
  f.press(Key::Right);
  CHECK(f.browser->tags().selected() == 2);
}

namespace {
// One edge quoted in `unit` ("pct": a rate, "pts": an index) with daily bars from `first` (90 days ago) to `last` (today).
struct RateWorld {
  std::unique_ptr<Store> store = makeMemoryStore();
  EdgeKey edgeOf(const char * symbol, const char * unit, double first, double last) {
    InstrumentId base = store->addInstrument(symbol, symbol, InstrumentKind::Other);
    InstrumentId quote = store->addInstrument(unit, unit, InstrumentKind::Unit);
    EdgeKey k{base, quote, store->addMarket("XXXX", "Rates"), store->addDataset("providera")};
    std::vector<Candle> days;
    for (int d = 90; d >= 0; d--) days.push_back({kToday - d, 1, 1, 1, d == 0 ? last : first, 1});
    store->appendCandles(k, Resolution::Day, days);
    return k;
  }
};

int rowOf(Quotes & q, const EdgeKey & k) {
  for (int i = 0; i < q.rowCount(); i++)
    if (q.edge(i) == k) return i;
  return -1;
}
}  // namespace

TEST(a_rates_change_is_the_difference_in_basis_points) {
  RateWorld w;
  EdgeKey up = w.edgeOf("A", kPercentUnit, 3.00, 3.10), neg = w.edgeOf("B", kPercentUnit, -0.50, -0.40), zero = w.edgeOf("C", kPercentUnit, 0, 0.25);
  EdgeKey small = w.edgeOf("D", kPercentUnit, 3.000, 3.025), index = w.edgeOf("E", kPointsUnit, 100, 101);
  auto snap = w.store->snapshot();
  CHECK(isRate(*snap, up) && !isRate(*snap, index));
  PeriodView v = periodView(*snap, up, Period::Quarter, false);
  CHECK(v.rate && std::abs(v.change() - 10) < 1e-9 && v.base == 3.00 && v.last == 3.10);
  CHECK(std::abs(periodView(*snap, neg, Period::Quarter, false).change() - 10) < 1e-9);
  PeriodView z = periodView(*snap, zero, Period::Quarter, false);
  CHECK(z.hasChange() && std::abs(z.change() - 25) < 1e-9);
  CHECK(std::abs(periodView(*snap, index, Period::Quarter, false).change() - 1.0) < 1e-9);  // percent, not basis points

  Quotes q(w.store->snapshot(), {up, neg, zero, small, index});  // the quarter, by default
  const int ru = rowOf(q, up), rn = rowOf(q, neg), rz = rowOf(q, zero), rs = rowOf(q, small), ri = rowOf(q, index);
  CHECK(q.text(ru, Quotes::CHANGE) == "+10 bp" && q.text(rn, Quotes::CHANGE) == "+10 bp" && q.text(rz, Quotes::CHANGE) == "+25 bp");
  CHECK(q.text(rs, Quotes::CHANGE) == "+2.5 bp");
  CHECK(q.text(ri, Quotes::CHANGE) == "+1.00%");
  CHECK(q.color(ru, Quotes::CHANGE) && q.color(ru, Quotes::CHANGE) == q.color(rn, Quotes::CHANGE) && q.color(ru, Quotes::CHANGE) == q.color(ru, Quotes::HISTORY));
  CHECK(q.text(ru, Quotes::PRICE) == "3.100%" && q.text(ri, Quotes::PRICE) == "101.00");
  // Sorting by Change orders the rates by basis points.
  CHECK(std::abs(*q.number(rs, Quotes::CHANGE) - 2.5) < 1e-9 && std::abs(*q.number(rz, Quotes::CHANGE) - 25.0) < 1e-9);
  CHECK(q.compare(rs, rz, Quotes::CHANGE) < 0 && q.compare(rz, ru, Quotes::CHANGE) > 0);
}

TEST(the_days_change_of_a_rate_is_the_difference_from_the_previous_close_in_basis_points) {
  RateWorld w;
  EdgeKey k = w.edgeOf("A", kPercentUnit, 3.00, 3.105);
  w.store->appendCandles(k, Resolution::Min5, {{fiveMinuteSlot(kToday, 0), 1, 1, 1, 3.11, 1}, {fiveMinuteSlot(kToday, 1), 1, 1, 1, 3.105, 1}});
  w.store->setPreviousClose(k, kToday, 3.10);
  PeriodView v = periodView(*w.store->snapshot(), k, Period::Day, false);
  CHECK(v.rate && v.base == 3.10 && std::abs(v.change() - 0.5) < 1e-9);
}

// ---- Contexts: Markets and Portfolio

namespace {

constexpr int64_t kNow = dayNumber(2026, 12, 1);

// Two portfolios of a store: "Retirement" holds 10 Nokia (bought at 4) and 2 Apple (bought at 150 dollars), "Trading" 3 Nokia (at 6).
// The euro costs 1.25 dollars, Nokia 5 euros and Apple 200 dollars.
struct Holdings {
  PortfolioId retirement, trading;
  EdgeKey nokia, apple;
  InstrumentId eur, usd;
  MarketId hel, nasdaq;
  DatasetId user;
};
Holdings addHoldings(Store & store) {
  Holdings h;
  h.eur = store.addInstrument("EUR", "Euro", InstrumentKind::Currency);
  h.usd = store.addInstrument("USD", "US dollar", InstrumentKind::Currency);
  InstrumentId nokia = store.addInstrument("", "Nokia Oyj", InstrumentKind::Company, "FI0009000681");
  InstrumentId apple = store.addInstrument("", "Apple Inc", InstrumentKind::Company, "US0378331005");
  h.hel = store.addMarket("XHEL", "Nasdaq Helsinki");
  h.nasdaq = store.addMarket("XNAS", "Nasdaq");
  MarketId none = store.addMarket(kNoMarket, "No market");
  DatasetId data = store.addDataset(kNordnetDataset);
  h.user = store.addDataset(kUserDataset);
  h.nokia = {nokia, h.eur, h.hel, data};
  h.apple = {apple, h.usd, h.nasdaq, data};
  auto bars = [&](const EdgeKey & key, double from, double to) {
    std::vector<Candle> days;
    for (int d = 100; d >= 0; d--) days.push_back({kNow - d, 1, 1, 1, from + (to - from) * (100 - d) / 100.0, 1});
    store.appendCandles(key, Resolution::Day, days);
  };
  bars(h.nokia, 4, 5);
  bars(h.apple, 150, 200);
  bars({h.eur, h.usd, none, data}, 1.25, 1.25);
  store.setListing({h.nokia, "NOKIA", "", "", "STOCK", "Nokia"});
  store.setListing({h.apple, "AAPL", "", "", "STOCK", "Apple"});
  h.retirement = PortfolioId{portfolioKey(1)->value};
  h.trading = PortfolioId{portfolioKey(2)->value};
  store.setAttributes(NodeRef(h.retirement), {{predicate::name, "Retirement"}, {predicate::currency, NodeKey{h.eur.value}}});
  store.setAttributes(NodeRef(h.trading), {{predicate::name, "Trading"}, {predicate::currency, NodeKey{h.eur.value}}});
  auto trade = [&](uint64_t id, const EdgeKey & e, double quantity, double price) {
    portfolio::Transaction t;
    t.id = id;
    t.day = kNow - 50;
    t.instrument = e.base;
    t.market = e.market;
    t.quote = e.quote;
    t.quantity = quantity;
    t.price = price;
    return t;
  };
  store.appendTransactions({h.retirement, h.user}, {trade(1, h.nokia, 10, 4), trade(2, h.apple, 2, 150)});
  store.appendTransactions({h.trading, h.user}, {trade(3, h.nokia, 3, 6)});
  return h;
}

}  // namespace

TEST(tab_switches_the_context_and_shift_tab_goes_back_and_both_are_commands) {
  Fixture f(false, 160);  // wide enough for the whole help line
  addHoldings(*f.store);
  CHECK(f.browser->context() == Context::Markets);
  f.press(Key::Tab);
  CHECK(f.browser->context() == Context::Portfolio);
  f.press(Key::Tab);
  CHECK(f.browser->context() == Context::News);
  f.press(Key::Tab);  // three contexts: round to the first
  CHECK(f.browser->context() == Context::Markets);
  f.press(Key::Tab, kModShift);
  CHECK(f.browser->context() == Context::News);
  f.press(Key::Tab, kModShift);
  CHECK(f.browser->context() == Context::Portfolio);
  f.press(Key::Tab, kModShift);
  CHECK(f.browser->context() == Context::Markets);
  f.app->execute("next-context");
  CHECK(f.browser->context() == Context::Portfolio);
  f.app->execute("previous-context");
  CHECK(f.browser->context() == Context::Markets);
  for (const char * name : {"next-context", "previous-context"}) CHECK(!f.app->commands().description(name).empty());
  CHECK(f.text().find("Tab: switch context") != std::string::npos && f.text().find("switch focus") == std::string::npos);
  // Both keymap profiles bind them: the key that a profile reads is the same.
  CHECK(f.app->bindingFor("next-context") == "TAB" || f.app->bindingFor("next-context") == "Tab");
}

TEST(the_portfolio_context_shows_the_portfolios_as_tags_and_what_the_chosen_one_holds) {
  Fixture f(false);
  addHoldings(*f.store);
  f.press(Key::Tab);
  const std::string screen = f.text();
  CHECK(screen.find("Retirement") != std::string::npos && screen.find("Trading") != std::string::npos);
  for (const char * heading : {"Symbol", "Name", "Quantity", "Avg cost", "Price", "Value EUR", "Weight", "P&L", "P&L %", "Change 3m"})
    CHECK(screen.find(heading) != std::string::npos);
  CHECK(screen.find("Base") == std::string::npos && screen.find("Dataset") == std::string::npos);  // not the Markets columns
  CHECK(f.browser->tags().selected() == 0 && f.browser->shownPortfolio() == PortfolioId{portfolioKey(1)->value});  // by name: Retirement first
  auto * positions = dynamic_cast<Positions *>(f.browser->table().model().get());
  CHECK(positions != nullptr && positions->rowCount() == 2);
  if (!positions) return;
  const auto text = [&](const std::string & symbol, int column) {
    for (int r = 0; r < positions->rowCount(); r++)
      if (positions->text(r, Positions::SYMBOL) == symbol) return positions->text(r, column);
    return std::string("-");
  };
  CHECK(text("NOKIA", Positions::QUANTITY) == "10" && text("NOKIA", Positions::COST) == "4.00" && text("NOKIA", Positions::PRICE) == "5.00");
  CHECK(text("NOKIA", Positions::VALUE) == "50.00" && text("NOKIA", Positions::PROFIT) == "+10.00" && text("NOKIA", Positions::PROFIT_PERCENT) == "+25.00%");
  CHECK(text("AAPL", Positions::QUANTITY) == "2" && text("AAPL", Positions::PRICE) == "200.00");
  CHECK(text("AAPL", Positions::VALUE) == "320.00");  // 400 dollars at 1.25 dollars to the euro
  CHECK(text("AAPL", Positions::PROFIT) == "+100.00");  // in its own currency
  CHECK(text("NOKIA", Positions::WEIGHT) == "13.5%" && text("AAPL", Positions::WEIGHT) == "86.5%");
  CHECK(text("NOKIA", Positions::NAME) == "Nokia" && !positions->valuation().incomplete);
  CHECK_NEAR(positions->valuation().total, 370, 1e-9);
  CHECK(screen.find("Retirement: 370.00 EUR in 2 positions") != std::string::npos);  // the status line
  // The other portfolio, chosen by its tag.
  f.browser->tags().select(1);
  positions = dynamic_cast<Positions *>(f.browser->table().model().get());
  CHECK(positions && positions->rowCount() == 1 && positions->text(0, Positions::QUANTITY) == "3" && positions->text(0, Positions::COST) == "6.00");
  CHECK(positions && positions->text(0, Positions::PROFIT) == "-3.00");
}

TEST(a_position_with_no_rate_shows_its_value_in_its_own_currency_and_the_heading_says_it_is_incomplete) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  // A portfolio in a currency nothing converts to.
  InstrumentId sek = f.store->addInstrument("SEK", "Swedish krona", InstrumentKind::Currency);
  const PortfolioId swedish{portfolioKey(3)->value};
  f.store->setAttributes(NodeRef(swedish), {{predicate::name, "Swedish"}, {predicate::currency, NodeKey{sek.value}}});
  portfolio::Transaction t;
  t.id = 9;
  t.day = kNow - 10;
  t.instrument = h.nokia.base;
  t.market = h.hel;
  t.quote = h.eur;
  t.quantity = 1;
  t.price = 4;
  f.store->appendTransactions({swedish, h.user}, {t});
  f.press(Key::Tab);
  f.browser->tags().select(1);  // by name: Retirement, Swedish, Trading
  auto * positions = dynamic_cast<Positions *>(f.browser->table().model().get());
  CHECK(positions != nullptr);
  if (!positions) return;
  CHECK(f.browser->shownPortfolio() == swedish && positions->valuation().incomplete);
  CHECK(positions->text(0, Positions::VALUE) == "5.00 EUR");  // its own, with the code
  CHECK(!positions->number(0, Positions::WEIGHT) && positions->valuation().total == 0);
  CHECK(f.text().find("Value SEK*") != std::string::npos);
}

TEST(each_context_keeps_its_own_tag_sort_and_scroll_position_for_the_session) {
  Fixture f(true);  // the made-up data fills Markets with many rows
  addHoldings(*f.store);
  Table & table = f.browser->table();
  f.app->execute("show-nasdaq-helsinki");
  table.sortBy(Quotes::PRICE, SortOrder::DESCENDING);
  table.select(40);
  const int first = table.firstVisiblePosition(), selected = table.selectedRow();
  CHECK(first > 0 && selected >= 0);
  const int markets_tag = f.browser->tags().selected();
  f.press(Key::Tab);
  CHECK(table.sortColumn() == -1 && f.browser->tags().selected() == 0);  // a context of its own starts clean
  f.browser->tags().select(1);
  table.sortBy(Positions::SYMBOL, SortOrder::ASCENDING);
  f.press(Key::Tab, kModShift);
  CHECK(f.browser->context() == Context::Markets);
  CHECK(f.browser->tags().selected() == markets_tag && table.sortColumn() == Quotes::PRICE && table.sortOrder() == SortOrder::DESCENDING);
  CHECK(table.firstVisiblePosition() == first && table.selectedRow() == selected);
  f.press(Key::Tab);
  CHECK(f.browser->tags().selected() == 1 && f.browser->shownPortfolio() == PortfolioId{portfolioKey(2)->value});
  CHECK(table.sortColumn() == Positions::SYMBOL && table.sortOrder() == SortOrder::ASCENDING);
  // A preset command from the Portfolio context is of Markets: it goes there.
  f.app->execute("show-indices");
  CHECK(f.browser->context() == Context::Markets && f.browser->tags().selected() == 3);
}

TEST(the_period_tags_stay_and_apply_to_both_contexts) {
  Fixture f(false);
  addHoldings(*f.store);
  f.app->execute("show-period-week");
  CHECK(f.text().find("Change 1w") != std::string::npos);
  f.press(Key::Tab);
  CHECK(f.browser->period() == Period::Week && f.text().find("Change 1w") != std::string::npos);
  f.app->execute("show-period-year");
  CHECK(f.text().find("Change 1y") != std::string::npos);
  f.press(Key::Tab);  // News, which has no change, and then round to Markets
  f.press(Key::Tab);
  CHECK(f.browser->period() == Period::Year && f.text().find("Change 1y") != std::string::npos);
}

TEST(with_no_portfolio_the_portfolio_context_says_so_and_offers_new_portfolio) {
  Fixture f(false);
  f.press(Key::Tab);
  const std::string screen = f.text();
  CHECK(screen.find("No portfolio yet") != std::string::npos && screen.find("new-portfolio") != std::string::npos);
  CHECK(f.browser->table().model()->rowCount() == 0 && f.browser->tags().tags().empty());
  // A portfolio appears while it is shown: the hint goes and its tag comes.
  addHoldings(*f.store);
  f.app->widgets().postToRoot(StoreChanged{});
  const std::string later = f.text();
  CHECK(later.find("No portfolio yet") == std::string::npos && later.find("Retirement") != std::string::npos);
  CHECK(f.browser->table().model()->rowCount() == 2);
  // A portfolio with nothing held says that.
  const PortfolioId empty{portfolioKey(7)->value};
  f.store->setAttribute(NodeRef(empty), {predicate::name, "Aaa empty"});
  f.app->widgets().postToRoot(StoreChanged{});
  f.browser->tags().select(0);
  CHECK(f.text().find("Nothing held in this portfolio yet") != std::string::npos);
}

TEST(a_change_to_a_portfolio_while_it_is_shown_keeps_the_selected_row_and_a_deleted_one_goes) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  f.press(Key::Tab);
  f.browser->tags().select(1);  // Trading
  const PortfolioId trading = f.browser->shownPortfolio();
  f.store->setAttribute(NodeRef(trading), {predicate::name, "Active"});  // renamed: it sorts first now
  f.app->widgets().postToRoot(StoreChanged{});
  f.app->present();
  CHECK(f.browser->tags().tags().size() == 2 && f.browser->tags().tags()[0] == "Active" && f.browser->shownPortfolio() == trading && f.browser->tags().selected() == 0);
  f.store->removePortfolio(trading);
  f.app->widgets().postToRoot(StoreChanged{});
  f.app->present();
  CHECK(f.browser->tags().tags().size() == 1 && f.browser->shownPortfolio() == h.retirement);
}

TEST(the_history_loader_is_told_the_edges_of_the_positions_on_screen) {
  std::shared_ptr<Store> store = makeMemoryStore();
  InstrumentId usd = store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId market = store->addMarket("XHEL", "Nasdaq Helsinki");
  DatasetId nordnet = store->addDataset(kNordnetDataset), user = store->addDataset(kUserDataset);
  InstrumentId inst = store->addInstrument("", "Held", InstrumentKind::Company, testIsin(100001));
  InstrumentId other = store->addInstrument("", "Not held", InstrumentKind::Company, testIsin(100002));
  for (auto [i, book] : {std::pair{inst, "book-held"}, std::pair{other, "book-other"}}) {
    EdgeKey k{i, usd, market, nordnet};
    store->appendCandles(k, Resolution::Day, {{dayNumber(2026, 12, 1), kUndefined, 1, 1, 1, kUndefined}});
    store->setListing({k, "SYM", "", book, "ESH", "Instrument"});
  }
  const PortfolioId mine{portfolioKey(1)->value};
  portfolio::Transaction t;
  t.id = 1;
  t.day = dayNumber(2026, 11, 1);
  t.instrument = inst;
  t.market = market;
  t.quote = usd;
  t.quantity = 1;
  t.price = 1;
  store->setAttributes(NodeRef(mine), {{predicate::name, "Mine"}, {predicate::currency, NodeKey{usd.value}}});
  store->appendTransactions({mine, user}, {t});
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(404); };
  struct Borrowed : Http {
    FakeHttp * inner = nullptr;
    HttpResult get(const std::string & url, const HttpHeaders & h) override { return inner->get(url, h); }
  };
  auto lend = std::make_unique<Borrowed>();
  lend->inner = &http;
  HistoryLoader::Options quick;
  quick.pause = std::chrono::milliseconds(0);
  auto history = std::make_shared<HistoryLoader>(store, std::move(lend), quick, [] {});
  history->addProvider(std::make_shared<NordnetHistory>());
  auto app = Application::adopt(std::make_unique<test::TestBackend>(24, 100));
  Browser browser(*app, store, nullptr, history);
  app->present();
  history->waitIdle();
  auto asked = [&](const std::string & book) {
    for (const std::string & url : http.urls)
      if (url.find("identifier/" + book + "?") != std::string::npos) return true;
    return false;
  };
  CHECK(!asked("book-held") && !asked("book-other"));  // the first preset has neither
  app->execute("next-context");
  app->present();
  history->waitIdle();
  CHECK(asked("book-held") && !asked("book-other"));  // the position's edge, and nothing else
}

// ---- Buy and sell, portfolios, undo

namespace {

// The rows that cannot be bought, beside the stocks of addHoldings: an index, a currency pair (addHoldings has the EUR/USD one), a
// rate in per cent, and a fund priced by its company.
struct Untradables {
  EdgeKey index, rate, fund;
};
Untradables addUntradables(Store & store, const Holdings & h) {
  Untradables u;
  InstrumentId omx = store.addInstrument("OMXH25", "OMX Helsinki 25", InstrumentKind::Index);
  InstrumentId euribor = store.addInstrument("EURIBOR12", "Euribor 12m", InstrumentKind::Other);
  InstrumentId fund = store.addInstrument("AFUND", "A fund", InstrumentKind::Fund);
  InstrumentId pts = store.addInstrument("pts", "Points", InstrumentKind::Unit);
  InstrumentId pct = store.addInstrument("pct", "Per cent", InstrumentKind::Unit);
  MarketId none = store.addMarket(kNoMarket, "No market"), off = store.addMarket(kOffExchange, "Off exchange");
  DatasetId data = store.addDataset(kNordnetDataset);
  u.index = {omx, pts, none, data};
  u.rate = {euribor, pct, none, data};
  u.fund = {fund, h.eur, off, data};
  for (const EdgeKey & k : {u.index, u.rate, u.fund}) store.appendCandles(k, Resolution::Day, {{kNow - 1, 1, 1, 1, 10, 1}, {kNow, 1, 1, 1, 11, 1}});
  store.setListing({u.index, "OMXH25", "", "", "INDEX", "OMX Helsinki 25"});
  store.setListing({u.rate, "EURIBOR12", "", "", "RATE", "Euribor 12m"});
  store.setListing({u.fund, "AFUND", "", "", "FUND", "A fund"});
  return u;
}

// The model row of a base symbol in the table now, -1 for none.
int rowOfBase(Fixture & f, const std::string & base) {
  const auto * quotes = dynamic_cast<const Quotes *>(f.browser->table().model().get());
  for (int r = 0; quotes && r < quotes->rowCount(); r++)
    if (quotes->text(r, Quotes::BASE) == base) return r;
  return -1;
}


size_t countOf(const std::string & text, const std::string & needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) n++;
  return n;
}

double heldOf(Fixture & f, PortfolioId p, const EdgeKey & e) {
  const auto held = portfolio::holdings(*f.store->snapshot(), p);
  for (const portfolio::Position & x : held.positions)
    if (x.instrument == e.base && x.market == e.market) return x.quantity;
  return 0;
}

// Fills a form that has the keyboard in its first field (the portfolio): the texts, one for each field after it, left alone where
// empty ("" keeps what the form offered), then Enter.
std::vector<std::function<void()>> fillOrder(Fixture & f, std::vector<std::string> texts, bool accept = true) {
  std::vector<std::function<void()>> steps;
  steps.push_back([&f, texts, accept] {
    for (const std::string & text : texts) {
      f.press(Key::Tab);
      if (!text.empty()) {
        f.press('a', kModCtrl);
        f.press('k', kModCtrl);
        f.type(text);
      }
    }
    if (accept) f.press(Key::Enter);
  });
  return steps;
}

}  // namespace

namespace {
// What the menu of the selected row offers, as the screen shows it, and then closes it.
std::string menuOf(Fixture & f) {
  std::string shown;
  f.inModal([&] { f.app->execute("row-menu"); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  f.press(Key::Down);  // the Escape's key again (the KeyReader waits for it, as Emacs's does)
  return shown;
}
}  // namespace

TEST(the_menu_offers_buy_and_sell_on_the_rows_that_can_be_bought_and_not_on_an_index_a_rate_or_a_pair) {
  Fixture f(false, 120);  // the Portfolio context has more columns, and the button is the last
  Holdings h = addHoldings(*f.store);
  Untradables u = addUntradables(*f.store, h);
  f.app->execute("show-all");
  auto offers = [&](const std::string & base) {
    const int r = rowOfBase(f, base);
    CHECK(r >= 0);
    f.browser->table().select(r);
    const std::string menu = menuOf(f);
    return std::vector<bool>{menu.find("Show info") != std::string::npos, menu.find("Buy") != std::string::npos, menu.find("Sell") != std::string::npos};
  };
  const std::vector<bool> all{true, true, true}, info_only{true, false, false};
  CHECK(offers("NOKIA") == all && offers("AAPL") == all && offers("AFUND") == all);                 // a venue; a fund off exchange
  CHECK(offers("OMXH25") == info_only && offers("EURIBOR12") == info_only && offers("EUR") == info_only);  // an index, a rate, a pair
  // On the screen: one button on each of the six rows, and no more.
  CHECK(countOf(f.text(), kMenuGlyphText) == 6);
  // The Portfolio context always offers them.
  f.press(Key::Tab);
  CHECK(dynamic_cast<const Positions *>(f.browser->table().model().get()) != nullptr);
  f.browser->table().select(0);
  const std::string menu = menuOf(f);
  CHECK(menu.find("Buy") != std::string::npos && menu.find("Sell") != std::string::npos && menu.find("Watchlist") == std::string::npos);  // a position is of no dataset
  CHECK(countOf(f.text(), kMenuGlyphText) == 2);
}

namespace {
// Retirement's NOKIA, sold in part, and a sell of more than is held that the arithmetic leaves out.
void addNokiaSells(Store & store, const Holdings & h) {
  auto sell = [&](uint64_t id, int64_t day, double quantity, double price, double fee, std::string note) {
    portfolio::Transaction t;
    t.id = id;
    t.day = day;
    t.side = portfolio::Side::Sell;
    t.instrument = h.nokia.base;
    t.market = h.nokia.market;
    t.quote = h.nokia.quote;
    t.quantity = quantity;
    t.price = price;
    t.fee = fee;
    t.note = std::move(note);
    return t;
  };
  store.appendTransactions({h.retirement, h.user}, {sell(11, kNow - 5, 100, 5, 0, ""), sell(10, kNow - 10, 4, 5, 1, "half")});
}
}  // namespace

TEST(the_transactions_of_a_position_are_its_own_in_the_order_they_are_applied_with_what_is_held_after_each) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  addNokiaSells(*f.store, h);
  const TransactionTable table = buildTransactionTable(*f.store->snapshot(), h.retirement, h.nokia.base, h.nokia.market, h.nokia.quote);
  CHECK(table.currency == "EUR");
  CHECK(table.rows.size() == 3);  // not Apple's, nor Trading's
  using T = TransactionTable;
  auto cells = [&](size_t r) { return std::vector<std::string>(std::begin(table.rows[r].cells), std::end(table.rows[r].cells)); };
  CHECK(cells(0) == (std::vector<std::string>{dateText(kNow - 50), "Buy", "10", "4", "", "40.00", "10", ""}));
  CHECK(cells(1) == (std::vector<std::string>{dateText(kNow - 10), "Sell", "4", "5", "1", "19.00", "6", "half"}));
  CHECK(cells(2)[T::HELD] == "-" && cells(2)[T::NOTE] == "Left out: it sells more than is held" && table.rows[2].refused == "it sells more than is held");
  CHECK(table.rows[0].refused.empty() && table.rows[1].refused.empty());
  // Nothing of a portfolio that holds none.
  CHECK(buildTransactionTable(*f.store->snapshot(), h.trading, h.apple.base, h.apple.market, h.apple.quote).rows.empty());
}

TEST(the_menu_of_a_position_shows_its_transactions_as_a_table_and_markets_rows_do_not_offer_it) {
  Fixture f(false, 120);
  Holdings h = addHoldings(*f.store);
  addNokiaSells(*f.store, h);
  f.app->execute("show-all");
  f.browser->table().select(rowOfBase(f, "NOKIA"));
  CHECK(menuOf(f).find("Show transactions") == std::string::npos);
  f.app->execute("show-transactions");  // from the palette in Markets: said, and no dialog opens (or this would wait for one)
  CHECK(f.text().find("Only a position has transactions") != std::string::npos);
  f.press(Key::Tab);
  auto positions = std::dynamic_pointer_cast<Positions>(f.browser->table().model());
  CHECK(positions != nullptr && f.browser->shownPortfolio() == h.retirement);
  int nokia = -1;
  for (int r = 0; r < positions->rowCount(); r++)
    if (positions->row(r).position.instrument == h.nokia.base) nokia = r;
  CHECK(nokia >= 0);
  f.browser->table().select(nokia);
  // Show info, then Show transactions: one Down, and Enter.
  std::string shown;
  const bool closed = f.inModal([&] { f.app->execute("row-menu"); },
                                {[&] { f.press(Key::Down); f.press(Key::Enter); }, [&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(closed);
  CHECK(shown.find("Transactions: NOKIA on Nasdaq Helsinki in Retirement") != std::string::npos);
  for (const char * text : {"Date", "Side", "Quantity", "Held", "Note", "half", "19.00", "Left out: it sells more than is held", "3 transactions, prices in EUR. Held 6"})
    CHECK(shown.find(text) != std::string::npos);
  CHECK(shown.find(dateText(kNow - 50)) < shown.find(dateText(kNow - 10)) && shown.find(dateText(kNow - 10)) < shown.find(dateText(kNow - 5)));
  CHECK(shown.find("Held 6 at an average cost of 4.00 EUR") != std::string::npos);
}

TEST(a_click_on_the_menu_button_and_buy_opens_the_form_for_that_row_and_not_the_selected_one) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  addUntradables(*f.store, h);
  f.browser->setToday([] { return kNow; });
  f.app->execute("show-all");
  f.browser->table().sortBy(Quotes::BASE, SortOrder::ASCENDING);
  f.browser->table().select(rowOfBase(f, "AFUND"));
  const std::string screen = f.text();
  // The menu button on the NOKIA row, which is not the selected one.
  int target_row = -1, target_col = -1;
  {
    int row = 0;
    for (size_t from = 0; from <= screen.size(); row++) {
      size_t end = screen.find('\n', from);
      if (end == std::string::npos) end = screen.size();
      const std::string line = screen.substr(from, end - from);
      const size_t first = line.find_first_not_of(' ');  // the icon column comes before the base
      if (first != std::string::npos && line.compare(first, 5, "NOKIA") == 0) {
        const auto at = cellOf(line, kMenuGlyphText);
        target_row = row;
        target_col = at.second;
      }
      from = end + 1;
    }
  }
  CHECK(target_row >= 0);
  std::string shown;
  // Show info, then Buy: one Down, and Enter.
  const bool closed = f.inModal([&] { f.click(target_row, target_col); },
                                {[&] { f.press(Key::Down); f.press(Key::Enter); }, [&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(closed);
  CHECK(shown.find("Buy NOKIA on Nasdaq Helsinki") != std::string::npos);
  CHECK(shown.find("2026-12-01") != std::string::npos && shown.find("Retirement") != std::string::npos);
  CHECK(f.browser->table().selectedRow() == rowOfBase(f, "AFUND"));  // the click did not select its row
  CHECK(f.text().find("Nothing bought") != std::string::npos);
}

TEST(the_keyboard_reaches_the_menu_button_with_the_cursor_and_enter_presses_it_on_every_row) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  Untradables u = addUntradables(*f.store, h);
  f.app->execute("show-all");
  Table & table = f.browser->table();
  for (const char * base : {"NOKIA", "OMXH25"}) {  // a row that can be bought, and one that cannot: the one button is on both
    table.select(rowOfBase(f, base));
    for (int i = 0; i < 14; i++) f.press(Key::Right);
    CHECK(table.cursorColumn() == static_cast<int>(table.columns().size()) - 1 && table.cursorButton() == 0);
    std::string shown;
    f.inModal([&] { f.press(Key::Enter); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
    CHECK(shown.find("Show info") != std::string::npos && shown.find(base) != std::string::npos);
    f.press(Key::Down);  // the Escape's key again
  }
}

TEST(buy_and_sell_refuse_a_row_that_cannot_be_bought_when_run_from_the_palette) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  addUntradables(*f.store, h);
  f.app->execute("show-all");
  for (const char * base : {"OMXH25", "EURIBOR12", "EUR"}) {
    f.browser->table().select(rowOfBase(f, base));
    for (const char * command : {"buy", "sell"}) {
      f.app->execute(command);  // no form opens, or this would wait for one
      const std::string screen = f.text();
      CHECK(screen.find(std::string(base) + " cannot be " + (std::string(command) == "buy" ? "bought" : "sold") + ": it is priced here, not traded") != std::string::npos);
    }
  }
  CHECK(f.browser->undoDepth() == 0 && f.store->snapshot()->transactions({h.retirement, h.user}).size() == 2);
  f.app->execute("buy");  // with a row that can be bought but no form answered: nothing bought
  f.browser->table().select(rowOfBase(f, "NOKIA"));
  CHECK(f.browser->undoDepth() == 0);
}

TEST(buying_writes_a_transaction_the_status_line_says_so_and_undo_and_redo_take_it_back_and_again) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  f.browser->setToday([] { return kNow; });
  f.app->execute("show-nasdaq-helsinki");
  f.browser->table().select(0);
  CHECK(heldOf(f, h.retirement, h.nokia) == 10);
  // Quantity 10, the price offered (the last: 5), a fee of 1.
  std::string shown;
  f.inModal([&] { f.app->execute("buy"); },
            {[&] { shown = f.text(); }, [&] { for (auto & s : fillOrder(f, {"10", "", "", "1"})) s(); }});
  CHECK(shown.find("Total -") != std::string::npos);
  CHECK(heldOf(f, h.retirement, h.nokia) == 20);
  const auto kept = f.store->snapshot()->transactions({h.retirement, h.user});
  CHECK(kept.size() == 3);
  portfolio::Transaction bought;
  for (const auto & t : kept)
    if (t.quantity == 10 && t.price == 5) bought = t;
  CHECK(bought.id != 0 && bought.fee == 1 && bought.day == kNow && bought.side == portfolio::Side::Buy && bought.quote == h.eur && bought.market == h.hel);
  CHECK(f.text().find("Bought 10 NOKIA at 5 EUR in Retirement") != std::string::npos);
  CHECK(f.browser->undoDepth() == 1);
  f.app->execute("undo");
  CHECK(heldOf(f, h.retirement, h.nokia) == 10 && f.browser->undoDepth() == 0 && f.browser->redoDepth() == 1);
  CHECK(f.text().find("Undid: bought 10 NOKIA at 5 EUR in Retirement") != std::string::npos);
  f.app->execute("redo");
  CHECK(heldOf(f, h.retirement, h.nokia) == 20 && f.browser->undoDepth() == 1 && f.browser->redoDepth() == 0);
  CHECK(f.text().find("Redid:") != std::string::npos);
  f.app->execute("redo");
  CHECK(f.text().find("Nothing to redo") != std::string::npos);
  f.app->execute("undo");
  f.app->execute("undo");
  CHECK(f.text().find("Nothing to undo") != std::string::npos && heldOf(f, h.retirement, h.nokia) == 10);
}

TEST(the_form_has_a_live_total_the_last_price_and_checks_its_fields) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  f.browser->setToday([] { return kNow; });
  f.app->execute("show-nasdaq-helsinki");
  f.browser->table().select(0);
  std::string total, bad_portfolio, bad_quantity, bad_date;
  f.inModal([&] { f.app->execute("buy"); },
            {[&] {
               f.press(Key::Tab);
               f.type("4");
               f.press(Key::Tab);
               total = f.text();  // 4 at the last price, 5
               f.press(Key::Tab);
               f.press(Key::Tab);
               f.type("0,5");  // a comma is a decimal point
             },
             [&] { total += f.text(); f.press(Key::Escape); }});
  CHECK(total.find("Total 20.00 EUR") != std::string::npos && total.find("Total 20.00 EUR with the fee") == std::string::npos);
  CHECK(total.find("Total 20.50 EUR with the fee") != std::string::npos);
  // A wrong portfolio, quantity and date are said where they are.
  f.inModal([&] { f.app->execute("buy"); },
            {[&] {
               f.press('a', kModCtrl);
               f.press('k', kModCtrl);
               f.type("Nobody");
               bad_portfolio = f.text();
               f.press(Key::Tab);
               f.type("-2");
               bad_quantity = f.text();
             },
             [&] {
               f.press('a', kModCtrl);
               f.press('k', kModCtrl);
               f.type("1");
               f.press(Key::Tab);
               f.press(Key::Tab);
               f.press('a', kModCtrl);
               f.press('k', kModCtrl);
               f.type("2026-13-45");
               bad_date = f.text();
               f.press(Key::Enter);  // refused: the form stays
             },
             [&] { f.press(Key::Escape); }});
  CHECK(bad_portfolio.find("No portfolio called Nobody") != std::string::npos);
  CHECK(bad_quantity.find("Quantity must be above zero") != std::string::npos);
  CHECK(bad_date.find("Date is not a date") != std::string::npos);
  CHECK(f.store->snapshot()->transactions({h.retirement, h.user}).size() == 2);  // nothing was bought
}

TEST(selling_more_than_is_held_is_said_as_it_is_typed_and_not_accepted) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  f.browser->setToday([] { return kNow; });
  f.app->execute("show-nasdaq-helsinki");
  f.browser->table().select(0);
  std::string too_many;
  f.inModal([&] { f.app->execute("sell"); },
            {[&] {
               f.press(Key::Tab);
               f.type("11");
               too_many = f.text();
               f.press(Key::Enter);  // not accepted
             },
             [&] {
               f.press('a', kModCtrl);
               f.press('k', kModCtrl);
               f.type("4");
               f.press(Key::Enter);
             }});
  CHECK(too_many.find("Sells more than is held: you hold 10 in Retirement") != std::string::npos);
  CHECK(heldOf(f, h.retirement, h.nokia) == 6);  // the second try: 4 sold
  CHECK(f.text().find("Sold 4 NOKIA at 5 EUR in Retirement") != std::string::npos);
  // Selling all of it closes the position.
  f.inModal([&] { f.app->execute("sell"); }, {[&] { f.press(Key::Tab); f.type("6"); f.press(Key::Enter); }});
  CHECK(heldOf(f, h.retirement, h.nokia) == 0);
  // Selling what a portfolio never held.
  std::string none;
  f.inModal([&] { f.app->execute("sell"); }, {[&] { f.press(Key::Tab); f.type("1"); none = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(none.find("Sells more than is held: you hold none in Retirement") != std::string::npos);
}

TEST(a_trade_in_the_portfolio_context_is_of_the_row_and_the_shown_portfolio) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  f.browser->setToday([] { return kNow; });
  f.press(Key::Tab);
  f.browser->tags().select(1);  // Trading holds 3 Nokia
  f.browser->table().select(0);
  std::string shown;
  f.inModal([&] { f.app->execute("sell"); }, {[&] { shown = f.text(); f.press(Key::Tab); f.type("3"); f.press(Key::Enter); }});
  CHECK(shown.find("Sell NOKIA") != std::string::npos && shown.find("Trading") != std::string::npos);  // the shown portfolio is offered
  CHECK(heldOf(f, h.trading, h.nokia) == 0 && heldOf(f, h.retirement, h.nokia) == 10);
  CHECK(f.browser->table().model()->rowCount() == 0);  // the position closed: its row is gone
  CHECK(f.text().find("Nothing held in this portfolio yet") != std::string::npos);
}

TEST(the_first_buy_offers_to_make_a_portfolio) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  f.store->removePortfolio(h.retirement);
  f.store->removePortfolio(h.trading);
  f.browser->setToday([] { return kNow; });
  f.app->execute("show-nasdaq-helsinki");
  f.browser->table().select(0);
  std::string first, second;
  f.inModal([&] { f.app->execute("buy"); },
            {[&] { first = f.text(); f.type("Savings"); f.press(Key::Tab); f.press(Key::Enter); },  // the name; the currency EUR is offered
             [&] { second = f.text(); f.press(Key::Tab); f.type("2"); f.press(Key::Enter); }});
  CHECK(first.find("Make a portfolio first") != std::string::npos && first.find("Currency") != std::string::npos);
  CHECK(second.find("Buy NOKIA") != std::string::npos && second.find("Savings") != std::string::npos);
  const auto ids = f.store->snapshot()->portfolios();
  CHECK(ids.size() == 1 && f.store->snapshot()->name(NodeRef(ids[0])) == "Savings" && heldOf(f, ids[0], h.nokia) == 2);
  CHECK(f.browser->undoDepth() == 2);  // the portfolio, and the buy
  // Cancelling the first form buys nothing.
  Fixture g(false);
  addHoldings(*g.store);
  g.store->removePortfolio(PortfolioId{portfolioKey(1)->value});
  g.store->removePortfolio(PortfolioId{portfolioKey(2)->value});
  g.app->execute("show-nasdaq-helsinki");
  g.browser->table().select(0);
  g.inModal([&] { g.app->execute("buy"); }, {[&] { g.press(Key::Escape); }});
  CHECK(g.store->snapshot()->portfolios().empty() && g.browser->undoDepth() == 0 && g.text().find("No portfolio made") != std::string::npos);
}

TEST(new_portfolio_makes_one_with_a_name_and_a_currency_and_it_is_the_one_shown) {
  Fixture f(false);
  addHoldings(*f.store);
  f.press(Key::Tab);
  std::string shown, taken;
  f.inModal([&] { f.app->execute("new-portfolio"); },
            {[&] {
               shown = f.text();
               f.type("Retirement");  // taken
               taken = f.text();
               f.press('a', kModCtrl);
               f.press('k', kModCtrl);
               f.type("Dividends");
               f.press(Key::Tab);
               f.press('a', kModCtrl);
               f.press('k', kModCtrl);
               f.type("usd");  // any case
               f.press(Key::Enter);
             }});
  CHECK(shown.find("New portfolio") != std::string::npos && shown.find("EUR") != std::string::npos);
  CHECK(taken.find("A portfolio called Retirement exists") != std::string::npos);
  const auto snapshot = f.store->snapshot();
  CHECK(snapshot->portfolios().size() == 3);
  CHECK(f.browser->tags().tags().size() == 3 && f.browser->tags().tags()[f.browser->tags().selected()] == "Dividends");
  const PortfolioId made = f.browser->shownPortfolio();
  CHECK(snapshot->name(NodeRef(made)) == "Dividends" && portfolio::currencyOf(*snapshot, made) == std::optional<InstrumentId>(InstrumentId{currencyKey("USD")->value}));
  CHECK(f.text().find("Made portfolio Dividends in USD") != std::string::npos && f.browser->table().model()->rowCount() == 0);
  f.app->execute("undo");
  CHECK(f.store->snapshot()->portfolios().size() == 2 && f.browser->tags().tags().size() == 2);
  f.app->execute("redo");
  CHECK(f.store->snapshot()->portfolios().size() == 3 && f.store->snapshot()->name(NodeRef(made)) == "Dividends");
}

TEST(new_portfolio_refuses_an_unknown_currency_and_an_empty_name) {
  Fixture f(false);
  addHoldings(*f.store);
  std::string bad_currency, empty;
  f.inModal([&] { f.app->execute("new-portfolio"); },
            {[&] {
               f.type("Abc");
               f.press(Key::Tab);
               f.press('a', kModCtrl);
               f.press('k', kModCtrl);
               f.type("ZZZ");
               bad_currency = f.text();
               f.press(Key::Enter);
             },
             [&] { f.press(Key::Escape); }});
  CHECK(bad_currency.find("No currency ZZZ is known") != std::string::npos);
  CHECK(f.store->snapshot()->portfolios().size() == 2);
  f.inModal([&] { f.app->execute("new-portfolio"); }, {[&] { f.press(Key::Enter); empty = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(empty.find("Name is required") != std::string::npos && f.store->snapshot()->portfolios().size() == 2);
}

TEST(rename_portfolio_says_the_new_name_and_undo_says_the_old_again) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  f.press(Key::Tab);
  f.inModal([&] { f.app->execute("rename-portfolio"); }, {[&] { f.press('a', kModCtrl); f.press('k', kModCtrl); f.type("Pension"); f.press(Key::Enter); }});
  CHECK(f.store->snapshot()->name(NodeRef(h.retirement)) == "Pension" && f.browser->tags().tags()[0] == "Pension");
  CHECK(f.text().find("Renamed Retirement to Pension") != std::string::npos);
  f.app->execute("undo");
  CHECK(f.store->snapshot()->name(NodeRef(h.retirement)) == "Retirement" && f.browser->tags().tags()[0] == "Retirement");
  f.app->execute("redo");
  CHECK(f.store->snapshot()->name(NodeRef(h.retirement)) == "Pension");
  // Another's name is taken; the same name is no change.
  std::string taken;
  f.inModal([&] { f.app->execute("rename-portfolio"); }, {[&] { f.press('a', kModCtrl); f.press('k', kModCtrl); f.type("Trading"); taken = f.text(); f.press(Key::Escape); }});
  CHECK(taken.find("A portfolio called Trading exists") != std::string::npos);
  f.inModal([&] { f.app->execute("rename-portfolio"); }, {[&] { f.press(Key::Enter); }});
  CHECK(f.text().find("The name is the same") != std::string::npos && f.browser->undoDepth() == 1);
}

TEST(delete_portfolio_confirms_removes_everything_said_of_it_and_undo_brings_it_all_back) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  f.press(Key::Tab);
  // Keeping it.
  std::string asked;
  f.inModal([&] { f.app->execute("delete-portfolio"); }, {[&] { asked = f.text(); f.press(Key::Escape); }});
  CHECK(asked.find("Delete portfolio Retirement?") != std::string::npos && asked.find("Delete it and its 2 transactions") != std::string::npos);
  CHECK(f.store->snapshot()->portfolios().size() == 2 && f.text().find("Kept Retirement") != std::string::npos);
  // Deleting it: the first answer.
  f.inModal([&] { f.app->execute("delete-portfolio"); }, {[&] { f.press(Key::Enter); }, [&] { f.press(Key::Up); f.press(Key::Enter); }});
  CHECK(f.store->snapshot()->portfolios().size() == 2);  // Enter on "Keep it", which was the choice to start with
  f.inModal([&] { f.app->execute("delete-portfolio"); }, {[&] { f.press(Key::Up); f.press(Key::Enter); }});
  auto s = f.store->snapshot();
  CHECK(s->portfolios() == std::vector<PortfolioId>{h.trading} && !s->known(NodeRef(h.retirement)) && s->ledgers(h.retirement).empty());
  CHECK(f.browser->tags().tags() == std::vector<std::string>{"Trading"} && f.browser->shownPortfolio() == h.trading);
  CHECK(f.text().find("Deleted portfolio Retirement") != std::string::npos);
  // Undo brings the name, the currency link and every transaction back, under the same key.
  f.app->execute("undo");
  s = f.store->snapshot();
  CHECK(s->portfolios().size() == 2 && s->name(NodeRef(h.retirement)) == "Retirement" && portfolio::currencyOf(*s, h.retirement) == std::optional<InstrumentId>(h.eur));
  CHECK(s->transactions({h.retirement, h.user}).size() == 2 && heldOf(f, h.retirement, h.nokia) == 10 && heldOf(f, h.retirement, h.apple) == 2);
  f.app->execute("redo");
  CHECK(f.store->snapshot()->portfolios().size() == 1);
}

TEST(undo_takes_back_whatever_the_last_change_was_in_order) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  f.browser->setToday([] { return kNow; });
  f.app->execute("show-nasdaq-helsinki");
  f.browser->table().select(0);
  f.inModal([&] { f.app->execute("buy"); }, {[&] { f.press(Key::Tab); f.type("1"); f.press(Key::Enter); }});
  f.press(Key::Tab);
  f.inModal([&] { f.app->execute("rename-portfolio"); }, {[&] { f.press('a', kModCtrl); f.press('k', kModCtrl); f.type("Renamed"); f.press(Key::Enter); }});
  CHECK(f.browser->undoDepth() == 2);
  f.app->execute("undo");
  CHECK(f.store->snapshot()->name(NodeRef(h.retirement)) == "Retirement" && heldOf(f, h.retirement, h.nokia) == 11);
  f.app->execute("undo");
  CHECK(heldOf(f, h.retirement, h.nokia) == 10);
  CHECK(f.browser->undoDepth() == 0 && f.browser->redoDepth() == 2);
  f.app->execute("redo");
  CHECK(f.browser->undoDepth() == 1 && f.browser->redoDepth() == 1 && heldOf(f, h.retirement, h.nokia) == 11);
  // A new change after an undo ends what could be made again.
  f.inModal([&] { f.app->execute("rename-portfolio"); }, {[&] { f.press('a', kModCtrl); f.press('k', kModCtrl); f.type("Other"); f.press(Key::Enter); }});
  CHECK(f.browser->redoDepth() == 0 && f.browser->undoDepth() == 2);
  f.app->execute("redo");
  CHECK(f.text().find("Nothing to redo") != std::string::npos && f.store->snapshot()->name(NodeRef(h.retirement)) == "Other");
}

TEST(every_portfolio_command_has_a_description_and_is_in_the_palette) {
  Fixture f;
  for (const char * name : {"buy", "sell", "new-portfolio", "rename-portfolio", "delete-portfolio", "undo", "redo", "next-context", "previous-context"}) {
    bool listed = false;
    for (const std::string & n : f.app->commands().names()) listed |= n == name;
    CHECK(listed && !f.app->commands().description(name).empty());
  }
  // Without a portfolio or a row, they say so and do nothing.
  Fixture g(false);
  g.app->execute("rename-portfolio");
  CHECK(g.text().find("No portfolio to rename") != std::string::npos);
  g.app->execute("delete-portfolio");
  CHECK(g.text().find("No portfolio to delete") != std::string::npos);
  g.app->execute("buy");
  CHECK(g.text().find("No row selected") != std::string::npos);
}

// ---- Keys, bound for each keymap profile

namespace {

// One change that can be taken back: the first portfolio renamed.
void renameFirst(Fixture & f, const std::string & name) {
  f.inModal([&] { f.app->execute("rename-portfolio"); }, {[&] { f.press('a', kModCtrl); f.press('k', kModCtrl); f.type(name); f.press(Key::Enter); }});
}

}  // namespace

TEST(terminal_keys_undo_is_c_x_u_and_c_slash_and_redo_is_c_m_underscore) {
  Fixture f(false, 100, KeymapProfile::TERMINAL);
  Holdings h = addHoldings(*f.store);
  f.press(Key::Tab);
  auto name = [&] { return f.store->snapshot()->name(NodeRef(h.retirement)); };
  renameFirst(f, "One");
  CHECK(name() == "One");
  f.press('x', kModCtrl);
  f.press('u');
  CHECK(name() == "Retirement");
  f.press('_', kModCtrl | kModAlt);  // C-M-_
  CHECK(name() == "One");
  f.press('/', kModCtrl);  // C-/ where the terminal reports it
  CHECK(name() == "Retirement");
  f.press('_', kModCtrl | kModAlt);
  CHECK(name() == "One");
  f.press('_', kModCtrl);  // and C-_, which is what most terminals send for it
  CHECK(name() == "Retirement");
  CHECK(f.app->bindingFor("redo") == "C-M-_" && !f.app->bindingFor("undo").empty());
  // Not the desktop's.
  f.press('z', kModCtrl);
  f.press('y', kModCtrl);
  CHECK(name() == "Retirement" && f.browser->redoDepth() == 1);
}

TEST(desktop_keys_undo_is_ctrl_z_and_redo_is_ctrl_y) {
  Fixture f(false, 100, KeymapProfile::DESKTOP);
  Holdings h = addHoldings(*f.store);
  f.press(Key::Tab);
  auto name = [&] { return f.store->snapshot()->name(NodeRef(h.retirement)); };
  renameFirst(f, "One");
  f.press('z', kModCtrl);
  CHECK(name() == "Retirement" && f.app->bindingFor("undo") == "Ctrl+Z");
  f.press('y', kModCtrl);
  CHECK(name() == "One" && f.app->bindingFor("redo") == "Ctrl+Y");
  // Not the terminal's.
  f.press('x', kModCtrl);
  f.press('u');
  CHECK(name() == "One");
}

TEST(tab_and_shift_tab_switch_the_context_in_both_profiles_and_buy_and_sell_have_no_key) {
  for (KeymapProfile profile : {KeymapProfile::TERMINAL, KeymapProfile::DESKTOP}) {
    Fixture f(false, 100, profile);
    addHoldings(*f.store);
    f.press(Key::Tab);
    CHECK(f.browser->context() == Context::Portfolio);
    f.press(Key::Tab, kModShift);
    CHECK(f.browser->context() == Context::Markets);
    CHECK(f.app->bindingFor("next-context") == "TAB" || f.app->bindingFor("next-context") == "Tab");
    CHECK(!f.app->bindingFor("previous-context").empty());
    CHECK(f.app->bindingFor("buy").empty() && f.app->bindingFor("sell").empty());
    // They are in the palette all the same.
    f.app->execute("command-palette");
    f.type("buy");
    CHECK(f.app->commandPaletteOpen() && f.text().find("buy") != std::string::npos && f.text().find("sell") == std::string::npos);
  }
}

// ---- News

namespace {

NewsItem newsItem(const char * id, Timestamp time, const char * headline, std::vector<NodeRef> about, const char * kind) {
  NewsItem n;
  n.id = id;
  n.time = time;
  n.headline = headline;
  n.about = std::move(about);
  n.attributes.push_back({predicate::kind, kind});
  return n;
}

}  // namespace

TEST(the_news_context_shows_the_news_newest_first_and_a_preset_is_a_selection_by_kind) {
  Fixture f(false, 160);
  const InstrumentId nokia = f.store->addInstrument("", "Nokia Oyj", InstrumentKind::Company, "FI0009000681", "ESH");
  const DatasetId data = f.store->addDataset(kNordnetDataset, "Nordnet");
  const NodeRef finland = nodeOf(*countryKey("FI"));
  f.store->appendNews(data, {newsItem("1", 1795214400, "Nokia buys back shares", {nokia, finland}, kPressRelease),
                             newsItem("2", 1795218000, "Analyst raises Nokia", {nokia}, kAnalysis),
                             newsItem("3", 1795221600, "Markets close higher", {}, kNewsStory)});
  f.app->execute("show-all-news");
  CHECK(f.browser->context() == Context::News && f.browser->tags().selected() == 0);
  auto & table = f.browser->table();
  const auto * news = dynamic_cast<const NewsTable *>(table.model().get());
  CHECK(news && news->rowCount() == 3);
  CHECK(news->item(0).headline == "Markets close higher" && news->item(2).headline == "Nokia buys back shares");  // newest first
  const std::string screen = f.text();
  CHECK(screen.find("Markets close higher") != std::string::npos && screen.find("Nokia Oyj, FI") != std::string::npos);
  CHECK(news->text(2, NewsTable::TIME) == "2026-11-20 22:40" && news->text(2, NewsTable::KIND) == kPressRelease);
  CHECK(news->text(2, NewsTable::SOURCE) == "Nordnet");
  // The presets select by what the item says under `kind`, and the general one by what it is about.
  f.app->execute("show-analyses");
  CHECK(f.browser->tags().selected() == 2 && table.model()->rowCount() == 1 && table.model()->text(0, NewsTable::HEADLINE) == "Analyst raises Nokia");
  f.app->execute("show-press-releases");
  CHECK(table.model()->rowCount() == 1 && table.model()->text(0, NewsTable::HEADLINE) == "Nokia buys back shares");
  f.app->execute("show-general-news");
  CHECK(table.model()->rowCount() == 1 && table.model()->text(0, NewsTable::HEADLINE) == "Markets close higher");
  CHECK(f.browser->tags().tags().size() == 5);
  // News that arrives while it is shown comes in at once, and the selection stays.
  f.app->execute("show-all-news");
  table.select(1);
  f.store->appendNews(data, {newsItem("4", 1795225200, "A fresh item", {}, kNewsStory)});
  f.app->widgets().postToRoot(StoreChanged{});
  f.app->present();
  CHECK(table.model()->rowCount() == 4 && table.model()->text(0, NewsTable::HEADLINE) == "A fresh item");
  CHECK(table.model()->text(table.selectedRow(), NewsTable::HEADLINE) == "Analyst raises Nokia");
  // Tab goes round Markets, Portfolio and News; every command has its description.
  for (const char * name : {"show-all-news", "show-press-releases", "show-analyses", "show-news-stories", "show-general-news"}) CHECK(!f.app->commands().description(name).empty());
}

TEST(the_info_of_a_news_item_says_whom_it_is_about_and_the_info_of_a_row_lists_its_latest_news) {
  Fixture f(false, 160);
  const InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency, "", "");
  const InstrumentId nokia = f.store->addInstrument("", "Nokia Oyj", InstrumentKind::Company, "FI0009000681", "ESH");
  const MarketId hel = f.store->addMarket("XHEL", "Nasdaq Helsinki");
  const DatasetId data = f.store->addDataset(kNordnetDataset, "Nordnet");
  f.store->appendCandles({nokia, eur, hel, data}, Resolution::Day, {{dayNumber(2026, 1, 2), 1, 1, 1, 4.2, 1}});
  f.store->appendNews(data, {newsItem("1", 1795214400, "Nokia buys back shares", {nokia}, kPressRelease)});
  auto snapshot = f.store->snapshot();
  std::string all;
  for (const InfoSection & section : buildInfo(*snapshot, {nokia, eur, hel, data}))
    for (const InfoRow & row : section.rows) all += section.title + "|" + row.predicate + "|" + row.value.text + "\n";
  CHECK(all.find("Latest news|2026-11-20 22:40|Nokia buys back shares") != std::string::npos);
  all.clear();
  for (const InfoSection & section : buildNewsInfo(*snapshot, snapshot->news()[0]))
    for (const InfoRow & row : section.rows) all += section.title + "|" + row.predicate + "|" + row.value.text + "\n";
  CHECK(all.find("News|headline|Nokia buys back shares") != std::string::npos && all.find("About|instrument|Nokia Oyj") != std::string::npos);
  CHECK(all.find("Attributes|kind|press-release") != std::string::npos);
}

TEST(the_physical_preset_has_what_is_held_in_hand_and_not_the_funds_or_the_spot) {
  Fixture f(false, 120);
  Holdings h = addHoldings(*f.store);
  Untradables u = addUntradables(*f.store, h);  // the fund is on XOFF too, and the index and the rate on XXXX
  InstrumentId gold = f.store->addInstrument("GOLDCOIN", "Gold coin (1 oz fine)", InstrumentKind::Commodity);
  InstrumentId spot = f.store->addInstrument("GOLD", "Gold", InstrumentKind::Commodity, "XC0009655157");
  MarketId off = *f.store->snapshot()->findMarket(kOffExchange), none = *f.store->snapshot()->findMarket(kNoMarket);
  DatasetId data = f.store->addDataset(kPhysicalDataset, "Estimate");
  for (const EdgeKey & k : {EdgeKey{gold, h.usd, off, data}, EdgeKey{spot, h.usd, none, h.user}})
    f.store->appendCandles(k, Resolution::Day, {{kNow, 1, 1, 1, 2900, 1}});
  f.store->setListing({EdgeKey{gold, h.usd, off, data}, "GOLDCOIN", "", "", "PHYSICAL", "Gold coin (1 oz fine)"});
  f.app->execute("show-physical");
  CHECK(f.browser->context() == Context::Markets && f.browser->tags().tags()[static_cast<size_t>(f.browser->tags().selected())] == "Physical");
  const auto * quotes = dynamic_cast<const Quotes *>(f.browser->table().model().get());
  CHECK(quotes != nullptr && quotes->rowCount() == 1 && quotes->text(0, Quotes::BASE) == "GOLDCOIN");  // not the fund, nor the spot gold
  CHECK(f.text().find("Estimate") != std::string::npos && f.text().find("Physical") != std::string::npos);
  // From the Portfolio context the command goes to Markets first, as the other presets do.
  f.press(Key::Tab);
  f.app->execute("show-physical");
  CHECK(f.browser->context() == Context::Markets && quotes != nullptr);
  CHECK(!f.app->commands().description("show-physical").empty());
}

namespace {
// Nokia on the Helsinki market from two datasets: two rows, which are two edges.
std::pair<EdgeKey, EdgeKey> addNokiaTwice(Store & store) {
  const EdgeKey one = addNokia(store);
  const DatasetId other = store.addDataset("yahoo", "Yahoo");
  const EdgeKey two{one.base, one.quote, one.market, other};
  store.appendCandles(two, Resolution::Day, {{dayNumber(2026, 1, 2), 1, 1, 1, 4.1, 1}, {dayNumber(2026, 1, 5), 1, 1, 1, 4.4, 1}});
  store.setListing({two, "NOKIA", "99", "98", "STOCK", "Nokia"});
  return {one, two};
}
}  // namespace

TEST(a_row_is_put_on_the_watchlist_from_its_menu_and_only_that_row_not_the_instrument_on_its_other_dataset) {
  Fixture f(false);
  const auto [one, two] = addNokiaTwice(*f.store);
  f.app->execute("show-nasdaq-helsinki");
  Table & table = f.browser->table();
  CHECK(table.model()->rowCount() == 2);
  auto & quotes = static_cast<Quotes &>(*table.model());
  const int first = quotes.edge(0) == one ? 0 : 1;
  table.select(first);
  std::string menu;
  // Show info, Buy, Sell, then the list: three Downs and Enter.
  f.inModal([&] { f.app->execute("row-menu"); },
            {[&] { menu = f.text(); }, [&] { f.press(Key::Down); f.press(Key::Down); f.press(Key::Down); f.press(Key::Enter); }, [&] { f.press(Key::Escape); }});
  CHECK(menu.find("Put on the Watchlist") != std::string::npos);
  CHECK(f.store->snapshot()->onList("watchlist", one) && !f.store->snapshot()->onList("watchlist", two));
  CHECK(f.text().find("Put NOKIA on the Watchlist") != std::string::npos && f.browser->undoDepth() == 1);
  // The menu now offers the way back, and the preset shows that row alone.
  table.select(first);
  CHECK(menuOf(f).find("Take off the Watchlist") != std::string::npos);
  f.app->execute("show-watchlist");
  CHECK(f.browser->table().model()->rowCount() == 1 && static_cast<Quotes &>(*f.browser->table().model()).edge(0) == one);
  CHECK(f.text().find("Nothing on the Watchlist") == std::string::npos);
  // Taking it off, from the list itself, leaves it empty and says what to do.
  f.browser->table().select(0);
  f.app->execute("remove-from-watchlist");
  CHECK(f.browser->table().model()->rowCount() == 0 && f.text().find("Nothing on the Watchlist yet") != std::string::npos);
  CHECK(!f.store->snapshot()->onList("watchlist", one));
  // Undo puts it back, and undo again takes it off, as for any change of hers.
  f.app->execute("undo");
  CHECK(f.browser->table().model()->rowCount() == 1 && f.store->snapshot()->onList("watchlist", one));
  f.app->execute("undo");
  CHECK(f.browser->table().model()->rowCount() == 0 && f.browser->undoDepth() == 0 && f.browser->redoDepth() == 2);
  f.app->execute("redo");
  CHECK(f.browser->table().model()->rowCount() == 1);
}

TEST(a_row_already_on_the_watchlist_or_not_on_it_is_told_so_and_nothing_is_recorded) {
  Fixture f(false);
  const auto edges = addNokiaTwice(*f.store);
  f.app->execute("show-nasdaq-helsinki");
  f.browser->table().select(0);
  f.app->execute("remove-from-watchlist");
  CHECK(f.text().find("is not on the Watchlist") != std::string::npos && f.browser->undoDepth() == 0);
  f.app->execute("add-to-watchlist");
  f.app->execute("add-to-watchlist");
  CHECK(f.text().find("is on the Watchlist already") != std::string::npos && f.browser->undoDepth() == 1);
}

TEST(a_list_is_of_edges_so_a_position_cannot_be_put_on_one) {
  Fixture f(false);
  Holdings h = addHoldings(*f.store);
  f.press(Key::Tab);
  f.browser->table().select(0);
  f.app->execute("add-to-watchlist");
  CHECK(f.text().find("Only a row of Markets") != std::string::npos && f.browser->undoDepth() == 0);
}

TEST(the_presets_wrap_onto_a_second_line_on_a_narrow_screen_and_every_one_can_still_be_clicked) {
  Fixture f(true, 60);
  const std::string text = f.text();
  const auto [watch_row, watch_col] = cellOf(text, "Watchlist");
  const auto [commodities_row, commodities_col] = cellOf(text, "Commodities");
  CHECK(commodities_row == 0 && watch_row > 0);  // the last one is on a line of its own below the first
  const auto [heading_row, heading_col] = cellOf(text, "Base");
  CHECK(heading_row > watch_row);  // and the table starts below them
  f.click(watch_row, watch_col + 2);
  CHECK(f.browser->tags().selected() == static_cast<int>(f.browser->tags().tags().size()) - 1);
  CHECK(f.text().find("Showing Watchlist") != std::string::npos);
  // Wide enough for one line, they are on one.
  Fixture wide(true, 200);
  CHECK(cellOf(wide.text(), "Watchlist").first == 0);
}

TEST(the_menu_opens_in_place_under_its_button_and_the_screen_behind_is_as_it_was) {
  Fixture f(false);
  addNokia(*f.store);
  f.app->execute("show-nasdaq-helsinki");
  f.browser->table().select(0);
  const std::string before = f.text();
  const auto [button_row, button_col] = cellOf(before, kMenuGlyphText);
  CHECK(button_row >= 0);
  std::string shown;
  f.inModal([&] { f.app->execute("row-menu"); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  f.press(Key::Down);  // the Escape's key again
  // The first entry is on the line under the button, and the right edge of the menu is at the button's column.
  const auto [info_row, info_col] = cellOf(shown, "Show info");
  CHECK(info_row == button_row + 1 && info_col <= button_col);
  // What was there is there still: the rows above the menu are as they were, and the table's first row too (nothing is dimmed or blurred
  // away; the text is what the test sees, so only the cells under the menu differ).
  auto line = [](const std::string & text, int n) {
    size_t at = 0;
    for (int i = 0; i < n; i++) at = text.find('\n', at) + 1;
    return text.substr(at, text.find('\n', at) - at);
  };
  for (int n = 0; n <= button_row; n++) CHECK(line(shown, n) == line(before, n));
  CHECK(shown.find("Nasdaq Helsinki") != std::string::npos && shown.find("Nokia") != std::string::npos);
}
