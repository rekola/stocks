#include "TestFramework.h"

#include "model/FactWriter.h"
#include "model/NodeIdentity.h"
#include "model/NodeKey.h"
#include "model/SqliteFactStore.h"
#include "model/Store.h"
#include "model/Isin.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <mutex>
#include <thread>

namespace {

namespace fs = std::filesystem;

// A file in a directory of its own, gone with the object.
struct TempFile {
  fs::path dir, file;
  TempFile() {
    static std::atomic<int> counter{0};
    dir = fs::temp_directory_path() / ("stocks-facts-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                       "-" + std::to_string(counter++));
    fs::create_directories(dir);
    file = dir / "facts.sqlite";
  }
  ~TempFile() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  size_t movedAside() const {
    size_t n = 0;
    for (const auto & e : fs::directory_iterator(dir))
      if (e.path().filename().string().find(".bad-") != std::string::npos) n++;
    return n;
  }
};

// Time as the test says it is.
struct Clock {
  TimestampMs now = 1000;
  std::function<TimestampMs()> fn() {
    return [this] { return now; };
  }
};

StoreOptions optionsFor(const std::shared_ptr<FactStore> & facts, Clock & clock) {
  StoreOptions o;
  o.facts = facts;
  o.clock = clock.fn();
  o.writer.changed_delay = std::chrono::milliseconds(1);
  o.writer.rechecked_delay = std::chrono::milliseconds(1);
  return o;
}

// The facts by (node, predicate).
std::map<std::pair<uint64_t, std::string>, StoredFact> byKey(const std::vector<StoredFact> & all) {
  std::map<std::pair<uint64_t, std::string>, StoredFact> out;
  for (const StoredFact & f : all) out[{f.node, f.predicate}] = f;
  return out;
}

// The number of a key, or zero where there is none (no test wants a missing one to stop it).
uint64_t numberOf(const std::optional<NodeKey> & key) { return key ? key->value : 0; }
std::string nameOf(const std::optional<Dataset> & d) { return d ? d->name : std::string(); }

// One fact of a map, or an empty one (no test wants a missing one to stop it).
StoredFact at(const std::map<std::pair<uint64_t, std::string>, StoredFact> & facts, uint64_t node, const std::string & predicate) {
  const auto it = facts.find({node, predicate});
  return it == facts.end() ? StoredFact{} : it->second;
}

const char * kIsin = "FI0009000681";

// What is said, or nothing (a Value with no alternative set) where it is not.
Value valueOf(const Snapshot & s, NodeRef n, const std::string & predicate) { return s.attribute(n, predicate).value_or(Value{}); }
Attribute factOf(const Snapshot & s, NodeRef n, const std::string & predicate) {
  return s.fact(n, predicate).value_or(Attribute("", ""));
}

}  // namespace

TEST(every_value_alternative_round_trips_through_the_file) {
  TempFile t;
  const uint64_t node = marketKey(0x1234).value;
  const std::vector<Value> values = {std::monostate{},
                                     std::string("Nokia Oyj — ääkköset"),
                                     std::string(),
                                     int64_t{1},
                                     int64_t{std::numeric_limits<int64_t>::min()},
                                     int64_t{std::numeric_limits<int64_t>::max()},
                                     5.0,  // a whole real stays a real
                                     0.1,
                                     -1e300,
                                     std::numeric_limits<double>::infinity(),
                                     true,  // and a truth value is not the integer 1
                                     false};
  {
    auto facts = openSqliteFactStore(t.file);
    FactBatch batch;
    for (size_t i = 0; i < values.size(); i++) batch.changed.push_back({node, "p" + std::to_string(i), values[i], 11 + static_cast<TimestampMs>(i)});
    facts->apply(batch);
  }
  auto loaded = byKey(openSqliteFactStore(t.file)->loadAll());
  CHECK(loaded.size() == values.size());
  for (size_t i = 0; i < values.size(); i++) {
    const StoredFact & f = at(loaded, node, "p" + std::to_string(i));
    CHECK(f.value.index() == values[i].index());
    CHECK(f.value == values[i]);
    CHECK(f.updated == 11 + static_cast<TimestampMs>(i) && NodeKey{f.node}.kind() == NodeKind::Market);
  }
}

TEST(a_batch_replaces_by_node_and_predicate) {
  TempFile t;
  auto facts = openSqliteFactStore(t.file);
  const uint64_t a = numberOf(datasetKey("a")), b = numberOf(datasetKey("b"));
  FactBatch first;
  first.changed = {{a, "name", std::string("one"), 1}, {a, "kind", std::string("x"), 1}, {b, "name", std::string("two"), 1}};
  facts->apply(first);
  FactBatch second;
  second.rechecked = {{a, "name", std::string("uno"), 9}};
  facts->apply(second);
  auto loaded = byKey(facts->loadAll());
  CHECK(loaded.size() == 3 && std::get<std::string>(at(loaded, a, "name").value) == "uno" && at(loaded, a, "name").updated == 9);
  CHECK(std::get<std::string>(at(loaded, b, "name").value) == "two");
}

TEST(a_node_is_what_is_said_of_it_so_a_restart_shows_facts_and_no_edges) {
  TempFile t;
  Clock clock;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    Instrument nokia = store->resolveInstrument({{predicate::isin, kIsin}}, {{predicate::name, "Nokia"}});
    Market hel = store->resolveMarket({{predicate::mic, "XHEL"}, {predicate::name, "Nasdaq Helsinki"}});
    Dataset d = store->addDataset({{}, "providera"});
    store->appendCandles({nokia.id, nokia.id, hel.id, d.id}, Resolution::Day, {{1, 1, 1, 1, 1, 1}});
  }
  auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
  auto s = store->snapshot();
  // The facts are there, found by what they say, and nothing was made of them: no version, no edge, no market with data.
  CHECK(s->version() == 0);
  CHECK(s->nodeCount(NodeType::Instrument) == 1 && s->nodeCount(NodeType::Market) == 1 && s->nodeCount(NodeType::Dataset) == 1);
  const auto nokia = s->findInstrumentByIsin(kIsin);
  const auto hel = s->findMarket("Nasdaq Helsinki");
  CHECK(nokia && nokia->name == "Nokia" && hel && s->findDataset("providera"));
  CHECK(hel && s->edges(hel->id).empty() && s->edgeMarkets().empty());
}

