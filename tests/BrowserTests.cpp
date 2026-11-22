#include "TestFramework.h"

#include "Browser.h"
#include "model/Dates.h"
#include "Quotes.h"
#include "Synthetic.h"
#include "source/Source.h"
#include "source/InstrumentLists.h"

#include <fstream>
#include <sstream>
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
  explicit Fixture(bool synthetic = true, int cols = 100) {
    if (synthetic) fillSyntheticData(*store, instrumentList());
    auto owned = std::make_unique<test::TestBackend>(24, cols);
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

#include "FakeHttp.h"

TEST(the_rows_on_screen_have_their_history_loaded_and_the_table_follows) {
  std::shared_ptr<Store> store = makeMemoryStore();
  Instrument usd = store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Market market = store->addMarket({{}, "Nasdaq Helsinki"});
  Dataset nordnet = store->addDataset({{}, "nordnet"});
  std::vector<EdgeKey> edges;
  for (int i = 0; i < 40; i++) {  // more rows than the screen holds
    Instrument inst = store->addInstrument({{}, "", "Stock " + std::to_string(i), InstrumentKind::Company, "XX" + std::to_string(100000 + i)});
    EdgeKey k{inst.id, usd.id, market.id, nordnet.id};
    store->appendCandles(k, Resolution::Day, {{dayNumber(2026, 12, 1), kUndefined, 1, 1, 1, kUndefined}});
    store->setListing({k, "S" + std::to_string(i), "", "book-" + std::to_string(i), "ESH", "Stock " + std::to_string(i)});
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
