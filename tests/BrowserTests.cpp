#include "TestFramework.h"

#include "AttributeView.h"
#include "Browser.h"
#include "Quotes.h"
#include "Synthetic.h"
#include "model/Dates.h"
#include "source/Source.h"
#include "source/InstrumentLists.h"

#include <cellui/Application.h>
#include <cellui/TestBackend.h>

#include <clocale>

using namespace cellui;

namespace {

// The lists on disk, which the made-up data follows.
const InstrumentList & instrumentList() {
  static const InstrumentList u = [] {
    std::string error;
    InstrumentList loaded = loadInstrumentLists(std::filesystem::path(STOCKS_DATA_DIR) / "yahoo", error);
    if (loaded.empty()) std::fprintf(stderr, "  lists: %s\n", error.c_str());
    return loaded;
  }();
  return u;
}

size_t countInMarket(const std::string & market) {
  size_t n = 0;
  for (const ListEntry & e : instrumentList()) n += e.market == market;
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
  Instrument eur = store.addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument nokia = store.addInstrument({{}, "", "Nokia Oyj", InstrumentKind::Company, "FI0009000681"});
  Market helsinki = store.addMarket({{}, "Nasdaq Helsinki"});
  Dataset data = store.addDataset({{}, "nordnet"});
  EdgeKey key{nokia.id, eur.id, helsinki.id, data.id};
  store.appendCandles(key, Resolution::Day, {{dayNumber(2026, 1, 2), 1, 1, 1, 4.2, 1}, {dayNumber(2026, 1, 5), 1, 1, 1, 4.3, 1}});
  store.setListing({key, "NOKIA", "1234", "5678", "STOCK", "Nokia"});
  store.setAttributes(nokia.id, {{"market-cap", 12345.675}, {"report-day", dayNumber(2026, 3, 5)}, {"owners", int64_t{250000}}});
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
  Instrument eur = f.store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument usd = f.store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Market currencies = f.store->addMarket({{}, "Currencies"});
  Dataset a = f.store->addDataset({{}, "provider-a"}), b = f.store->addDataset({{}, "provider-b"});
  f.store->appendCandles({eur.id, usd.id, currencies.id, a.id}, Resolution::Day, {{0, 1, 1, 1, 1.00, 1}, {1000, 1, 1, 1, 1.10, 1}});
  f.store->appendCandles({eur.id, usd.id, currencies.id, b.id}, Resolution::Day, {{0, 1, 1, 1, 2.00, 1}, {1000, 1, 1, 1, 1.00, 1}});
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() == 2);  // the same pair on the same market from two datasets: two rows
  t.sortBy(Quotes::DATASET, SortOrder::ASCENDING);
  CHECK(t.model()->text(t.rowAt(0), Quotes::DATASET) == "provider-a" && t.model()->text(t.rowAt(1), Quotes::DATASET) == "provider-b");
  CHECK(t.model()->text(t.rowAt(0), Quotes::BASE) == "EUR" && t.model()->text(t.rowAt(0), Quotes::QUOTE) == "USD");
  CHECK(t.model()->text(t.rowAt(0), Quotes::PRICE) == "1.10" && t.model()->text(t.rowAt(0), Quotes::CHANGE) == "+10.00%");
  CHECK(t.model()->text(t.rowAt(1), Quotes::PRICE) == "1.00" && t.model()->text(t.rowAt(1), Quotes::CHANGE) == "-50.00%");
  CHECK(t.model()->trace(t.rowAt(1), Quotes::HISTORY).size() == 2);
}

TEST(a_preset_whose_market_has_no_data_shows_no_rows_and_a_book_alone_is_not_a_row) {
  Fixture f(false);
  Instrument eur = f.store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument usd = f.store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Market currencies = f.store->addMarket({{}, "Currencies"});
  Dataset a = f.store->addDataset({{}, "provider-a"});
  CHECK(f.browser->table().model()->rowCount() == 0);  // Commodities: no such market
  f.store->appendBooks({eur.id, usd.id, currencies.id, a.id}, {{0, {{1, 1}}, {{2, 1}}}});
  f.app->execute("show-currencies");
  CHECK(f.browser->table().model()->rowCount() == 0);
}

