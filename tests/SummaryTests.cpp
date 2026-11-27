#include "TestFramework.h"
#include "TestIsin.h"

#include "model/Dates.h"
#include "model/NodeKey.h"
#include "model/Period.h"
#include "model/SqliteSummaryStore.h"
#include "model/Store.h"
#include "model/SummaryRecords.h"
#include "model/SummaryWriter.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>

namespace {

namespace fs = std::filesystem;

struct TempDir {
  fs::path dir;
  TempDir() {
    static std::atomic<int> counter{0};
    dir = fs::temp_directory_path() / ("stocks-summary-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~TempDir() {
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

struct Messages {
  std::mutex mutex;
  std::vector<std::string> lines;
  FactLog log() {
    return [this](const std::string & text) {
      std::lock_guard lock(mutex);
      lines.push_back(text);
    };
  }
  bool said(const std::string & part) {
    std::lock_guard lock(mutex);
    for (const std::string & l : lines)
      if (l.find(part) != std::string::npos) return true;
    return false;
  }
};

EdgeKey edgeOf(int n, const char * dataset = "nordnet") {
  return EdgeKey{InstrumentId{instrumentKey(testIsin(n))->value}, InstrumentId{currencyKey("EUR")->value}, MarketId{marketKey("XHEL")->value},
                 DatasetId{datasetKey(dataset)->value}};
}

Candle bar(int64_t time, double open, double high, double low, double close, double volume) { return Candle{time, open, high, low, close, volume}; }

StoredEdge storedEdge(const EdgeKey & k, double last) {
  StoredEdge e;
  e.listing = {k, "NOKIA", "ident", "book-1", "STOCK", "Nokia Oyj"};
  e.quote.last = last;
  e.quote.bid = last - 0.01;
  e.quote.ask = last + 0.01;
  e.quote.time = 1'700'000'000;
  return e;
}

StoredQueue storedQueue(const EdgeKey & k, Resolution r, std::vector<Candle> bars, bool merge = false, int64_t keep = 0) {
  StoredQueue q;
  q.edge = k;
  q.resolution = r;
  q.fetched = 42;
  q.merge = merge;
  q.keep = keep;
  q.bars = std::move(bars);
  return q;
}

bool sameBars(const std::vector<Candle> & a, const std::vector<Candle> & b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (!summary::sameBar(a[i], b[i])) return false;
  return true;
}

}  // namespace

// ---- The records

TEST(a_candle_record_is_28_bytes_and_reads_back_as_the_bar_it_was_stored_from) {
  const Candle c = bar(5'000'000, 100.25, 101.5, 99.75, 100.5, 1234);
  const std::string blob = summary::pack(QueueKind::Candle, {c});
  CHECK(blob.size() == 2 + 28);
  CHECK(static_cast<uint8_t>(blob[0]) == summary::kFormat);
  CHECK(static_cast<uint8_t>(blob[1]) == 0);
  // Little-endian, whatever the machine: the slot first.
  CHECK(static_cast<uint8_t>(blob[2]) == (5'000'000 & 0xff) && static_cast<uint8_t>(blob[3]) == ((5'000'000 >> 8) & 0xff));
  QueueKind kind = QueueKind::Scalar;
  std::vector<Candle> back;
  CHECK(summary::unpack(blob, kind, back));
  CHECK(kind == QueueKind::Candle);
  CHECK(back.size() == 1);
  CHECK(back[0].time == 5'000'000);
  CHECK(back[0].open == 100.25);
  CHECK(back[0].high == 101.5 && back[0].low == 99.75 && back[0].close == 100.5);  // these deltas are exact in a float
  CHECK(back[0].volume == 1234);
  // What is read back is what `normalized` says, and that is the stored form.
  CHECK(summary::sameBar(back[0], summary::normalized(QueueKind::Candle, c)));
  CHECK(summary::sameBar(summary::normalized(QueueKind::Candle, back[0]), back[0]));
}

TEST(a_bar_with_no_open_reads_back_with_no_open_and_keeps_its_high_low_and_volume) {
  const Candle noOpen = bar(7'000, kUndefined, 12.5, 9.25, 10.5, 300);
  const Candle onlyClose = bar(7'001, kUndefined, kUndefined, kUndefined, 11, kUndefined);
  const Candle withOpen = bar(7'002, 10, 12, 9, 11, 5);
  const std::string blob = summary::pack(QueueKind::Candle, {noOpen, onlyClose, withOpen});
  CHECK(blob.size() == 2 + 3 * 28 && static_cast<uint8_t>(blob[0]) == 2);  // the record format after this change
  QueueKind kind = QueueKind::Scalar;
  std::vector<Candle> back;
  CHECK(summary::unpack(blob, kind, back) && back.size() == 3);
  if (back.size() != 3) return;
  CHECK(!isDefined(back[0].open) && back[0].high == 12.5 && back[0].low == 9.25 && back[0].close == 10.5 && back[0].volume == 300);
  CHECK(!isDefined(back[1].open) && !isDefined(back[1].high) && !isDefined(back[1].low) && back[1].close == 11 && !isDefined(back[1].volume));
  CHECK(back[2].open == 10 && back[2].high == 12 && back[2].low == 9 && back[2].close == 11);
  // normalized is the stored form, and the open stays undefined in it; a bar that has an open is not the same bar as one that has not
  for (const Candle & c : {noOpen, onlyClose}) CHECK(!isDefined(summary::normalized(QueueKind::Candle, c).open));
  CHECK(!summary::sameBar(summary::normalized(QueueKind::Candle, bar(7'000, 10.5, 12.5, 9.25, 10.5, 300)), back[0]));
  CHECK(summary::hashOf(QueueKind::Candle, bar(7'000, 10.5, 12.5, 9.25, 10.5, 300)) != summary::hashOf(QueueKind::Candle, noOpen));
  // the same bar again is the same bar
  CHECK(summary::sameBar(back[0], summary::normalized(QueueKind::Candle, back[0])));
}

TEST(a_value_record_is_12_bytes_and_reads_back_as_a_bar_of_one_value) {
  const std::string blob = summary::pack(QueueKind::Scalar, {bar(20'000, 1, 5, 0, 123.456, 77)});
  CHECK(blob.size() == 2 + 12);
  CHECK(static_cast<uint8_t>(blob[1]) == 1);
  QueueKind kind = QueueKind::Candle;
  std::vector<Candle> back;
  CHECK(summary::unpack(blob, kind, back));
  CHECK(kind == QueueKind::Scalar);
  CHECK(back.size() == 1);
  CHECK(back[0].time == 20'000);
  CHECK(back[0].open == 123.456 && back[0].high == 123.456 && back[0].low == 123.456 && back[0].close == 123.456);
  CHECK(!isDefined(back[0].volume));
}

TEST(a_volume_of_zero_is_no_volume_a_larger_one_saturates_and_a_fraction_is_rounded) {
  const auto volume = [](double v) { return summary::normalized(QueueKind::Candle, bar(1, 10, 11, 9, 10, v)).volume; };
  CHECK(!isDefined(volume(0)));
  CHECK(!isDefined(volume(kUndefined)));
  CHECK(!isDefined(volume(-5)));
  CHECK(!isDefined(volume(0.4)));  // rounds to none
  CHECK(volume(0.6) == 1);
  CHECK(volume(1234.5) == 1235);
  CHECK(volume(1234.4) == 1234);
  CHECK(volume(4'294'967'295.0) == 4'294'967'295.0);
  CHECK(volume(1e12) == 4'294'967'295.0);
  CHECK(volume(std::numeric_limits<double>::infinity()) == 4'294'967'295.0);
}

TEST(a_bar_with_no_open_has_its_close_as_its_base_and_keeps_its_high_and_low_from_it) {
  const Candle read = summary::normalized(QueueKind::Candle, bar(100, kUndefined, 12, 8, 10.125, kUndefined));
  CHECK(!isDefined(read.open) && read.close == 10.125);
  CHECK(read.high == 12 && read.low == 8);
  CHECK(!isDefined(read.volume));
  const std::string blob = summary::pack(QueueKind::Candle, {bar(100, kUndefined, 12, 8, 10.125, kUndefined)});
  QueueKind kind;
  std::vector<Candle> back;
  CHECK(summary::unpack(blob, kind, back) && back.size() == 1 && summary::sameBar(back[0], read));
}

TEST(an_undefined_delta_is_NaN_and_a_bar_without_a_close_or_a_key_that_fits_is_not_kept) {
  const Candle read = summary::normalized(QueueKind::Candle, bar(100, 10, kUndefined, 9, 11, 5));
  CHECK(!isDefined(read.high));
  CHECK(read.low == 9 && read.close == 11);
  CHECK(!summary::fits(bar(1, 10, 11, 9, kUndefined, 5)));
  CHECK(!summary::fits(bar(-1, 10, 11, 9, 10, 5)));
  CHECK(!summary::fits(bar(int64_t{1} << 32, 10, 11, 9, 10, 5)));
  CHECK(summary::fits(bar(4'294'967'295LL, 10, 11, 9, 10, 5)));
  QueueKind kind;
  std::vector<Candle> back;
  CHECK(summary::unpack(summary::pack(QueueKind::Candle, {bar(1, 10, 11, 9, kUndefined, 5), bar(2, 10, 11, 9, 10, 5)}), kind, back));
  CHECK(back.size() == 1 && back[0].time == 2);
  CHECK(!summary::unpack("", kind, back));
  CHECK(!summary::unpack(std::string("\x03\x00", 2), kind, back));         // another version (the one before too: a file of it is started over)
  CHECK(!summary::unpack(std::string("\x01\x00", 2), kind, back));
  CHECK(!summary::unpack(std::string("\x02\x00" "abc", 5), kind, back));    // not a whole number of records
}

TEST(the_same_bar_is_decided_on_the_stored_form) {
  // Two bars that differ below what a float delta holds are the same bar once stored.
  const Candle a = bar(7, 100, 101.00000001, 99, 100.5, 10), b = bar(7, 100, 101.00000002, 99, 100.5, 10.2);
  CHECK(!summary::sameBar(a, b));
  CHECK(summary::sameBar(summary::normalized(QueueKind::Candle, a), summary::normalized(QueueKind::Candle, b)));
  // A bar read back and normalized again is itself: nothing moves when it is stored twice.
  const Candle once = summary::normalized(QueueKind::Candle, bar(7, 100.1, 101.3, 99.7, 100.9, 1234.4));
  CHECK(summary::sameBar(once, summary::normalized(QueueKind::Candle, once)));
}

TEST(a_queue_is_cut_by_the_window_measured_back_from_its_last_bar_not_from_the_clock) {
  // A day of 5-minute bars, on a Friday and on a Sunday the same: measured from the last bar.
  std::vector<Candle> bars;
  const int64_t friday = slotOfInstant(Resolution::Min5, (dayNumber(2026, 11, 20) - kUnixEpochDayNumber) * 86400 + 10 * 3600);
  for (int i = 0; i < 400; i++) bars.push_back(bar(friday + i, 1, 1, 1, 1, 1));
  const size_t popped = summary::trim(bars, Resolution::Min5, 86400);
  CHECK(popped == 400 - 289);  // the last bar and the 288 before it, a day of five-minute bars (the window includes its start)
  CHECK(bars.back().time == friday + 399 && bars.front().time == friday + 399 - 288);
  // Daily bars: three months back from the last.
  std::vector<Candle> days;
  const int64_t last = dayNumber(2026, 11, 20);
  for (int i = 0; i < 200; i++) days.push_back(bar(last - 199 + i, 1, 1, 1, 1, 1));
  summary::trim(days, Resolution::Day, 92 * 86400);
  CHECK(days.size() == 93 && days.back().time == last && days.front().time == last - 92);
  CHECK(summary::trim(days, Resolution::Day, 0) == 0);  // no rule: nothing is popped
}

TEST(a_bar_at_a_time_already_there_replaces_it_and_a_new_time_is_put_in_its_place) {
  std::vector<Candle> bars = {bar(1, 1, 1, 1, 1, 1), bar(3, 1, 1, 1, 3, 1)};
  summary::put(bars, bar(3, 1, 1, 1, 33, 1));
  summary::put(bars, bar(2, 1, 1, 1, 2, 1));
  summary::put(bars, bar(4, 1, 1, 1, 4, 1));
  CHECK(bars.size() == 4);
  CHECK(bars[0].time == 1 && bars[1].time == 2 && bars[2].time == 3 && bars[3].time == 4);
  CHECK(bars[2].close == 33);
}

// ---- The SQLite store

TEST(edges_and_queues_put_in_the_file_come_back_with_their_coverage_and_no_bars_are_read_at_start) {
  TempDir t;
  Messages m;
  const EdgeKey a = edgeOf(1), b = edgeOf(2);
  const std::vector<Candle> days = {bar(740000, 10, 11, 9, 10.5, 100), bar(740001, 10.5, 12, 10, 11, 200)};
  {
    auto store = openSqliteSummaryStore(t.dir / "nordnet.sqlite", a.dataset, m.log());
    SummaryBatch batch;
    batch.edges.push_back(storedEdge(a, 10.5));
    StoredEdge noListing;
    noListing.listing.key = b;
    noListing.quote.last = 7;
    noListing.previous_close = 6.5;
    noListing.previous_close_day = 740001;
    batch.edges.push_back(noListing);
    batch.queues.push_back(storedQueue(a, Resolution::Day, days));
    batch.queues.push_back(storedQueue(a, Resolution::Week, {bar(739990, 1, 2, 0, 1, 5)}));
    store->apply(batch);
  }
  auto store = openSqliteSummaryStore(t.dir / "nordnet.sqlite", a.dataset, m.log());
  const std::vector<StoredEdge> edges = store->loadEdges();
  CHECK(edges.size() == 2);
  const StoredEdge * ea = nullptr;
  const StoredEdge * eb = nullptr;
  for (const StoredEdge & e : edges) (e.listing.key == a ? ea : eb) = &e;
  CHECK(ea && eb);
  if (ea && eb) {
    CHECK(ea->listing.key == a);
    CHECK(ea->listing.symbol == "NOKIA" && ea->listing.nordnetIdentifier == "ident" && ea->listing.nordnetOrderBookId == "book-1" && ea->listing.nordnetType == "STOCK" && ea->listing.name == "Nokia Oyj");
    CHECK(ea->quote.last == 10.5 && ea->quote.bid == 10.49 && ea->quote.ask == 10.51 && ea->quote.time == 1'700'000'000);
    CHECK(!isDefined(ea->previous_close));
    CHECK(ea->queues.size() == 2);
    CHECK(ea->queues[0].first == Resolution::Day && ea->queues[1].first == Resolution::Week);
    const QueueCoverage & day = ea->queues[0].second;
    CHECK(day.count == 2 && day.first_key == 740000 && day.last_key == 740001 && day.fetched == 42 && day.kind == QueueKind::Candle);
    CHECK(!eb->hasListing());
    CHECK(eb->quote.last == 7 && !isDefined(eb->quote.bid) && !isDefined(eb->quote.ask) && eb->quote.time == 0);
    CHECK(eb->previous_close == 6.5 && eb->previous_close_day == 740001);
    CHECK(eb->queues.empty());
  }
  const auto q = store->readQueue(a, Resolution::Day);
  CHECK(q && sameBars(q->bars, {summary::normalized(QueueKind::Candle, days[0]), summary::normalized(QueueKind::Candle, days[1])}));
  CHECK(q && q->fetched == 42 && q->kind == QueueKind::Candle);
  CHECK(!store->readQueue(b, Resolution::Day));
  CHECK(!store->readQueue(a, Resolution::Hour1));
  CHECK(m.lines.empty());
}

TEST(the_changes_highs_and_lows_of_an_edge_are_kept_and_read_back_with_the_edges_at_start) {
  TempDir t;
  Messages m;
  const EdgeKey a = edgeOf(1), b = edgeOf(2);
  {
    auto store = openSqliteSummaryStore(t.dir / "nordnet.sqlite", a.dataset, m.log());
    SummaryBatch batch;
    StoredEdge e = storedEdge(a, 10.5);
    e.changes.pct[static_cast<size_t>(Period::Quarter)] = 3.5;
    e.ranges.high[static_cast<size_t>(Period::Day)] = 11.25;
    e.ranges.low[static_cast<size_t>(Period::Day)] = 9.75;
    e.ranges.high[static_cast<size_t>(Period::Year10)] = 120;
    e.ranges.low[static_cast<size_t>(Period::Year10)] = 0.125;
    batch.edges.push_back(e);
    batch.edges.push_back(storedEdge(b, 3));  // says none
    store->apply(batch);
  }
  // A new store, which has read nothing before: every period's number comes with the edges.
  auto store = openSqliteSummaryStore(t.dir / "nordnet.sqlite", a.dataset, m.log());
  const std::vector<StoredEdge> edges = store->loadEdges();
  CHECK(edges.size() == 2);
  for (const StoredEdge & e : edges) {
    if (e.listing.key == a) {
      CHECK(e.changes.pct[static_cast<size_t>(Period::Quarter)] == 3.5 && !isDefined(e.changes.pct[static_cast<size_t>(Period::Day)]));
      CHECK(e.ranges.high[static_cast<size_t>(Period::Day)] == 11.25 && e.ranges.low[static_cast<size_t>(Period::Day)] == 9.75);
      CHECK(e.ranges.high[static_cast<size_t>(Period::Year10)] == 120 && e.ranges.low[static_cast<size_t>(Period::Year10)] == 0.125);
      CHECK(!isDefined(e.ranges.high[static_cast<size_t>(Period::Week)]) && !isDefined(e.ranges.low[static_cast<size_t>(Period::Year)]));
    } else {
      CHECK(!e.ranges.says() && !e.changes.says());
    }
  }
  CHECK(m.lines.empty());
}

TEST(a_file_of_the_schema_before_the_highs_and_lows_is_started_over) {
  TempDir t;
  Messages m;
  const EdgeKey a = edgeOf(1);
  {
    // As the file was before: version 4, the changes and no highs and lows.
    SQLite::Database db((t.dir / "nordnet.sqlite").string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE edges (id INTEGER PRIMARY KEY, base INTEGER NOT NULL, quote INTEGER NOT NULL, market INTEGER NOT NULL, last REAL, change_day REAL)");
    db.exec("INSERT INTO edges (base, quote, market, last, change_day) VALUES (1, 2, 3, 5.0, 1.0)");
    db.exec("PRAGMA user_version = 4");
  }
  auto store = openSqliteSummaryStore(t.dir / "nordnet.sqlite", a.dataset, m.log());
  CHECK(m.said("schema 4"));
  CHECK(store->loadEdges().empty());
  SummaryBatch batch;
  StoredEdge e = storedEdge(a, 1);
  e.ranges.high[0] = 2;
  batch.edges.push_back(e);
  store->apply(batch);
  const auto edges = store->loadEdges();
  CHECK(edges.size() == 1 && edges[0].ranges.high[0] == 2);
}

TEST(a_queue_put_replaces_the_one_there_and_a_merge_is_put_into_it_and_cut) {
  TempDir t;
  const EdgeKey a = edgeOf(1);
  auto store = openSqliteSummaryStore(t.dir / "nordnet.sqlite", a.dataset);
  const int64_t last = 740100;
  std::vector<Candle> days;
  for (int i = 0; i < 100; i++) days.push_back(bar(last - 99 + i, 1, 1, 1, 1 + i, 1));
  SummaryBatch first;
  first.edges.push_back(storedEdge(a, 1));
  first.queues.push_back(storedQueue(a, Resolution::Day, days));
  store->apply(first);
  CHECK(store->readQueue(a, Resolution::Day)->bars.size() == 100);
  // A merge: one bar replaced, one added, the window cut to 30 days from the new last.
  SummaryBatch merge;
  merge.queues.push_back(storedQueue(a, Resolution::Day, {bar(last, 1, 1, 1, 555, 1), bar(last + 1, 1, 1, 1, 7, 1)}, true, 30 * 86400));
  store->apply(merge);
  auto q = store->readQueue(a, Resolution::Day);
  CHECK(q && q->bars.size() == 31);
  CHECK(q && q->bars.back().time == last + 1 && q->bars.back().close == 7);
  CHECK(q && q->bars[q->bars.size() - 2].time == last && q->bars[q->bars.size() - 2].close == 555);
  CHECK(q && q->bars.front().time == last + 1 - 30);
  // A put replaces the whole queue.
  SummaryBatch put;
  put.queues.push_back(storedQueue(a, Resolution::Day, {bar(5, 1, 1, 1, 9, 1)}));
  store->apply(put);
  q = store->readQueue(a, Resolution::Day);
  CHECK(q && q->bars.size() == 1 && q->bars[0].time == 5);
  // A merge into a queue the file does not have makes it.
  SummaryBatch fresh;
  fresh.queues.push_back(storedQueue(a, Resolution::Week, {bar(8, 1, 1, 1, 2, 1)}, true, 365 * 86400));
  store->apply(fresh);
  CHECK(store->readQueue(a, Resolution::Week)->bars.size() == 1);
  // An edge's row is made bare for a queue that comes without it.
  const EdgeKey b = edgeOf(2);
  SummaryBatch bare;
  bare.queues.push_back(storedQueue(b, Resolution::Day, {bar(5, 1, 1, 1, 9, 1)}));
  store->apply(bare);
  CHECK(store->loadEdges().size() == 2);
}

TEST(a_value_queue_is_kept_as_value_records) {
  TempDir t;
  const EdgeKey a = edgeOf(1);
  auto store = openSqliteSummaryStore(t.dir / "nordnet.sqlite", a.dataset);
  SummaryBatch batch;
  StoredQueue q = storedQueue(a, Resolution::Day, {bar(5, 3, 3, 3, 3, kUndefined), bar(6, 4, 4, 4, 4, kUndefined)});
  q.kind = QueueKind::Scalar;
  batch.edges.push_back(storedEdge(a, 4));
  batch.queues.push_back(q);
  store->apply(batch);
  const auto read = store->readQueue(a, Resolution::Day);
  CHECK(read && read->kind == QueueKind::Scalar && read->bars.size() == 2 && read->bars[1].close == 4 && !isDefined(read->bars[1].volume));
  CHECK(store->loadEdges()[0].queues[0].second.kind == QueueKind::Scalar);
}

TEST(a_damaged_file_is_moved_aside_and_a_new_one_made_and_the_log_says_so) {
  TempDir t;
  Messages m;
  const EdgeKey a = edgeOf(1);
  {
    std::ofstream out(t.dir / "nordnet.sqlite", std::ios::binary);
    out << std::string(5000, 'x');
  }
  auto store = openSqliteSummaryStore(t.dir / "nordnet.sqlite", a.dataset, m.log());
  CHECK(m.said("moved aside"));
  CHECK(t.movedAside() >= 1);
  SummaryBatch batch;
  batch.edges.push_back(storedEdge(a, 1));
  store->apply(batch);
  CHECK(store->loadEdges().size() == 1);
}

TEST(a_file_of_another_schema_is_emptied_and_started_over) {
  TempDir t;
  Messages m;
  const EdgeKey a = edgeOf(1);
  {
    SQLite::Database db((t.dir / "nordnet.sqlite").string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE edges (x INTEGER)");
    db.exec("INSERT INTO edges VALUES (1)");
    db.exec("PRAGMA user_version = 99");
  }
  auto store = openSqliteSummaryStore(t.dir / "nordnet.sqlite", a.dataset, m.log());
  CHECK(m.said("schema 99"));
  CHECK(store->loadEdges().empty());
  SummaryBatch batch;
  batch.edges.push_back(storedEdge(a, 1));
  store->apply(batch);
  CHECK(store->loadEdges().size() == 1);
  CHECK(t.movedAside() == 0);  // emptied in place, as the facts are
}

TEST(a_directory_that_cannot_be_made_keeps_nothing_and_says_so_and_nothing_throws) {
  TempDir t;
  Messages m;
  { std::ofstream(t.dir / "a-file") << "x"; }
  const EdgeKey a = edgeOf(1);
  auto store = openSqliteSummaryStore(t.dir / "a-file" / "summaries" / "nordnet.sqlite", a.dataset, m.log());
  CHECK(m.said("not kept"));
  SummaryBatch batch;
  batch.edges.push_back(storedEdge(a, 1));
  batch.queues.push_back(storedQueue(a, Resolution::Day, {bar(5, 1, 1, 1, 9, 1)}));
  store->apply(batch);
  CHECK(store->loadEdges().empty());
  CHECK(!store->readQueue(a, Resolution::Day));
  // A directory of them with no path keeps nothing either.
  auto none = openSqliteSummaries({});
  CHECK(none->datasets().empty() && !none->open("nordnet"));
}

TEST(a_directory_of_stores_has_a_file_for_each_dataset) {
  TempDir t;
  auto stores = openSqliteSummaries(t.dir / "summaries");
  auto a = stores->open("nordnet");
  auto b = stores->open("spfb");
  CHECK(a && b && a != b);
  CHECK(stores->open("nordnet") == a);
  SummaryBatch batch;
  batch.edges.push_back(storedEdge(edgeOf(1), 1));
  a->apply(batch);
  CHECK(fs::exists(t.dir / "summaries" / "nordnet.sqlite") && fs::exists(t.dir / "summaries" / "spfb.sqlite"));
  auto again = openSqliteSummaries(t.dir / "summaries");
  const auto codes = again->datasets();
  CHECK(codes.size() == 2 && codes[0] == "nordnet" && codes[1] == "spfb");
  CHECK(!stores->open("not a code!"));
}

// ---- The writer

namespace {

// A store that records what it is given and takes its time over it.
class SlowStore : public SummaryStore {
 public:
  std::vector<StoredEdge> loadEdges() override { return {}; }
  std::optional<StoredQueue> readQueue(const EdgeKey &, Resolution) override { return std::nullopt; }
  void apply(const SummaryBatch & b) override {
    std::this_thread::sleep_for(delay);
    std::lock_guard lock(mutex);
    batches.push_back(b);
  }
  std::chrono::milliseconds delay{0};
  std::mutex mutex;
  std::vector<SummaryBatch> batches;
};

class OneStore : public SummaryStores {
 public:
  explicit OneStore(std::shared_ptr<SlowStore> s) : store(std::move(s)) {}
  std::vector<std::string> datasets() override { return {"nordnet"}; }
  std::shared_ptr<SummaryStore> open(const std::string &) override { return store; }
  std::shared_ptr<SlowStore> store;
};

}  // namespace

TEST(the_writer_puts_what_is_waiting_together_one_row_per_edge_and_one_blob_per_queue_the_last_said) {
  auto slow = std::make_shared<SlowStore>();
  SummaryWriter writer(std::make_shared<OneStore>(slow), SummaryWriter::Options{std::chrono::milliseconds(50), std::chrono::milliseconds(50)});
  const EdgeKey a = edgeOf(1);
  for (int i = 0; i < 5; i++) {
    SummaryBatch batch;
    batch.edges.push_back(storedEdge(a, 10 + i));
    batch.queues.push_back(storedQueue(a, Resolution::Day, {bar(i + 1, 1, 1, 1, i, 1)}));
    writer.post(std::move(batch));
  }
  writer.flush();
  CHECK(slow->batches.size() == 1);
  if (!slow->batches.empty()) {
    CHECK(slow->batches[0].edges.size() == 1 && slow->batches[0].edges[0].quote.last == 14);
    CHECK(slow->batches[0].queues.size() == 1 && slow->batches[0].queues[0].bars.size() == 1 && slow->batches[0].queues[0].bars[0].time == 5);
  }
}

TEST(a_merge_waiting_is_folded_into_the_queue_waiting_for_the_same_key_and_a_put_replaces_all) {
  auto slow = std::make_shared<SlowStore>();
  SummaryWriter writer(std::make_shared<OneStore>(slow), SummaryWriter::Options{std::chrono::milliseconds(200), std::chrono::milliseconds(200)});
  const EdgeKey a = edgeOf(1), b = edgeOf(2);
  const auto post = [&](StoredQueue q) {
    SummaryBatch batch;
    batch.queues.push_back(std::move(q));
    writer.post(std::move(batch));
  };
  post(storedQueue(a, Resolution::Day, {bar(1, 1, 1, 1, 1, 1), bar(2, 1, 1, 1, 2, 1)}));
  post(storedQueue(a, Resolution::Day, {bar(2, 1, 1, 1, 22, 1), bar(3, 1, 1, 1, 3, 1)}, true, 86400 * 100));  // onto a put: still a put
  post(storedQueue(b, Resolution::Day, {bar(2, 1, 1, 1, 22, 1)}, true, 86400 * 100));
  post(storedQueue(b, Resolution::Day, {bar(3, 1, 1, 1, 3, 1)}, true, 86400 * 100));  // onto a merge: still a merge, both bars
  const auto waiting = writer.pending(a, Resolution::Day);
  CHECK(waiting.size() == 1 && waiting[0].bars.size() == 3 && !waiting[0].merge);
  writer.flush();
  CHECK(slow->batches.size() == 1);
  for (const StoredQueue & q : slow->batches[0].queues) {
    if (q.edge == a) CHECK(!q.merge && q.bars.size() == 3 && q.bars[1].close == 22);
    else CHECK(q.merge && q.bars.size() == 2);
  }
  CHECK(writer.pending(a, Resolution::Day).empty());
}

TEST(the_writer_never_makes_the_poster_wait_for_the_store_and_what_is_being_written_is_still_pending) {
  auto slow = std::make_shared<SlowStore>();
  slow->delay = std::chrono::milliseconds(300);
  SummaryWriter writer(std::make_shared<OneStore>(slow), SummaryWriter::Options{std::chrono::milliseconds(1), std::chrono::milliseconds(1)});
  const EdgeKey a = edgeOf(1);
  SummaryBatch batch;
  batch.queues.push_back(storedQueue(a, Resolution::Day, {bar(1, 1, 1, 1, 1, 1)}));
  const auto start = std::chrono::steady_clock::now();
  writer.post(batch);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // taken by the writer, in the store's apply now
  writer.post(batch);
  writer.post(batch);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250));
  CHECK(!writer.pending(a, Resolution::Day).empty());  // in progress and waiting both
  CHECK(writer.pending(a, Resolution::Day).size() == 2);
  writer.flush();
  CHECK(writer.pending(a, Resolution::Day).empty());
  CHECK(slow->batches.size() == 2);
}

TEST(the_writer_writes_what_waits_when_it_is_destroyed_and_a_store_that_cannot_be_opened_loses_nothing_else) {
  auto slow = std::make_shared<SlowStore>();
  {
    SummaryWriter writer(std::make_shared<OneStore>(slow), SummaryWriter::Options{std::chrono::hours(1), std::chrono::hours(1)});
    SummaryBatch batch;
    batch.edges.push_back(storedEdge(edgeOf(1), 1));
    writer.post(batch);
  }
  CHECK(slow->batches.size() == 1);
  SummaryWriter none(nullptr);
  SummaryBatch batch;
  batch.edges.push_back(storedEdge(edgeOf(1), 1));
  none.post(batch);
  none.flush();
  CHECK(none.failed() == 0);
}

TEST(the_writer_puts_the_edges_quickly_and_the_queues_at_most_once_in_their_delay) {
  auto slow = std::make_shared<SlowStore>();
  SummaryWriter writer(std::make_shared<OneStore>(slow), SummaryWriter::Options{std::chrono::milliseconds(10), std::chrono::milliseconds(400)});
  const EdgeKey a = edgeOf(1);
  const auto start = std::chrono::steady_clock::now();
  const auto post = [&](double last, double close) {
    SummaryBatch batch;
    batch.edges.push_back(storedEdge(a, last));
    batch.queues.push_back(storedQueue(a, Resolution::Day, {bar(1, 1, 1, 1, close, 1)}));
    writer.post(std::move(batch));
  };
  post(1, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  {
    std::lock_guard lock(slow->mutex);
    CHECK(slow->batches.size() == 1 && slow->batches[0].edges.size() == 1 && slow->batches[0].queues.empty());  // the quote went, the queue waits
  }
  post(2, 2);
  post(3, 3);  // said again meanwhile: one write
  CHECK(!writer.pending(a, Resolution::Day).empty());
  writer.flush();
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(380));  // a flush does not wait for the delay
  size_t queues = 0;
  for (const SummaryBatch & b : slow->batches)
    for (const StoredQueue & q : b.queues) {
      queues++;
      CHECK(q.bars[0].close == 3);
    }
  CHECK(queues == 1);
}