TEST(an_instrument_is_its_isin_and_a_currency_or_unit_its_code) {
  TempFile t;
  Clock clock;
  InstrumentId eur, pts, nokia;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    eur = store->addInstrument({{}, "EUR", "Euro", InstrumentKind::Currency}).id;
    pts = store->addInstrument({{}, "pts", "Index points", InstrumentKind::Unit}).id;
    nokia = store->addInstrument({{}, "NOKIA", "Nokia", InstrumentKind::Company, kIsin}).id;
    store->setAttribute(eur, {"country", "EU"});
  }
  CHECK(eur.value == numberOf(currencyKey("EUR")) && pts.value == numberOf(currencyKey("pts")) && nokia.value == numberOf(instrumentKey(kIsin)));
  std::set<uint64_t> nodes;
  for (const StoredFact & f : openSqliteFactStore(t.file)->loadAll()) nodes.insert(f.node);
  CHECK((nodes == std::set<uint64_t>{eur.value, pts.value, nokia.value}));
}

TEST(an_instrument_with_no_isin_that_is_no_currency_is_given_one_of_ours) {
  Clock clock;
  auto store = makeMemoryStore(optionsFor(nullptr, clock));
  Instrument acme = store->addInstrument({{}, "ACME", "Acme Corporation", InstrumentKind::Company});
  CHECK(acme.id.valid() && acme.isin == privateIsin("ACME") && acme.symbol == "ACME");
  CHECK(acme.id.value == numberOf(instrumentKey(privateIsin("ACME"))));
  CHECK(store->addInstrument({{}, "ACME", "Acme again", InstrumentKind::Company}).id == acme.id);  // the same one: said by its symbol again
  // One with no symbol is made from its name, if that fits.
  Instrument named = store->addInstrument({{}, "", "Zinc", InstrumentKind::Commodity});
  CHECK(named.isin == privateIsin("Zinc") && named.id.valid());
}

