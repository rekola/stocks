#include "TestFramework.h"

#include "model/FactWriter.h"
#include "model/NodeIdentity.h"
#include "model/SqliteFactStore.h"
#include "model/Store.h"

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

std::map<std::string, StoredFact> byPredicate(const std::vector<StoredFact> & all) {
  std::map<std::string, StoredFact> out;
  for (const StoredFact & f : all) out[f.node + "/" + f.predicate] = f;
  return out;
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
    for (size_t i = 0; i < values.size(); i++)
      batch.changed.push_back({NodeType::Market, "mic:XHEL", "p" + std::to_string(i), values[i], 11 + static_cast<TimestampMs>(i)});
    facts->apply(batch);
  }
  auto loaded = byPredicate(openSqliteFactStore(t.file)->loadAll());
  CHECK(loaded.size() == values.size());
  for (size_t i = 0; i < values.size(); i++) {
    const StoredFact & f = loaded["mic:XHEL/p" + std::to_string(i)];
    CHECK(f.value.index() == values[i].index());
    CHECK(f.value == values[i]);
    CHECK(f.updated == 11 + static_cast<TimestampMs>(i) && f.type == NodeType::Market);
  }
}

TEST(a_batch_replaces_by_key_and_forgets_whole_nodes) {
  TempFile t;
  auto facts = openSqliteFactStore(t.file);
  FactBatch a;
  a.changed = {{NodeType::Instrument, "isin:A", "name", std::string("one"), 1},
               {NodeType::Instrument, "isin:A", "kind", std::string("company"), 1},
               {NodeType::Instrument, "isin:B", "name", std::string("two"), 1}};
  facts->apply(a);
  FactBatch b;
  b.rechecked = {{NodeType::Instrument, "isin:A", "name", std::string("uno"), 9}};
  b.forget = {{NodeType::Instrument, "isin:B"}};
  facts->apply(b);
  auto loaded = byPredicate(facts->loadAll());
  CHECK(loaded.size() == 2 && std::get<std::string>(loaded["isin:A/name"].value) == "uno" && loaded["isin:A/name"].updated == 9);
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

TEST(a_node_with_no_edges_is_not_seen_after_a_restore) {
  TempFile t;
  Clock clock;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    Instrument nokia = store->resolveInstrument({{predicate::isin, kIsin}}, {{predicate::name, "Nokia"}});
    Market hel = store->resolveMarket({{predicate::mic, "XHEL"}, {predicate::name, "Nasdaq Helsinki"}});
    Dataset d = store->addDataset({{}, "provider-a"});
    store->appendCandles({nokia.id, nokia.id, hel.id, d.id}, Resolution::Day, {{1, 1, 1, 1, 1, 1}});
  }
  auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
  auto s = store->snapshot();
  CHECK(s->instruments().size() == 0 && s->markets().size() == 0 && s->datasets().size() == 0);
  CHECK(!s->findInstrumentByIsin(kIsin) && !s->findMarket("Nasdaq Helsinki") && !s->findDataset("provider-a"));
  CHECK(s->version() == 0);

  // Said again, the node is made and has its facts; with no edges it is still not shown anywhere.
  Instrument nokia = store->resolveInstrument({{predicate::isin, kIsin}}, {});
  Market hel = store->resolveMarket({{predicate::mic, "XHEL"}});
  s = store->snapshot();
  CHECK(nokia.name == "Nokia" && hel.name == "Nasdaq Helsinki");
  CHECK(s->edges(hel.id).empty());
}

TEST(facts_follow_a_node_to_a_stronger_identity) {
  TempFile t;
  Clock clock;
  {
    auto store = makeMemoryStore(optionsFor(openSqliteFactStore(t.file), clock));
    Instrument i = store->addInstrument({{}, "ACME", "Acme", InstrumentKind::Company});  // known by its symbol
    store->setAttribute(i.id, {"shares", int64_t{7}});
    store->setAttribute(i.id, {predicate::isin, "SE0000000001"});  // and then by its ISIN
  }
  auto loaded = byPredicate(openSqliteFactStore(t.file)->loadAll());
  CHECK(loaded.count("isin:SE0000000001/shares") == 1 && loaded.count("isin:SE0000000001/symbol") == 1);
  for (const auto & [key, f] : loaded) CHECK(f.node != "symbol:ACME");
}

