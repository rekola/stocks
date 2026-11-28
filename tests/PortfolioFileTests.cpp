#include "TestFramework.h"

#include "model/Dates.h"
#include "model/NodeKey.h"
#include "model/SqliteFactStore.h"
#include "model/Store.h"
#include "portfolio/Portfolio.h"
#include "portfolio/Transaction.h"
#include "source/DatasetCodes.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <variant>

namespace {

namespace fs = std::filesystem;

// A directory of its own, gone with the object, with the two files in it.
struct Dir {
  fs::path dir, facts, portfolios;
  Dir() {
    static std::atomic<int> counter{0};
    dir = fs::temp_directory_path() / ("stocks-portfolio-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                       std::to_string(counter++));
    fs::create_directories(dir);
    facts = dir / "facts.sqlite";
    portfolios = dir / "portfolios.sqlite";
  }
  ~Dir() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  // The files moved aside with this mark, one for each time (their WAL files are not counted).
  size_t files(const std::string & mark) const {
    size_t n = 0;
    for (const auto & e : fs::directory_iterator(dir))
      if (const std::string name = e.path().filename().string(); name.rfind("portfolios.sqlite" + mark, 0) == 0 && !name.ends_with("-wal") && !name.ends_with("-shm")) n++;
    return n;
  }
};

std::unique_ptr<Store> openStore(const Dir & d, std::vector<std::string> * said = nullptr, bool cache = true) {
  StoreOptions o;
  auto log = [said](const std::string & t) { if (said) said->push_back(t); };
  if (cache) o.facts = openSqliteFactStore(d.facts, log);
  o.portfolios = openSqlitePortfolioStore(d.portfolios, log);
  o.writer.changed_delay = std::chrono::milliseconds(1);
  o.writer.rechecked_delay = std::chrono::milliseconds(1);
  return makeMemoryStore(std::move(o));
}

struct Things {
  InstrumentId eur, nokia;
  MarketId hel, none;
  DatasetId user;
};
Things makeThings(Store & store) {
  Things t;
  t.eur = store.addInstrument("EUR", "Euro", InstrumentKind::Currency);
  t.nokia = store.addInstrument("", "Nokia Oyj", InstrumentKind::Company, "FI0009000681");
  t.hel = store.addMarket("XHEL", "Nasdaq Helsinki");
  t.none = store.addMarket(kNoMarket);
  t.user = store.addDataset(kUserDataset);
  return t;
}

portfolio::Transaction trade(const Things & t, uint64_t id, double quantity, double price, int64_t day, portfolio::Side side = portfolio::Side::Buy) {
  portfolio::Transaction x;
  x.id = id;
  x.day = day;
  x.side = side;
  x.instrument = t.nokia;
  x.market = t.hel;
  x.quote = t.eur;
  x.quantity = quantity;
  x.price = price;
  x.fee = 1.5;
  x.note = "a note, with \"quotes\" and ä";
  return x;
}

const PortfolioId kMine{portfolioKey(0x1234567890abcdull)->value};

// Every key in a table of a file, read with a connection of its own.
std::vector<uint64_t> keysIn(const fs::path & file, const char * sql) {
  std::vector<uint64_t> out;
  SQLite::Database db(file.string(), SQLite::OPEN_READONLY);
  SQLite::Statement q(db, sql);
  while (q.executeStep()) out.push_back(static_cast<uint64_t>(q.getColumn(0).getInt64()));
  return out;
}

}  // namespace

TEST(portfolios_come_back_after_a_restart_with_no_version_and_the_same_holdings) {
  Dir d;
  const auto day = dayNumber(2026, 5, 1);
  {
    auto store = openStore(d);
    const Things t = makeThings(*store);
    store->setAttributes(NodeRef(kMine), {{predicate::name, "Retirement"}, {predicate::currency, NodeKey{t.eur.value}}, {predicate::ledger, NodeKey{t.user.value}}});
    store->appendTransactions({kMine, t.user}, {trade(t, 11, 10, 4, day), trade(t, 12, 4, 6, day + 1, portfolio::Side::Sell), trade(t, 13, 5, 5, day + 2)});
  }
  auto store = openStore(d);
  auto s = store->snapshot();
  CHECK(s->version() == 0);  // restoring makes none
  CHECK(s->portfolios() == std::vector<PortfolioId>{kMine} && s->name(NodeRef(kMine)) == "Retirement");
  const auto currency = portfolio::currencyOf(*s, kMine);
  CHECK(currency && *currency == InstrumentId{currencyKey("EUR")->value});
  CHECK(s->findLinked(NodeType::Portfolio, predicate::currency, *currencyKey("EUR")).size() == 1);
  const auto ledger = *datasetKey(kUserDataset);
  const auto kept = s->transactions({kMine, DatasetId{ledger.value}});
  CHECK(kept.size() == 3 && kept[0].id == 11 && kept[0].quantity == 10 && kept[0].price == 4 && kept[0].fee == 1.5 && kept[0].day == day);
  CHECK(kept[0].note == "a note, with \"quotes\" and ä" && kept[1].side == portfolio::Side::Sell && kept[1].quantity == 4 && kept[2].id == 13);
  const auto held = portfolio::holdings(*s, kMine);
  CHECK(held.refused.empty() && held.positions.size() == 1 && held.positions[0].quantity == 11);
  CHECK_NEAR(held.positions[0].average_cost, ((10 * 4 + 1.5) * 6 / 10 + 5 * 5 + 1.5) / 11.0, 1e-9);
}

TEST(a_portfolio_is_never_in_the_cache_and_nothing_of_the_cache_is_in_the_portfolio_file) {
  Dir d;
  {
    auto store = openStore(d);
    const Things t = makeThings(*store);
    store->setAttribute(NodeRef(kMine), {predicate::name, "Mine"});
    store->appendTransactions({kMine, t.user}, {trade(t, 1, 1, 1, 100)});
    store->flush();
  }
  for (uint64_t key : keysIn(d.facts, "SELECT node_key FROM facts")) CHECK(NodeKey{key}.kind() != NodeKind::Portfolio);
  CHECK(!keysIn(d.facts, "SELECT node_key FROM facts").empty());
  const auto own = keysIn(d.portfolios, "SELECT node_key FROM facts");
  CHECK(own.size() == 1 && own[0] == kMine.value);
  CHECK(keysIn(d.portfolios, "SELECT portfolio FROM events").size() == 1);
  // Not even a portfolio planted in the cache is read: the file of the user's own is the only place they are.
  {
    auto cache = openSqliteFactStore(d.facts);
    FactBatch planted;
    planted.changed.push_back({portfolioKey(77)->value, "name", std::string("Planted"), 5});
    cache->apply(planted);
  }
  auto store = openStore(d);
  CHECK(store->snapshot()->portfolios() == std::vector<PortfolioId>{kMine});
}

TEST(a_write_is_in_the_file_when_it_returns) {
  Dir d;
  auto store = openStore(d);
  const Things t = makeThings(*store);
  store->setAttribute(NodeRef(kMine), {predicate::name, "At once"});
  store->appendTransactions({kMine, t.user}, {trade(t, 1, 1, 1, 100)});
  // Read with another connection while the store is still open and nothing was flushed.
  CHECK(keysIn(d.portfolios, "SELECT node_key FROM facts").size() == 1);
  CHECK(keysIn(d.portfolios, "SELECT id FROM events").size() == 1);
}

TEST(what_is_taken_back_is_taken_back_in_the_file_and_what_is_put_again_is_there) {
  Dir d;
  const PortfolioId other{portfolioKey(5)->value};
  {
    auto store = openStore(d);
    const Things t = makeThings(*store);
    store->setAttributes(NodeRef(kMine), {{predicate::name, "Gone"}, {"note", "x"}});
    store->setAttribute(NodeRef(other), {predicate::name, "Stays"});
    store->appendTransactions({kMine, t.user}, {trade(t, 1, 1, 1, 100), trade(t, 2, 2, 2, 101)});
    store->appendTransactions({other, t.user}, {trade(t, 3, 3, 3, 102)});
    store->removeTransactions({kMine, t.user}, {1});
    store->removeAttribute(NodeRef(kMine), "note");
    store->removePortfolio(other);
    // Said again in the same batch as it is taken back: it is there.
    store->batch([&](Batch & b) {
      b.removePortfolio(kMine);
      b.setAttribute(NodeRef(kMine), {predicate::name, "Back"});
      b.appendTransactions({kMine, t.user}, {trade(t, 4, 4, 4, 103)});
    });
  }
  auto store = openStore(d);
  auto s = store->snapshot();
  CHECK(s->portfolios() == std::vector<PortfolioId>{kMine} && s->name(NodeRef(kMine)) == "Back" && !s->attribute(NodeRef(kMine), "note"));
  const DatasetId user{datasetKey(kUserDataset)->value};
  CHECK(s->transactions({kMine, user}).size() == 1 && s->transactions({kMine, user})[0].id == 4 && s->transactions({other, user}).empty());
  CHECK(keysIn(d.portfolios, "SELECT id FROM events").size() == 1 && keysIn(d.portfolios, "SELECT node_key FROM facts").size() == 1);
  // And gone for good once it is taken back and the program ends.
  store->removePortfolio(kMine);
  store.reset();
  CHECK(openStore(d)->snapshot()->portfolios().empty());
}

TEST(a_file_of_another_schema_is_moved_aside_with_what_it_held_and_a_new_one_started) {
  Dir d;
  {
    SQLite::Database db(d.portfolios.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE facts (node_key INTEGER NOT NULL, predicate TEXT NOT NULL, value, tag INTEGER NOT NULL DEFAULT 0, updated INTEGER NOT NULL, "
            "PRIMARY KEY (node_key, predicate)) WITHOUT ROWID");
    db.exec("INSERT INTO facts VALUES (" + std::to_string(portfolioKey(1)->value) + ", 'name', 'Typed by hand', 0, 1)");
    db.exec("PRAGMA user_version = 99");
  }
  std::vector<std::string> said;
  {
    auto store = openStore(d, &said);
    CHECK(store->snapshot()->portfolios().empty());  // the old one is not read: nothing is migrated
    store->setAttribute(NodeRef(kMine), {predicate::name, "New"});
  }
  CHECK(d.files(".old-") == 1 && d.files(".bad-") == 0);
  bool told = false;
  for (const std::string & line : said) told |= line.find("schema 99") != std::string::npos && line.find("moved aside") != std::string::npos;
  CHECK(told);
  // What she typed is in the moved file, as it was.
  bool found = false;
  for (const auto & e : fs::directory_iterator(d.dir))
    if (e.path().filename().string().find("portfolios.sqlite.old-") == 0 && e.path().extension() != ".shm" && e.path().filename().string().find("-wal") == std::string::npos) {
      SQLite::Database db(e.path().string(), SQLite::OPEN_READONLY);
      found = db.execAndGet("SELECT value FROM facts").getString() == "Typed by hand" && db.execAndGet("PRAGMA user_version").getInt() == 99;
    }
  CHECK(found);
  CHECK(openStore(d)->snapshot()->name(NodeRef(kMine)) == "New");  // and the new one is the file now
  // Older, too, is another schema: the same.
  {
    auto store = openStore(d);
    store.reset();
    SQLite::Database db(d.portfolios.string(), SQLite::OPEN_READWRITE);
    db.exec("PRAGMA user_version = 0");
  }
  openStore(d);
  CHECK(d.files(".old-") == 2);
}

// A key of schema 1, packed as it was (six bits a character, ten at most, the case kept) and written here on its own, so that this checks
// the upgrade against the old way and not against the code that does it.
static uint64_t keyOfSchema1(uint64_t kind, const std::string & code) {
  uint64_t packed = 0;
  for (size_t i = 0; i < 10; i++) {
    uint64_t symbol = 0;
    if (i < code.size()) {
      const char c = code[i];
      symbol = c >= '0' && c <= '9' ? static_cast<uint64_t>(c - '0') + 1 : c >= 'A' && c <= 'Z' ? static_cast<uint64_t>(c - 'A') + 11 : static_cast<uint64_t>(c - 'a') + 37;
    }
    packed = (packed << 6) | symbol;
  }
  return (kind << 60) | packed;
}

TEST(a_file_of_schema_one_is_upgraded_to_the_keys_of_codes_as_they_are_packed_now) {
  Dir d;
  const uint64_t nokia = instrumentKey("FI0009000681")->value, mine = portfolioKey(7)->value;
  const uint64_t oldUser = keyOfSchema1(3, "user"), oldHel = keyOfSchema1(2, "XHEL"), oldEur = keyOfSchema1(1, "EUR"), oldPts = keyOfSchema1(1, "pts");
  const std::string body = "{\"side\":\"buy\",\"instrument\":" + std::to_string(nokia) + ",\"market\":" + std::to_string(oldHel) + ",\"quote\":" +
                           std::to_string(oldEur) + ",\"quantity\":10.0,\"price\":4.5,\"fee\":1.0,\"note\":\"by hand\"}";
  {
    SQLite::Database db(d.portfolios.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE facts (node_key INTEGER NOT NULL, predicate TEXT NOT NULL, value, tag INTEGER NOT NULL DEFAULT 0, updated INTEGER NOT NULL, "
            "PRIMARY KEY (node_key, predicate)) WITHOUT ROWID");
    db.exec("CREATE TABLE events (portfolio INTEGER NOT NULL, dataset INTEGER NOT NULL, id INTEGER NOT NULL, time INTEGER NOT NULL, body TEXT NOT NULL, "
            "PRIMARY KEY (portfolio, dataset, id)) WITHOUT ROWID");
    db.exec("INSERT INTO facts VALUES (" + std::to_string(mine) + ", 'name', 'Typed by hand', 0, 1)");
    db.exec("INSERT INTO facts VALUES (" + std::to_string(mine) + ", 'ledger', " + std::to_string(oldUser) + ", 2, 2)");  // a link to a dataset
    db.exec("INSERT INTO facts VALUES (" + std::to_string(oldPts) + ", 'name', 'Points', 0, 3)");                         // a fact of a unit
    db.exec("INSERT INTO facts VALUES (" + std::to_string(nokia) + ", 'name', 'Nokia', 0, 4)");                           // an instrument is as it was
    db.exec("INSERT INTO facts VALUES (" + std::to_string(mine) + ", 'seen', 5, 0, 5)");                                  // a number that is no link stays
    db.exec("INSERT INTO events VALUES (" + std::to_string(mine) + ", " + std::to_string(oldUser) + ", 11, 20000, '" + body + "')");
    db.exec("PRAGMA user_version = 1");
  }
  std::vector<std::string> said;
  auto store = openSqlitePortfolioStore(d.portfolios, [&](const std::string & t) { said.push_back(t); });
  CHECK(d.files(".old-") == 0 && d.files(".bad-") == 0);  // upgraded in place, not moved aside
  bool told = false;
  for (const std::string & line : said) told |= line.find("schema 1 upgraded") != std::string::npos;
  CHECK(told);

  const uint64_t user = datasetKey("USER")->value, hel = marketKey("XHEL")->value, eur = currencyKey("EUR")->value, pts = currencyKey("PTS")->value;
  CHECK(user != oldUser && hel != oldHel && eur != oldEur && pts != oldPts);  // the keys are others now
  std::vector<StoredFact> facts = store->loadAll();
  auto fact = [&](uint64_t node, const std::string & name) -> const StoredFact * {
    for (const StoredFact & f : facts)
      if (f.node == node && f.predicate == name) return &f;
    return nullptr;
  };
  CHECK(facts.size() == 5);
  const StoredFact * typed = fact(mine, "name"), * ledger = fact(mine, "ledger"), * points = fact(pts, "name"), * seen = fact(mine, "seen");
  CHECK(typed && std::get<std::string>(typed->value) == "Typed by hand");
  CHECK(ledger && std::holds_alternative<NodeKey>(ledger->value) && std::get<NodeKey>(ledger->value).value == user);
  CHECK(points && std::get<std::string>(points->value) == "Points" && points->updated == 3);
  CHECK(fact(nokia, "name") != nullptr);
  CHECK(seen && std::holds_alternative<int64_t>(seen->value) && std::get<int64_t>(seen->value) == 5);
  const auto events = store->loadEvents();
  CHECK(events.size() == 1 && events[0].portfolio == mine && events[0].dataset == user && events[0].id == 11 && events[0].time == 20000);
  const auto transaction = portfolio::decodeTransaction(events[0].id, events[0].time, events[0].body);
  CHECK(transaction && transaction->instrument.value == nokia && transaction->market.value == hel && transaction->quote.value == eur);
  CHECK(transaction && transaction->quantity == 10.0 && transaction->price == 4.5 && transaction->fee == 1.0 && transaction->note == "by hand");

  // Done once: the version is 2 and a second open reads it as it is.
  store.reset();
  {
    SQLite::Database db(d.portfolios.string(), SQLite::OPEN_READONLY);
    CHECK(db.execAndGet("PRAGMA user_version").getInt() == 4);
  }
  said.clear();
  auto again = openSqlitePortfolioStore(d.portfolios, [&](const std::string & t) { said.push_back(t); });
  CHECK(said.empty() && again->loadEvents().size() == 1 && again->loadEvents()[0].dataset == user);
}

TEST(a_damaged_file_is_moved_aside_and_nothing_throws) {
  Dir d;
  {
    std::ofstream out(d.portfolios, std::ios::binary);
    out << "this is not a database, it is a letter from my aunt, and it is long enough to be taken for a header of one." << std::string(200, 'x');
  }
  std::vector<std::string> said;
  auto store = openStore(d, &said);
  CHECK(store->snapshot()->portfolios().empty() && d.files(".bad-") == 1);
  store->setAttribute(NodeRef(kMine), {predicate::name, "After"});
  store.reset();
  CHECK(openStore(d)->snapshot()->name(NodeRef(kMine)) == "After");
  bool told = false;
  for (const std::string & line : said) told |= line.find("moved aside") != std::string::npos;
  CHECK(told);
}

TEST(a_file_that_cannot_be_made_keeps_nothing_and_does_not_throw) {
  Dir d;
  fs::create_directories(d.portfolios);  // a directory where the file should be
  std::vector<std::string> said;
  auto store = openStore(d, &said);
  const Things t = makeThings(*store);
  store->setAttribute(NodeRef(kMine), {predicate::name, "In memory"});
  store->appendTransactions({kMine, t.user}, {trade(t, 1, 1, 1, 100)});
  CHECK(store->snapshot()->name(NodeRef(kMine)) == "In memory" && store->snapshot()->transactions({kMine, t.user}).size() == 1);
  CHECK(!said.empty() && fs::is_directory(d.portfolios));
}

TEST(with_no_file_portfolios_live_for_the_session_and_the_cache_never_sees_them) {
  Dir d;
  {
    StoreOptions o;
    o.facts = openSqliteFactStore(d.facts);
    auto store = makeMemoryStore(std::move(o));  // no portfolio file: --no-portfolios
    const Things t = makeThings(*store);
    store->setAttribute(NodeRef(kMine), {predicate::name, "Session"});
    store->appendTransactions({kMine, t.user}, {trade(t, 1, 1, 1, 100)});
    CHECK(store->snapshot()->portfolios() == std::vector<PortfolioId>{kMine});
  }
  for (uint64_t key : keysIn(d.facts, "SELECT node_key FROM facts")) CHECK(NodeKey{key}.kind() != NodeKind::Portfolio);
  CHECK(!fs::exists(d.portfolios));
  StoreOptions o;
  o.facts = openSqliteFactStore(d.facts);
  CHECK(makeMemoryStore(std::move(o))->snapshot()->portfolios().empty());
}

TEST(a_transaction_the_file_holds_that_can_no_longer_be_bought_or_read_is_left_out) {
  Dir d;
  const Things dummy{};
  {
    auto store = openStore(d);
    const Things t = makeThings(*store);
    store->appendTransactions({kMine, t.user}, {trade(t, 1, 1, 1, 100)});
  }
  {
    SQLite::Database db(d.portfolios.string(), SQLite::OPEN_READWRITE);
    const std::string key = std::to_string(kMine.value), dataset = std::to_string(datasetKey(kUserDataset)->value);
    db.exec("INSERT INTO events VALUES (" + key + ", " + dataset + ", 2, 5, 'not json')");
    const std::string noMarket = "{\"side\":\"buy\",\"instrument\":" + std::to_string(instrumentKey("FI0009000681")->value) + ",\"market\":" +
                                 std::to_string(marketKey(kNoMarket)->value) + ",\"quote\":" + std::to_string(currencyKey("EUR")->value) +
                                 ",\"quantity\":1,\"price\":1,\"fee\":0}";
    db.exec("INSERT INTO events VALUES (" + key + ", " + dataset + ", 3, 5, '" + noMarket + "')");
  }
  (void)dummy;
  auto store = openStore(d);
  CHECK(store->snapshot()->transactions({kMine, DatasetId{datasetKey(kUserDataset)->value}}).size() == 1);
}

namespace {
// Nokia quoted in euro on the Helsinki market, and on no market.
GroupKey nokiaOnHel(const Things & t) { return GroupKey{t.hel, t.nokia, t.eur}; }
GroupKey nokiaOnNone(const Things & t) { return GroupKey{t.none, t.nokia, t.eur}; }
}  // namespace

TEST(a_group_is_on_a_list_or_not_whatever_else_the_instrument_is_on_and_a_list_is_made_by_its_first) {
  auto store = makeMemoryStore();
  const Things t = makeThings(*store);
  const GroupKey one = nokiaOnHel(t), two = nokiaOnNone(t);
  CHECK(store->snapshot()->listMembers("watchlist").empty() && !store->snapshot()->onList("watchlist", one));
  const uint64_t before = store->snapshot()->version();
  store->addToList("watchlist", {one});
  auto s = store->snapshot();
  CHECK(s->version() == before + 1 && s->onList("watchlist", one) && !s->onList("watchlist", two));  // that group, not the instrument
  CHECK(!s->onList("holdings", one));  // a list of its own
  store->addToList("holdings", {one, two});
  store->addToList("watchlist", {one});  // there already: nothing changes
  CHECK(store->snapshot()->version() == s->version() + 1 && store->snapshot()->listMembers("holdings").size() == 2);
  CHECK(s->listMembers("holdings").empty());  // a snapshot already taken does not change
  store->removeFromList("watchlist", {two});  // not on it: nothing changes
  const uint64_t same = store->snapshot()->version();
  store->removeFromList("watchlist", {two});
  CHECK(store->snapshot()->version() == same && store->snapshot()->onList("watchlist", one));
  store->removeFromList("watchlist", {one});
  CHECK(store->snapshot()->listMembers("watchlist").empty() && store->snapshot()->listMembers("holdings").size() == 2);
  // A list with no code, and a group with a part that is none, are refused and nothing of the call is written.
  const uint64_t refused_at = store->snapshot()->version();
  bool refused_list = false, refused_edge = false;
  try { store->addToList("", {one}); } catch (const std::invalid_argument &) { refused_list = true; }
  try { store->addToList("watchlist", {one, GroupKey{MarketId{}, t.nokia, t.eur}}); } catch (const std::invalid_argument &) { refused_edge = true; }
  CHECK(refused_list && refused_edge && store->snapshot()->version() == refused_at && store->snapshot()->listMembers("watchlist").empty());
  // A batch is one version.
  store->batch([&](Batch & b) {
    b.addToList("watchlist", {one});
    b.addToList("watchlist", {two});
  });
  CHECK(store->snapshot()->version() == refused_at + 1 && store->snapshot()->listMembers("watchlist").size() == 2);
}

TEST(the_lists_come_back_after_a_restart_with_no_version_and_a_removal_is_kept) {
  Dir d;
  GroupKey one, two;
  {
    auto store = openStore(d);
    const Things t = makeThings(*store);
    one = nokiaOnHel(t);
    two = nokiaOnNone(t);
    store->addToList("watchlist", {one, two});
    store->addToList("holdings", {two});
    store->removeFromList("watchlist", {two});
    store->addToList("watchlist", {two});  // put back after it was taken off, in a later write
    store->removeFromList("watchlist", {one});
  }
  auto store = openStore(d);
  auto s = store->snapshot();
  CHECK(s->version() == 0);
  CHECK(s->listMembers("watchlist") == std::vector<GroupKey>{two});
  CHECK(s->listMembers("holdings") == std::vector<GroupKey>{two});
  CHECK(!s->onList("watchlist", one));
  // They are the user's own: in their file and not in the cache.
  CHECK(keysIn(d.portfolios, "SELECT base FROM list_members").size() == 2);
  SQLite::Database cache(d.facts.string(), SQLite::OPEN_READONLY);
  CHECK(cache.execAndGet("SELECT count(*) FROM sqlite_master WHERE name = 'list_members'").getInt() == 0);
}

TEST(a_list_member_with_a_part_that_is_none_in_the_file_is_not_read) {
  Dir d;
  {
    auto store = openStore(d);
    makeThings(*store);
  }
  {
    auto file = openSqlitePortfolioStore(d.portfolios);
    FactBatch planted;
    planted.members.push_back({"watchlist", 0, 0, 0});
    planted.members.push_back({"", 1, 2, 3});
    file->apply(planted);
  }
  auto store = openStore(d);
  CHECK(store->snapshot()->listMembers("watchlist").empty());
}
