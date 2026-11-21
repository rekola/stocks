#include "TestFramework.h"

#include "Browser.h"
#include "Quotes.h"
#include "Synthetic.h"
#include "source/Source.h"
#include "source/InstrumentLists.h"

#include <cellui/Application.h>
#include <cellui/TestBackend.h>

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
  explicit Fixture(bool synthetic = true) {
    if (synthetic) fillSyntheticData(*store, instrumentList());
    auto owned = std::make_unique<test::TestBackend>(24, 100);
    backend = owned.get();
    app = Application::adopt(std::move(owned));
    browser = std::make_unique<Browser>(*app, store);
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
};

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
  Fixture f;
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
  for (int p = 1; p < quotes.rowCount(); p++) CHECK(quotes.rows[static_cast<size_t>(table.rowAt(p - 1))].price <= quotes.rows[static_cast<size_t>(table.rowAt(p))].price);
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
  f.store->appendCandles({eur.id, usd.id, currencies.id, a.id}, interval::day, {{0, 1, 1, 1, 1.00, 1}, {1000, 1, 1, 1, 1.10, 1}});
  f.store->appendCandles({eur.id, usd.id, currencies.id, b.id}, interval::day, {{0, 1, 1, 1, 2.00, 1}, {1000, 1, 1, 1, 1.00, 1}});
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
  const char * markets[] = {"Commodities", "Nasdaq Helsinki", "Currencies"};
  int i = 0;
  for (const char * command : {"show-commodities", "show-nasdaq-helsinki", "show-currencies"}) {
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
  f.store->appendCandles(eurusd, interval::day, {{0, 1, 1, 1, 1.0, 1}, {1000, 1, 1, 1, 1.1, 1}});
  f.app->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() == 1);
  t.sortBy(Quotes::PRICE, SortOrder::ASCENDING);

  f.store->appendCandles(gbpusd, interval::day, {{0, 1, 1, 1, 1.2, 1}, {1000, 1, 1, 1, 1.3, 1}});
  f.app->widgets().postToRoot(StoreChanged{});  // what the source thread's post turns into
  CHECK(t.model()->rowCount() == 2);
  CHECK(t.sortColumn() == Quotes::PRICE);
  CHECK(t.model()->text(t.rowAt(0), Quotes::BASE) == "EUR" && t.model()->text(t.rowAt(1), Quotes::BASE) == "GBP");

  t.select(t.rowAt(1));  // GBP; new data for EUR must not move the selection
  f.store->appendCandles(eurusd, interval::day, {{2000, 1, 1, 1, 1.5, 1}});
  f.app->widgets().postToRoot(StoreChanged{});
  CHECK(t.model()->text(t.selectedRow(), Quotes::BASE) == "GBP");
  CHECK(t.model()->text(t.rowAt(1), Quotes::BASE) == "EUR");  // 1.5 sorts after 1.3

  f.app->widgets().postToRoot(SourceMessage{"yahoo: NOKIA.HE: HTTP 404"});
  CHECK(f.text().find("yahoo: NOKIA.HE: HTTP 404") != std::string::npos);
}
