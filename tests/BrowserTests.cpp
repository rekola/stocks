#include "TestFramework.h"

#include "Browser.h"
#include "CommandDescriptions.h"
#include "Quotes.h"
#include "Synthetic.h"

#include <cellui/Application.h>
#include <cellui/TestBackend.h>

using namespace cellui;

namespace {

struct Fixture {
  test::TestBackend * backend;
  std::unique_ptr<Application> app;
  std::shared_ptr<Store> store = makeMemoryStore();
  std::unique_ptr<Browser> browser;
  // Over the made-up data, or over a store the test has filled.
  explicit Fixture(bool synthetic = true) {
    if (synthetic) fillSyntheticData(*store);
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
  std::string text() {
    app->present();
    return backend->text();
  }
};

}  // namespace

TEST(every_command_has_a_description) {
  Fixture f;
  CHECK(!f.browser->commandNames().empty());
  for (const std::string & name : f.browser->commandNames()) {
    if (commandDescription(name).empty()) std::fprintf(stderr, "  no description: %s\n", name.c_str());
    CHECK(!commandDescription(name).empty());
  }
}

TEST(meta_x_opens_the_palette_and_a_chosen_command_runs) {
  Fixture f;
  CHECK(!f.browser->paletteOpen());
  f.press('x', kModAlt);
  CHECK(f.browser->paletteOpen());
  CHECK(f.text().find("show-currencies") != std::string::npos);  // the list is as tall as the screen allows
  f.type("sort-by-pr");
  CHECK(f.text().find("sort-by-price") != std::string::npos && f.text().find("sort-by-name") == std::string::npos);
  f.press(Key::Enter);
  CHECK(!f.browser->paletteOpen());
  CHECK(f.browser->table().sortColumn() == 4 && f.browser->table().sortOrder() == SortOrder::ASCENDING);
}

TEST(the_palette_takes_every_key_while_open_and_escape_closes_it_without_running) {
  Fixture f;
  f.press('x', kModAlt);
  f.press('q');  // typed into the query, not a quit
  CHECK(f.browser->paletteOpen());
  f.press('g', kModCtrl);  // cancel (a lone Escape waits for the key after it, in case it is a Meta prefix)
  CHECK(!f.browser->paletteOpen());
  CHECK(f.browser->table().sortColumn() == -1);
}

TEST(the_palette_lists_the_bindings_of_the_commands) {
  Fixture f;
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
  CHECK(f.browser->execute("show-currencies"));
  CHECK(f.browser->tags().selected() == 2);
  bool euro = false;  // the rows come in the store's order, not a hand-written one
  for (int r = 0; r < f.browser->table().model()->rowCount(); r++)
    euro = euro || f.browser->table().model()->text(f.browser->table().rowAt(r), 2) == "Euro";
  CHECK(euro);
  CHECK(f.browser->execute("previous-preset"));
  CHECK(f.browser->tags().selected() == 1);
  CHECK(!f.browser->execute("no-such-command"));
}

TEST(sort_commands_reverse_and_clear) {
  Fixture f;
  CHECK(f.browser->execute("sort-by-name"));
  CHECK(f.browser->table().sortColumn() == 2);
  CHECK(f.browser->execute("reverse-sort"));
  CHECK(f.browser->table().sortOrder() == SortOrder::DESCENDING);
  CHECK(f.browser->execute("clear-sort"));
  CHECK(f.browser->table().sortColumn() == -1);
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

TEST(the_status_line_says_what_a_preset_shows) {
  Fixture f;
  f.browser->execute("show-nasdaq-helsinki");
  CHECK(f.text().find("Showing Nasdaq Helsinki") != std::string::npos);
}

TEST(a_preset_shows_one_row_per_edge_with_its_dataset) {
  Fixture f(false);
  Instrument eur = f.store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument usd = f.store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Market currencies = f.store->addMarket({{}, "Currencies"});
  Dataset a = f.store->addDataset({{}, "provider-a"}), b = f.store->addDataset({{}, "provider-b"});
  f.store->appendCandles({eur.id, usd.id, currencies.id, a.id}, 1000, {{0, 1, 1, 1, 1.00, 1}, {1000, 1, 1, 1, 1.10, 1}});
  f.store->appendCandles({eur.id, usd.id, currencies.id, b.id}, 1000, {{0, 1, 1, 1, 2.00, 1}, {1000, 1, 1, 1, 1.00, 1}});
  f.browser->execute("show-currencies");
  Table & t = f.browser->table();
  CHECK(t.model()->rowCount() == 2);  // the same pair on the same market from two datasets: two rows
  f.browser->execute("sort-by-dataset");
  CHECK(t.model()->text(t.rowAt(0), 3) == "provider-a" && t.model()->text(t.rowAt(1), 3) == "provider-b");
  CHECK(t.model()->text(t.rowAt(0), 0) == "EUR" && t.model()->text(t.rowAt(0), 1) == "USD");
  CHECK(t.model()->text(t.rowAt(0), 4) == "1.10" && t.model()->text(t.rowAt(0), 5) == "+10.00%");
  CHECK(t.model()->text(t.rowAt(1), 4) == "1.00" && t.model()->text(t.rowAt(1), 5) == "-50.00%");
  CHECK(t.model()->trace(t.rowAt(1), 6).size() == 2);
}

TEST(a_preset_whose_market_has_no_data_shows_no_rows_and_a_book_alone_is_not_a_row) {
  Fixture f(false);
  Instrument eur = f.store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency});
  Instrument usd = f.store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Market currencies = f.store->addMarket({{}, "Currencies"});
  Dataset a = f.store->addDataset({{}, "provider-a"});
  CHECK(f.browser->table().model()->rowCount() == 0);  // Commodities: no such market
  f.store->appendBooks({eur.id, usd.id, currencies.id, a.id}, {{0, {{1, 1}}, {{2, 1}}}});
  f.browser->execute("show-currencies");
  CHECK(f.browser->table().model()->rowCount() == 0);
}

TEST(the_made_up_data_fills_every_preset) {
  Fixture f;
  for (const char * command : {"show-commodities", "show-nasdaq-helsinki", "show-currencies"}) {
    f.browser->execute(command);
    CHECK(f.browser->table().model()->rowCount() == 12);
    CHECK(f.browser->table().model()->text(f.browser->table().rowAt(0), 3) == "synthetic");
  }
}