TEST(the_made_up_data_fills_every_preset_with_what_the_lists_hold) {
  Fixture f;
  const char * markets[] = {"Nasdaq Helsinki", "Currencies", "Indices"};  // the commodities come from Nordnet, not the lists
  int i = 0;
  for (const char * command : {"show-nasdaq-helsinki", "show-currencies", "show-indices"}) {
    f.app->execute(command);
    CHECK(f.browser->table().model()->rowCount() > 0);
    CHECK(static_cast<size_t>(f.browser->table().model()->rowCount()) == countInMarket(markets[i++]));
    CHECK(f.browser->table().model()->text(f.browser->table().rowAt(0), Quotes::DATASET) == "synthetic");
  }
}

TEST(the_table_follows_the_store_when_told_it_changed_and_keeps_the_sort_and_the_selection) {
  Fixture f(false);
  Instrument eur = f.store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument usd = f.store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Instrument gbp = f.store->addInstrument({{}, "GBP", "British pound", InstrumentKind::Currency});
  Market currencies = f.store->addMarket({{}, "Currencies"});
  Dataset a = f.store->addDataset({{}, "provider-a"});
  EdgeKey eurusd{eur.id, usd.id, currencies.id, a.id}, gbpusd{gbp.id, usd.id, currencies.id, a.id};
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

TEST(a_row_shows_what_the_dataset_calls_the_listing_so_the_rows_of_one_base_are_told_apart) {
  Fixture f(false);
  Instrument eur = f.store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument usd = f.store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Instrument gbp = f.store->addInstrument({{}, "GBP", "British pound", InstrumentKind::Currency});
  Market currencies = f.store->addMarket({{}, "Currencies"});
  Dataset a = f.store->addDataset({{}, "provider-a"});
  EdgeKey eurusd{eur.id, usd.id, currencies.id, a.id}, eurgbp{eur.id, gbp.id, currencies.id, a.id};
  for (const EdgeKey & k : {eurusd, eurgbp}) f.store->appendCandles(k, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  f.store->setListing({eurusd, "EURUSD", "", "", "CURRENCY", "EUR/USD"});
  f.store->setListing({eurgbp, "EURGBP", "", "", "CURRENCY", "EUR/GBP"});
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  std::set<std::string> names;
  for (int r = 0; r < t.model()->rowCount(); r++) names.insert(t.model()->text(r, Quotes::NAME));
  CHECK((names == std::set<std::string>{"EUR/USD", "EUR/GBP"}));  // not "Euro" on both

  // An instrument listed on two markets: each market's own name for it; one with no name of its own shows the instrument's.
  Instrument fund = f.store->addInstrument({{}, "", "Fund (instrument)", InstrumentKind::Etf, "IE00BGV5VN51"});
  Market hel = f.store->addMarket({{}, "Nasdaq Helsinki"}), sto = f.store->addMarket({{}, "Nasdaq Stockholm"});
  EdgeKey onHel{fund.id, eur.id, hel.id, a.id}, onSto{fund.id, usd.id, sto.id, a.id};
  for (const EdgeKey & k : {onHel, onSto}) f.store->appendCandles(k, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  f.store->setListing({onHel, "XAIX", "", "", "UETF", "Fund (Helsinki)"});
  f.store->setListing({onSto, "XAIX", "", "", "UETF", ""});
  auto snapshot = f.store->snapshot();
  CHECK(makeRows(*snapshot, hel.id)[0].name == "Fund (Helsinki)");
  CHECK(makeRows(*snapshot, sto.id)[0].name == "Fund (instrument)");
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
  const EdgeKey k = snapshot->edges(snapshot->findMarket("Currencies")->id)[0];
  f.store->appendCandles(k, Resolution::Day, {{99999, 1, 1, 1, 2.0, 1}});
  f.app->widgets().postToRoot(StoreChanged{});  // what the source thread's post turns into
  CHECK(t.firstVisiblePosition() == first);      // not back at the first row
  CHECK(t.selectedRow() == selected);
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
  CHECK((markets == std::set<std::string>{"Currencies", "Indices", "Nasdaq Helsinki"}));  // the made-up data's markets

  // A market with no name yet is told by Nordnet's id for it, and one with neither by its number.
  Fixture g(false);
  Instrument eur = g.store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument usd = g.store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Market unnamed = g.store->resolveMarket({{predicate::nordnetId, "61"}});
  Dataset a = g.store->addDataset({{}, "nordnet"});
  g.store->appendCandles({eur.id, usd.id, unnamed.id, a.id}, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  g.app->execute("show-all");
  CHECK(g.browser->table().model()->text(0, Quotes::MARKET) == "Nordnet 61");
}

TEST(a_currency_pair_shows_its_first_currency_as_the_base_and_not_the_listing_symbol) {
  Fixture f(false);
  Instrument eur = f.store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument usd = f.store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Market currencies = f.store->addMarket({{}, "Currencies"});
  Dataset a = f.store->addDataset({{}, "nordnet"});
  EdgeKey k{eur.id, usd.id, currencies.id, a.id};
  f.store->appendCandles(k, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  f.store->setListing({k, "EURUSD", "", "", "CURRENCY", "EUR/USD"});
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.model()->text(0, Quotes::BASE) == "EUR" && t.model()->text(0, Quotes::QUOTE) == "USD");
  CHECK(t.model()->text(0, Quotes::NAME) == "EUR/USD");

  // What a market calls anything else stays the base: its symbol there.
  Instrument fund = f.store->addInstrument({{}, "", "Fund", InstrumentKind::Etf, "IE00BGV5VN51"});
  Market hel = f.store->addMarket({{}, "Nasdaq Helsinki"});
  EdgeKey onHel{fund.id, eur.id, hel.id, a.id};
  f.store->appendCandles(onHel, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  f.store->setListing({onHel, "XAIX", "", "", "UETF", "Fund"});
  f.app->execute("show-nasdaq-helsinki");
  CHECK(t.model()->text(0, Quotes::BASE) == "XAIX");
}

TEST(a_busy_source_is_shown_at_most_so_often_and_the_last_change_is_shown_when_it_is_time) {
  Fixture f(false);
  Instrument eur = f.store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument usd = f.store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Instrument gbp = f.store->addInstrument({{}, "GBP", "British pound", InstrumentKind::Currency});
  Market currencies = f.store->addMarket({{}, "Currencies"});
  Dataset a = f.store->addDataset({{}, "provider-a"});
  f.store->appendCandles({eur.id, usd.id, currencies.id, a.id}, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}});
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() == 1);

  f.browser->setMinRefreshInterval(3600);  // not for an hour
  f.store->appendCandles({eur.id, gbp.id, currencies.id, a.id}, Resolution::Day, {{0, 1, 1, 1, 0.9, 1}});
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

TEST(a_click_on_the_info_button_selects_its_row_and_opens_the_viewer) {
  Fixture f(false);
  addNokia(*f.store);
  f.app->execute("show-nasdaq-helsinki");
  f.browser->table().select(-1);
  auto [row, col] = cellOf(f.text(), kInfoGlyph);
  CHECK(row >= 0);
  std::string shown;
  const bool closed = f.inModal([&] { f.click(row, col); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
  CHECK(f.browser->table().selectedRow() == 0);
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
  auto open_first_visible = [&] {
    auto [row, col] = cellOf(f.text(), kInfoGlyph);  // the first row in view
    CHECK(row >= 0);
    const int expected = table.rowAt(table.firstVisiblePosition());
    std::string shown;
    f.inModal([&] { f.click(row, col); }, {[&] { shown = f.text(); }, [&] { f.press(Key::Escape); }});
    CHECK(table.selectedRow() == expected);
    auto isin = f.store->snapshot()->instrument(quotes.edge(expected).base)->isin;
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