TEST(what_identifies_a_node) {
  CHECK(nodeKey(NodeType::Instrument, {{predicate::symbol, "EUR"}}) == "symbol:EUR");
  CHECK(nodeKey(NodeType::Instrument, {{predicate::symbol, "ACME"}, {predicate::isin, kIsin}}) == std::string("isin:") + kIsin);
  CHECK(nodeKey(NodeType::Dataset, {{predicate::name, "provider-a"}}) == "name:provider-a");
  CHECK(nodeKey(NodeType::Dataset, {{"other", "x"}}).empty());
  // A market has no key of what it is said to be: it is recognised by it.
  CHECK(nodeKey(NodeType::Market, {{predicate::mic, "XHEL"}}).empty());
  const auto by = recognisedBy(NodeType::Market, {{predicate::name, "Nasdaq Helsinki"}, {predicate::nordnetId, "4"}, {predicate::mic, "XHEL"}});
  CHECK(by.size() == 3 && by[0].first == predicate::mic && by[1].first == predicate::nordnetId && by[2].first == predicate::name);
  CHECK(recognisedBy(NodeType::Market, {{predicate::country, "FI"}}).empty() && recognisedBy(NodeType::Instrument, {{predicate::mic, "X"}}).empty());
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
  std::set<std::string> keys;
  std::map<std::string, Value> mic;
  for (const StoredFact & f : openSqliteFactStore(t.file)->loadAll()) {
    CHECK(f.type == NodeType::Market);
    keys.insert(f.node);
    CHECK(f.node.find("XHEL") == std::string::npos && f.node.find("Helsinki") == std::string::npos);  // no fact's value is in a key
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
  b.changed = {{NodeType::Dataset, "name:x", "name", std::string("x"), 1}};
  facts->apply(b);
  CHECK(facts->loadAll().size() == 1);
}

TEST(a_file_of_a_newer_schema_is_moved_aside_and_replaced) {
  TempFile t;
  {
    auto facts = openSqliteFactStore(t.file);
    FactBatch b;
    b.changed = {{NodeType::Dataset, "name:x", "name", std::string("x"), 1}};
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
  b.changed = {{NodeType::Dataset, "name:x", "name", std::string("x"), 1}};
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

StoredFact fact(const std::string & node, const std::string & predicate, int64_t value, TimestampMs updated) {
  return {NodeType::Instrument, node, predicate, value, updated};
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
        b.rechecked = {fact("isin:" + std::to_string(n), "shares", 5, round)};
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
  quiet.rechecked = {fact("isin:1", "shares", 5, 1)};
  writer.post(std::move(quiet));
  FactBatch loud;
  loud.changed = {fact("isin:2", "shares", 6, 2)};
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

TEST(a_node_forgotten_takes_its_waiting_facts_with_it) {
  auto counting = std::make_shared<CountingFacts>();
  {
    FactWriter::Options o;
    o.changed_delay = std::chrono::minutes(10);
    o.rechecked_delay = std::chrono::minutes(10);
    FactWriter writer(counting, o);
    FactBatch a;
    a.changed = {fact("symbol:A", "shares", 1, 1)};
    writer.post(std::move(a));
    FactBatch b;
    b.forget = {{NodeType::Instrument, "symbol:A"}};
    b.changed = {fact("isin:A", "shares", 1, 1)};
    writer.post(std::move(b));
  }  // the end writes what waits
  CHECK(counting->applies == 1);
  CHECK(counting->batches[0].forget.size() == 1 && counting->batches[0].changed.size() == 1 && counting->batches[0].changed[0].node == "isin:A");
}