TEST(what_cannot_be_given_a_key_is_refused_and_told_of) {
  Clock clock;
  std::vector<std::string> told;
  StoreOptions options = optionsFor(nullptr, clock);
  options.notice = [&](const std::string & text) { told.push_back(text); };
  auto store = makeMemoryStore(std::move(options));
  CHECK(!store->addInstrument({{}, "NOKIA", "Nokia", InstrumentKind::Company, "FI0009000680"}).id.valid());  // the check digit is wrong
  CHECK(!store->addInstrument({{}, "", "A name that is much too long for a private ISIN", InstrumentKind::Other}).id.valid());
  CHECK(!store->addInstrument({{}, "A CURRENCY CODE", "Too long", InstrumentKind::Currency}).id.valid());
  CHECK(!store->addDataset({{}, "provider-a"}).id.valid());  // a code is letters and digits
  CHECK(!store->addDataset({{}, "morethanten1"}).id.valid());
  CHECK(told.size() == 5);
  CHECK(store->snapshot()->nodeCount(NodeType::Instrument) == 0 && store->snapshot()->nodeCount(NodeType::Dataset) == 0);
}

TEST(a_dataset_is_its_code_and_its_name_is_a_fact_that_is_the_code_until_said) {
  Clock clock;
  auto store = makeMemoryStore(optionsFor(nullptr, clock));
  Dataset d = store->addDataset({{}, "nordnet"});
  CHECK(d.id.valid() && d.code == "nordnet" && d.name == "nordnet" && d.id.value == numberOf(datasetKey("nordnet")));
  store->addDataset({{}, "nordnet", "Nordnet"});
  CHECK(nameOf(store->snapshot()->dataset(d.id)) == "Nordnet");
  store->addDataset({{}, "nordnet"});  // said again with no name: the name that was said stays
  CHECK(nameOf(store->snapshot()->dataset(d.id)) == "Nordnet" && nameOf(store->snapshot()->findDataset("nordnet")) == "Nordnet");
}

