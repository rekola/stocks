#include "TestFramework.h"

#include "AttributeView.h"
#include "Browser.h"
#include "model/Dates.h"
#include "Quotes.h"
#include "Synthetic.h"
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
  explicit Fixture(bool synthetic = true, int cols = 100) {
    if (synthetic) fillSyntheticData(*store, instrumentList());
    auto owned = std::make_unique<test::TestBackend>(24, cols);
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
  DatasetId data = store.addDataset("nordnet");
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
  CHECK(f.text().find("show-currencies") != std::string::npos);  // the list is as tall as the screen allows
  f.type("show-curr");
  CHECK(f.text().find("show-currencies") != std::string::npos && f.text().find("show-commodities") == std::string::npos);
  f.press(Key::Enter);
  CHECK(!f.app->commandPaletteOpen());
  CHECK(f.browser->tags().selected() == 2);
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
  // The Price heading, on the table's first row.
  std::string text = f.text();
  size_t line = 0;
  for (int row = 0; row < 2; row++) line = text.find('\n', line) + 1;
  int col = static_cast<int>(text.find("Price", line) - line);
  CHECK(col > 0 && text.find('\n', line) > line + static_cast<size_t>(col));
  f.click(2, col);
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

TEST(tab_moves_the_keyboard_between_the_table_and_the_tags) {
  Fixture f;
  CHECK(f.app->input().focused() == &f.browser->table());
  f.press(Key::Tab);
  CHECK(f.app->input().focused() == &f.browser->tags());
  f.press(Key::Right);
  CHECK(f.browser->tags().selected() == 1);
}

TEST(a_preset_shows_one_row_per_edge_with_its_dataset) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = f.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId currencies = f.store->addMarket("XXXX", "Currencies");
  DatasetId a = f.store->addDataset("providera"), b = f.store->addDataset("providerb");
  f.store->appendCandles({eur, usd, currencies, a}, Resolution::Day, {{0, 1, 1, 1, 1.00, 1}, {1000, 1, 1, 1, 1.10, 1}});
  f.store->appendCandles({eur, usd, currencies, b}, Resolution::Day, {{0, 1, 1, 1, 2.00, 1}, {1000, 1, 1, 1, 1.00, 1}});
  f.app->execute("show-currencies");
  f.app->execute("show-period-day");  // the bars are a thousand days apart: the day is the change since the previous bar
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() == 2);  // the same pair on the same market from two datasets: two rows
  t.sortBy(Quotes::DATASET, SortOrder::ASCENDING);
  CHECK(t.model()->text(t.rowAt(0), Quotes::DATASET) == "providera" && t.model()->text(t.rowAt(1), Quotes::DATASET) == "providerb");
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
  // The commodities come from Nordnet, not the lists. Currencies and indices
  // are on no market (XXXX), told apart by their kind.
  const std::pair<const char *, size_t> expected[] = {
      {"show-nasdaq-helsinki", countOf("XHEL")},
      {"show-currencies", countOf("XXXX", InstrumentKind::Currency)},
      {"show-indices", countOf("XXXX", InstrumentKind::Index)}};
  for (const auto &[command, count] : expected) {
    f.app->execute(command);
    CHECK(f.browser->table().model()->rowCount() > 0);
    CHECK(static_cast<size_t>(f.browser->table().model()->rowCount()) == count);
    CHECK(f.browser->table().model()->text(f.browser->table().rowAt(0), Quotes::DATASET) == "synthetic");
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
  DatasetId nordnet = store->addDataset("nordnet");
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
  DatasetId nordnet = store->addDataset("nordnet");
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
  DatasetId nordnet = store->addDataset("nordnet");
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
  DatasetId a = g.store->addDataset("nordnet");
  g.store->appendCandles({eur, usd, unnamed, a}, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  g.app->execute("show-all");
  CHECK(g.browser->table().model()->text(0, Quotes::MARKET) == "XNYS");
}

TEST(a_currency_pair_shows_its_first_currency_as_the_base_and_not_the_listing_symbol) {
  Fixture f(false);
  InstrumentId eur = f.store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  InstrumentId usd = f.store->addInstrument("USD", "US dollar", InstrumentKind::Currency);
  MarketId currencies = f.store->addMarket("XXXX", "Currencies");
  DatasetId a = f.store->addDataset("nordnet");
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
  std::string end;
  const bool closed_by_escape = f.inModal([&] { f.press('i'); },
                                          {[&] { shown = f.text(); }, [&] { f.press(Key::End); }, [&] { end = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(shown.find("FI0009000681") != std::string::npos);  // the instrument's ISIN
  CHECK(shown.find("NOKIA") != std::string::npos);         // the listing's symbol
  CHECK(shown.find("12345.675") != std::string::npos);     // a real number, with a point
  CHECK(end.find("2026-03-05") != std::string::npos);      // a day number, as a date (below, where the keys scrolled to)
  CHECK(end.find("2 bars, 2026-01-02 to 2026-01-05") != std::string::npos);
  CHECK(closed_by_escape);
  CHECK(f.text().find("Esc: close") == std::string::npos);
  // The Emacs key for it, from the terminal keymap.
  shown.clear();
  f.inModal([&] { f.press('x', kModCtrl), f.press('='); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(shown.find("FI0009000681") != std::string::npos);
}

TEST(a_click_on_the_info_button_opens_its_row_and_leaves_the_selection) {
  Fixture f(false);
  addNokia(*f.store);
  f.app->execute("show-nasdaq-helsinki");
  f.browser->table().select(-1);
  auto [row, col] = cellOf(f.text(), kInfoGlyph);
  CHECK(row >= 0);
  std::string shown;
  const bool closed = f.inModal([&] { f.click(row, col); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(f.browser->table().selectedRow() == -1);  // the click chose nothing
  CHECK(shown.find("FI0009000681") != std::string::npos && shown.find("NOKIA") != std::string::npos);
  CHECK(closed);
}

TEST(the_command_without_a_selected_row_says_so) {
  Fixture f(false);
  f.app->execute("show-info");
  CHECK(f.text().find("No row selected") != std::string::npos);
}

TEST(the_buttons_follow_a_sort_and_leave_the_scroll_position_alone) {
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
    auto [row, col] = cellOf(f.text(), kInfoGlyph);  // the first row in view
    CHECK(row >= 0);
    const int expected = table.rowAt(table.firstVisiblePosition());
    std::string shown;
    f.inModal([&] { f.click(row, col); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
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

TEST(values_are_shown_by_what_they_are_without_the_locale) {
  CHECK(showValue("report-day", int64_t{dayNumber(2026, 3, 5)}).text == "2026-03-05");
  CHECK(showValue("first-trade-day", int64_t{dayNumber(1999, 12, 31)}).text == "1999-12-31");
  CHECK(showValue("owners", int64_t{250000}).text == "250000");  // not a day
  CHECK(showValue("tradable", true).text == "yes");
  CHECK(showValue("name", std::string("Nokia")).text == "Nokia");
  CHECK(showValue("x", std::monostate{}).text.empty());
  const std::string before = std::setlocale(LC_NUMERIC, nullptr);
  for (const char * name : {"de_DE.UTF-8", "fi_FI.UTF-8", "de_DE", "fi_FI"})
    if (std::setlocale(LC_NUMERIC, name)) break;  // a decimal comma, where the system has one
  CHECK(realText(0.35) == "0.35");
  CHECK(realText(1234.5) == "1234.5");
  CHECK(realText(42) == "42");
  CHECK(showValue("dividend-amount", 0.7).text == "0.7");
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
  Fixture f(false);
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
  CHECK(f.browser->period() == Period::Year);  // round the end of the list
  f.app->execute("next-period");
  CHECK(f.browser->period() == Period::Day);
  f.app->execute("show-period-month");
  CHECK(f.browser->period() == Period::Month && f.browser->periodTags().selected() == static_cast<int>(Period::Month));
}

TEST(a_key_cycles_the_period_and_a_click_on_a_tag_chooses_it) {
  Fixture f(false);
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

TEST(a_period_without_bars_shows_no_number_no_trace_and_no_colour) {
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
    CHECK(t.model()->trace(r, Quotes::HISTORY).empty());
    CHECK(!t.model()->color(r, Quotes::CHANGE) && !t.model()->color(r, Quotes::HISTORY));
    CHECK(t.model()->text(r, Quotes::PRICE) == "5.00");  // the price is there
  }
  // Three months of bars are not a year: the first row has no change for the year either.
  f.app->execute("show-period-year");
  int first = -1;
  for (int i = 0; i < quotes.rowCount(); i++)
    if (quotes.edge(i) == rows.riseFall) first = i;
  CHECK(t.model()->text(first, Quotes::CHANGE).empty() && t.model()->trace(first, Quotes::HISTORY).empty());
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
  PeriodView v = periodView(*snapshot(), rows.riseFall, Period::Day, true);
  // The session's daily bar (22) is newer than its one five-minute bar (23): it is the last price, and the trace ends with it.
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

TEST(a_newer_daily_bar_is_the_last_price_of_the_day_week_and_year_and_ends_their_trace) {
  for (Period period : {Period::Day, Period::Week, Period::Year}) {
    auto store = makeMemoryStore();
    // The daily bar of today has moved on since the series was loaded.
    EdgeKey k = seriesWorld(*store, period, {{kToday - 1, 1, 1, 1, 9, 1}, {kToday, 1, 1, 1, 12, 1}});
    PeriodView v = periodView(*store->snapshot(), k, period, true);
    CHECK(v.last == 12 && !v.closes.empty() && v.closes.back() == 12.0f);
    CHECK(v.change() > 0);
    // The same close as the series' last bar adds no point.
    PeriodView same = periodView(*store->snapshot(), k, period, false);
    CHECK(same.last == 12 && same.base == v.base);
    store->appendCandles(k, Resolution::Day, {{kToday, 1, 1, 1, 10, 1}});
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
  Fixture wide(true, 100);
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