TEST(a_file_of_the_first_schema_is_converted_to_keys) {
  TempFile t;
  {
    SQLite::Database db(t.file.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec(
        "CREATE TABLE facts (node_type INTEGER NOT NULL, node_key TEXT NOT NULL, predicate TEXT NOT NULL, value, is_bool INTEGER NOT NULL DEFAULT 0, "
        "updated INTEGER NOT NULL, checked INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (node_type, node_key, predicate)) WITHOUT ROWID");
    db.exec("INSERT INTO facts VALUES (0, 'FI0009000681', 'name', 'Nokia', 0, 100, 0)");
    db.exec("INSERT INTO facts VALUES (0, 'FI0009000681', 'halal', 1, 1, 101, 0)");
    db.exec("INSERT INTO facts VALUES (0, 'FI0009000680', 'name', 'a wrong ISIN', 0, 102, 0)");
    db.exec("INSERT INTO facts VALUES (3, 'EUR', 'name', 'Euro', 0, 103, 0)");
    db.exec("INSERT INTO facts VALUES (2, 'nordnet', 'name', 'Nordnet', 0, 104, 0)");
    db.exec("INSERT INTO facts VALUES (2, 'not a code', 'name', 'x', 0, 105, 0)");
    db.exec("INSERT INTO facts VALUES (1, '00000000000abcde', 'country', 'FI', 0, 106, 0)");
    db.exec("INSERT INTO facts VALUES (1, 'nonsense', 'country', 'SE', 0, 107, 0)");
    db.exec("INSERT INTO facts VALUES (0, 'symbol:OLD', 'name', 'older still', 0, 108, 0)");
    db.exec("PRAGMA user_version = 1");
  }
  auto loaded = byKey(openSqliteFactStore(t.file)->loadAll());
  CHECK(loaded.size() == 5);  // what has no key is left out
  const uint64_t nokia = numberOf(instrumentKey("FI0009000681"));
  CHECK(std::get<std::string>(at(loaded, nokia, "name").value) == "Nokia" && at(loaded, nokia, "name").updated == 100);
  CHECK(std::get<bool>(at(loaded, nokia, "halal").value) == true);
  CHECK(std::get<std::string>(at(loaded, numberOf(currencyKey("EUR")), "name").value) == "Euro");
  CHECK(std::get<std::string>(at(loaded, numberOf(datasetKey("nordnet")), "name").value) == "Nordnet");
  CHECK(std::get<std::string>(at(loaded, marketKey(0xabcde).value, "country").value) == "FI");
  // It is a file of the new schema now: opened again it is not converted again, and it takes new facts.
  auto facts = openSqliteFactStore(t.file);
  FactBatch b;
  b.changed = {{nokia, "name", std::string("Nokia Oyj"), 200}};
  facts->apply(b);
  CHECK(byKey(facts->loadAll()).size() == 5 && t.movedAside() == 0);
}

TEST(an_unchanged_write_moves_updated_and_makes_no_version) {
  Clock clock;
  auto store = makeMemoryStore(optionsFor(nullptr, clock));
  Instrument nokia = store->addInstrument({{}, "NOKIA", "Nokia", InstrumentKind::Company, kIsin});
  store->setAttribute(nokia.id, {predicate::iconUrl, "http://icons/nokia.png"});
  const uint64_t version = store->snapshot()->version();
  auto first = store->snapshot()->fact(nokia.id, predicate::iconUrl);
  CHECK(first && first->updated == 1000);

  clock.now = 2000;
  store->setAttribute(nokia.id, {predicate::iconUrl, "http://icons/nokia.png"});
  auto again = store->snapshot()->fact(nokia.id, predicate::iconUrl);
  CHECK(store->snapshot()->version() == version);
  CHECK(again && again->updated == 2000);  // said again: stored again, whatever the value
  CHECK(std::get<std::string>(valueOf(*store->snapshot(), nokia.id, predicate::iconUrl)) == "http://icons/nokia.png");

  // The times are on what attributes() returns too, and do not take part in equality.
  bool found = false;
  for (const Attribute & a : store->snapshot()->attributes(nokia.id))
    if (a.predicate == predicate::iconUrl) found = a.updated == 2000;
  CHECK(found);
  Attribute x(predicate::iconUrl, "x"), y = x;
  y.updated = 5;
  CHECK(x == y);

  clock.now = 3000;
  store->setAttribute(nokia.id, {predicate::iconUrl, "http://icons/new.png"});
  auto changed = store->snapshot()->fact(nokia.id, predicate::iconUrl);
  CHECK(store->snapshot()->version() == version + 1);
  CHECK(changed && changed->updated == 3000);
}

TEST(a_value_nobody_can_say_is_not_said_or_kept) {
  TempFile t;
  Clock clock;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    Instrument nokia = store->addInstrument({{}, "NOKIA", "Nokia", InstrumentKind::Company, kIsin});
    store->setAttribute(nokia.id, {"shares", std::nan("")});
    CHECK(!store->snapshot()->fact(nokia.id, "shares"));
  }
  for (const StoredFact & f : openSqliteFactStore(t.file)->loadAll()) CHECK(f.predicate != "shares");
}

TEST(a_restart_shows_what_was_said_and_when_it_was_last_stored) {
  TempFile t;
  Clock clock;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    Instrument nokia = store->addInstrument({{}, "NOKIA", "Nokia", InstrumentKind::Company, kIsin});
    store->setAttributes(nokia.id, {{"shares", int64_t{5'600'000'000}}, {"free-float", 0.75}, {"halal", true}});
    Market hel = store->resolveMarket({{predicate::mic, "XHEL"}, {predicate::name, "Nasdaq Helsinki"}});
    store->setAttribute(hel.id, {predicate::country, "FI"});
    clock.now = 1500;
    store->setAttribute(nokia.id, {"shares", int64_t{5'600'000'000}});  // said again: only updated moves, and is kept without a version
  }  // the store's end writes everything

  clock.now = 9000;
  auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
  Instrument nokia = store->addInstrument({{}, "NOKIA", "Nokia", InstrumentKind::Company, kIsin});
  Attribute shares = factOf(*store->snapshot(), nokia.id, "shares");
  CHECK(std::get<int64_t>(shares.value) == 5'600'000'000 && shares.updated == 1500);
  CHECK(std::get<double>(valueOf(*store->snapshot(), nokia.id, "free-float")) == 0.75);
  CHECK(std::get<bool>(valueOf(*store->snapshot(), nokia.id, "halal")) == true);

  // Said again after a restart: stored again, and no new version, since the value is what was kept.
  const uint64_t version = store->snapshot()->version();
  store->setAttribute(nokia.id, {"shares", int64_t{5'600'000'000}});
  CHECK(store->snapshot()->version() == version);
  shares = factOf(*store->snapshot(), nokia.id, "shares");
  CHECK(shares.updated == 9000);
  store->setAttribute(nokia.id, {"shares", int64_t{5'500'000'000}});
  shares = factOf(*store->snapshot(), nokia.id, "shares");
  CHECK(shares.updated == 9000);

  Market hel = store->resolveMarket({{predicate::name, "Nasdaq Helsinki"}, {predicate::mic, "XHEL"}});
  CHECK(std::get<std::string>(valueOf(*store->snapshot(), hel.id, predicate::country)) == "FI");
}

TEST(facts_said_again_after_a_restart_are_in_the_file_when_the_store_ends) {
  TempFile t;
  Clock clock;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    store->resolveInstrument({{predicate::isin, kIsin}}, {{predicate::name, "Nokia"}});
  }
  clock.now = 9000;
  {
    StoreOptions options = optionsFor(openSqliteFactStore(t.file), clock);
    options.writer.rechecked_delay = std::chrono::minutes(10);  // not due: only the end of the store writes them
    auto store = makeMemoryStore(std::move(options));
    store->resolveInstrument({{predicate::isin, kIsin}}, {{predicate::name, "Nokia"}});
    CHECK(store->snapshot()->version() == 0);
  }
  for (const StoredFact & f : openSqliteFactStore(t.file)->loadAll()) CHECK(f.updated == 9000);
}

TEST(what_another_source_does_not_say_survives_its_load) {
  TempFile t;
  Clock clock;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    store->resolveInstrument({{predicate::isin, kIsin}}, {{predicate::name, "Nokia"}, {"shares", int64_t{100}}, {predicate::iconUrl, "a.png"}});
  }
  auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
  // A source that knows names and a type, and nothing of shares or icons.
  Instrument i = store->resolveInstrument({{predicate::isin, kIsin}}, {{predicate::name, "Nokia Oyj"}, {predicate::type, "EQUITY"}});
  auto s = store->snapshot();
  CHECK(i.name == "Nokia Oyj" && i.type == "EQUITY" && i.isin == kIsin);
  CHECK(std::get<int64_t>(valueOf(*s, i.id, "shares")) == 100);
  CHECK(std::get<std::string>(valueOf(*s, i.id, predicate::iconUrl)) == "a.png");
  CHECK(s->findByAttribute(NodeType::Instrument, predicate::iconUrl, "a.png").value_or(NodeRef{}).id == i.id.value);  // and found by it
}

TEST(a_market_is_kept_under_a_key_of_its_own_and_its_mic_is_a_fact) {
  TempFile t;
  Clock clock;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    Market hel = store->resolveMarket({{predicate::name, "Nasdaq Helsinki"}});  // known by its name first
    store->setAttribute(hel.id, {predicate::mic, "XHEL"});                      // its MIC learned later
    Market other = store->resolveMarket({{predicate::nordnetId, "7"}});         // one with no MIC and no name
    store->setAttribute(other.id, {predicate::country, "SE"});
  }
  std::set<uint64_t> keys;
  std::map<uint64_t, Value> mic;
  for (const StoredFact & f : openSqliteFactStore(t.file)->loadAll()) {
    CHECK(NodeKey{f.node}.kind() == NodeKind::Market);  // a key of a market says nothing of what the market is
    keys.insert(f.node);
    if (f.predicate == predicate::mic) mic[f.node] = f.value;
  }
  CHECK(keys.size() == 2);  // the market learned its MIC later kept its key, and is one
  CHECK(mic.size() == 1 && std::get<std::string>(mic.begin()->second) == "XHEL");
}

TEST(a_market_is_found_again_by_any_of_what_it_was_said_to_be) {
  TempFile t;
  Clock clock;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    Market hel = store->resolveMarket({{predicate::nordnetId, "4"}, {predicate::mic, "XHEL"}, {predicate::name, "Nasdaq Helsinki"}});
    store->setAttribute(hel.id, {predicate::country, "FI"});
    Market nameless = store->resolveMarket({{predicate::nordnetId, "9"}});  // no MIC, no name
    store->setAttribute(nameless.id, {predicate::country, "NO"});
    Market other = store->resolveMarket({{predicate::name, "Nasdaq Stockholm"}});
    store->setAttribute(other.id, {predicate::country, "SE"});
  }
  for (auto said : std::vector<std::vector<Attribute>>{{{predicate::mic, "XHEL"}}, {{predicate::nordnetId, "4"}}, {{predicate::name, "Nasdaq Helsinki"}}}) {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    Market m = store->resolveMarket(said);
    CHECK(std::get<std::string>(valueOf(*store->snapshot(), m.id, predicate::country)) == "FI");
    CHECK(std::get<std::string>(valueOf(*store->snapshot(), m.id, predicate::mic)) == "XHEL");
    CHECK(std::get<std::string>(valueOf(*store->snapshot(), m.id, predicate::name)) == "Nasdaq Helsinki");
  }
  // Markets with no MIC are as good: the id at a source finds the one, the name the other, and neither takes the other's facts.
  auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
  Market nameless = store->resolveMarket({{predicate::nordnetId, "9"}});
  Market sto = store->resolveMarket({{predicate::name, "Nasdaq Stockholm"}});
  CHECK(nameless.id.value != sto.id.value);
  CHECK(std::get<std::string>(valueOf(*store->snapshot(), nameless.id, predicate::country)) == "NO");
  CHECK(std::get<std::string>(valueOf(*store->snapshot(), sto.id, predicate::country)) == "SE");
  // One not seen before is a new market with nothing held for it.
  Market unknown = store->resolveMarket({{predicate::mic, "XNYS"}});
  CHECK(!store->snapshot()->fact(unknown.id, predicate::country));
}

TEST(two_markets_do_not_have_the_same_mic_or_the_same_id_at_a_source) {
  Clock clock;
  std::vector<std::string> told;
  StoreOptions options = optionsFor(nullptr, clock);
  options.notice = [&](const std::string & text) { told.push_back(text); };
  auto store = makeMemoryStore(std::move(options));
  Market a = store->resolveMarket({{predicate::name, "Nasdaq Helsinki"}, {predicate::mic, "XHEL"}, {predicate::nordnetId, "4"}});
  Market b = store->resolveMarket({{predicate::name, "Nasdaq Stockholm"}});
  const uint64_t version = store->snapshot()->version();

  store->setAttribute(b.id, {predicate::mic, "XHEL"});  // a's
  store->setAttribute(b.id, {predicate::nordnetId, "4"});
  CHECK(!store->snapshot()->fact(b.id, predicate::mic) && !store->snapshot()->fact(b.id, predicate::nordnetId));
  CHECK(std::get<std::string>(valueOf(*store->snapshot(), a.id, predicate::mic)) == "XHEL");  // the first keeps it
  CHECK(store->snapshot()->version() == version);
  CHECK(told.size() == 2 && told[0].find("mic XHEL") != std::string::npos && told[1].find("nordnet-id 4") != std::string::npos);
  store->setAttribute(b.id, {predicate::mic, "XHEL"});
  CHECK(told.size() == 2);  // each is told once

  // What a market says of itself again is no conflict, and another one's own value is no conflict either.
  store->setAttribute(a.id, {predicate::mic, "XHEL"});
  store->setAttribute(b.id, {predicate::mic, "XSTO"});
  CHECK(told.size() == 2 && std::get<std::string>(valueOf(*store->snapshot(), b.id, predicate::mic)) == "XSTO");
  // The one in the others' names is no unique: two markets may have one name said (it says nothing of which is meant).
  store->setAttribute(b.id, {predicate::name, "Nasdaq Helsinki"});
  CHECK(told.size() == 2 && std::get<std::string>(valueOf(*store->snapshot(), b.id, predicate::name)) == "Nasdaq Helsinki");

  // A resolve that says two markets are one (the MIC of the one, the id at a source of the other) finds the first, and the
  // second's id is not taken from it.
  Market c = store->resolveMarket({{predicate::mic, "XNYS"}, {predicate::nordnetId, "11"}});
  Market d = store->resolveMarket({{predicate::mic, "XNAS"}});
  Market found = store->resolveMarket({{predicate::mic, "XNAS"}, {predicate::nordnetId, "11"}});
  CHECK(found.id == d.id && c.id.value != d.id.value);
  CHECK(!store->snapshot()->fact(d.id, predicate::nordnetId));
  CHECK(std::get<std::string>(valueOf(*store->snapshot(), c.id, predicate::nordnetId)) == "11");
  CHECK(told.size() == 3);
}

TEST(a_mic_that_a_market_of_the_file_has_is_not_given_to_another) {
  TempFile t;
  Clock clock;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    store->resolveMarket({{predicate::mic, "XHEL"}, {predicate::name, "Nasdaq Helsinki"}});
  }
  std::vector<std::string> told;
  StoreOptions options = optionsFor(openSqliteFactStore(t.file), clock);
  options.notice = [&](const std::string & text) { told.push_back(text); };
  auto store = makeMemoryStore(std::move(options));
  Market other = store->resolveMarket({{predicate::name, "Somewhere else"}});  // not the held one
  store->setAttribute(other.id, {predicate::mic, "XHEL"});
  CHECK(!store->snapshot()->fact(other.id, predicate::mic) && told.size() == 1);
  // Said first of the market that is meant, it is that market: the held one is found by its MIC.
  Market hel = store->resolveMarket({{predicate::mic, "XHEL"}});
  CHECK(hel.name == "Nasdaq Helsinki" && hel.id.value != other.id.value && told.size() == 1);
}

TEST(a_file_that_gives_two_markets_the_same_mic_is_told_of) {
  TempFile t;
  Clock clock;
  {
    auto facts = openSqliteFactStore(t.file);
    FactBatch b;
    b.changed = {{marketKey(1).value, predicate::mic, std::string("XHEL"), 1}, {marketKey(2).value, predicate::mic, std::string("XHEL"), 2}};
    facts->apply(b);
  }
  std::vector<std::string> told;
  StoreOptions options = optionsFor(openSqliteFactStore(t.file), clock);
  options.notice = [&](const std::string & text) { told.push_back(text); };
  auto store = makeMemoryStore(std::move(options));
  CHECK(told.size() == 1 && told[0].find("mic XHEL") != std::string::npos && told[0].find("2 markets") != std::string::npos);
  CHECK(store->resolveMarket({{predicate::mic, "XHEL"}}).id.valid());  // and goes on: the lowest key is the market
}

TEST(a_file_that_is_no_database_is_moved_aside_and_replaced) {
  TempFile t;
  {
    std::ofstream out(t.file, std::ios::binary);
    out << "this is not a database, it is a long enough sentence to look like a damaged page of one.........................";
  }
  std::vector<std::string> said;
  auto facts = openSqliteFactStore(t.file, [&](const std::string & s) { said.push_back(s); });
  CHECK(said.size() == 1 && t.movedAside() == 1);
  CHECK(facts->loadAll().empty());
  FactBatch b;
  b.changed = {{numberOf(datasetKey("x")), "name", std::string("x"), 1}};
  facts->apply(b);
  CHECK(facts->loadAll().size() == 1);
}

TEST(a_file_of_a_newer_schema_is_moved_aside_and_replaced) {
  TempFile t;
  {
    auto facts = openSqliteFactStore(t.file);
    FactBatch b;
    b.changed = {{numberOf(datasetKey("x")), "name", std::string("x"), 1}};
    facts->apply(b);
  }  // closed: the log is folded into the file
  {
    // `user_version` is four bytes, big-endian, at offset 60 of the header.
    std::fstream f(t.file, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(60);
    const char newer[4] = {0, 0, 0, 99};
    f.write(newer, 4);
  }
  std::vector<std::string> said;
  auto facts = openSqliteFactStore(t.file, [&](const std::string & s) { said.push_back(s); });
  CHECK(said.size() == 1 && said[0].find("newer") != std::string::npos);
  CHECK(t.movedAside() >= 1 && facts->loadAll().empty());
}

TEST(a_file_that_cannot_be_made_keeps_nothing_and_does_not_throw) {
  TempFile t;
  std::ofstream(t.dir / "in-the-way") << "a file where a directory should be";
  std::vector<std::string> said;
  auto facts = openSqliteFactStore(t.dir / "in-the-way" / "facts.sqlite", [&](const std::string & s) { said.push_back(s); });
  FactBatch b;
  b.changed = {{numberOf(datasetKey("x")), "name", std::string("x"), 1}};
  facts->apply(b);
  CHECK(facts->loadAll().empty() && !said.empty());
}

namespace {

// A FactStore that counts what it is given.
struct CountingFacts : FactStore {
  std::mutex mutex;
  int applies = 0;
  std::vector<FactBatch> batches;
  std::vector<StoredFact> loadAll() override { return {}; }
  void apply(const FactBatch & b) override {
    std::lock_guard lock(mutex);
    applies++;
    batches.push_back(b);
  }
};

StoredFact fact(uint64_t node, const std::string & predicate, int64_t value, TimestampMs updated) {
  return {node, predicate, value, updated};
}

}  // namespace

TEST(facts_said_again_are_written_together_and_not_one_by_one) {
  auto counting = std::make_shared<CountingFacts>();
  {
    FactWriter::Options o;
    o.changed_delay = std::chrono::milliseconds(1);
    o.rechecked_delay = std::chrono::minutes(10);
    FactWriter writer(counting, o);
    for (int round = 0; round < 50; round++)
      for (int n = 0; n < 100; n++) {
        FactBatch b;
        b.rechecked = {fact(static_cast<uint64_t>(n) + 1, "shares", 5, round)};
        writer.post(std::move(b));
      }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(counting->applies == 0);  // nothing is in a hurry, and nothing blocked
    writer.flush();
    CHECK(counting->applies == 1 && counting->batches[0].changed.size() == 100);  // one fact per key, the last said
    for (const StoredFact & f : counting->batches[0].changed) CHECK(f.updated == 49);
  }
  CHECK(counting->applies == 1);
}

TEST(a_change_is_written_soon_and_what_waits_goes_with_it) {
  auto counting = std::make_shared<CountingFacts>();
  FactWriter::Options o;
  o.changed_delay = std::chrono::milliseconds(5);
  o.rechecked_delay = std::chrono::minutes(10);
  FactWriter writer(counting, o);
  FactBatch quiet;
  quiet.rechecked = {fact(1, "shares", 5, 1)};
  writer.post(std::move(quiet));
  FactBatch loud;
  loud.changed = {fact(2, "shares", 6, 2)};
  writer.post(std::move(loud));
  for (int i = 0; i < 400; i++) {
    {
      std::lock_guard lock(counting->mutex);
      if (counting->applies > 0) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::lock_guard lock(counting->mutex);
  CHECK(counting->applies == 1 && counting->batches[0].changed.size() == 2);
}

