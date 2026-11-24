#include "HistoryWorld.h"
#include "TestFramework.h"
#include "TestIsin.h"

#include "history/HistoryLoader.h"
#include "model/Dates.h"
#include "model/NodeKey.h"
#include "model/Period.h"
#include "model/SqliteFactStore.h"
#include "model/SqliteSummaryStore.h"
#include "model/Store.h"
#include "model/SummaryRecords.h"
#include "portfolio/Portfolio.h"
#include "source/DatasetCodes.h"
#include "source/NordnetHistory.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

// The summary in the store: the quote and the queues derived from the bars a source writes, kept on the disk and read back when they are
// wanted. The data (candles) is the other half and is never kept.
namespace {

namespace fs = std::filesystem;

constexpr int64_t kLast = dayNumber(2026, 11, 20);  // a Friday
int64_t midnight(int64_t day) { return (day - kUnixEpochDayNumber) * 86400; }

Candle bar(int64_t time, double close, double volume = 1) { return Candle{time, close, close, close, close, volume}; }

// Daily bars on the `n` days ending on `last`, closing at 100, 101, ...
std::vector<Candle> days(int n, int64_t last = kLast) {
  std::vector<Candle> out;
  for (int i = 0; i < n; i++) out.push_back(Candle{last - n + 1 + i, 100.0 + i, 101.0 + i, 99.0 + i, 100.5 + i, 10.0 + i});
  return out;
}

template <class F>
bool waitFor(F done, std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return done();
}

// What the stores were asked, over every session of a test.
struct Counts {
  std::atomic<int> reads{0}, applies{0}, edges_loaded{0};
  std::atomic<size_t> queues_put{0}, edges_put{0};
  void reset() {
    reads = applies = edges_loaded = 0;
    queues_put = edges_put = 0;
  }
};

class CountingStore : public SummaryStore {
 public:
  CountingStore(std::shared_ptr<SummaryStore> inner, Counts & counts) : inner_(std::move(inner)), counts_(counts) {}
  std::vector<StoredEdge> loadEdges() override {
    counts_.edges_loaded++;
    return inner_->loadEdges();
  }
  std::optional<StoredQueue> readQueue(const EdgeKey & e, Resolution r) override {
    counts_.reads++;
    return inner_->readQueue(e, r);
  }
  void apply(const SummaryBatch & b) override {
    counts_.applies++;
    counts_.queues_put += b.queues.size();
    counts_.edges_put += b.edges.size();
    inner_->apply(b);
  }

 private:
  std::shared_ptr<SummaryStore> inner_;
  Counts & counts_;
};

class CountingStores : public SummaryStores {
 public:
  CountingStores(std::shared_ptr<SummaryStores> inner, Counts & counts) : inner_(std::move(inner)), counts_(counts) {}
  std::vector<std::string> datasets() override { return inner_->datasets(); }
  std::shared_ptr<SummaryStore> open(const std::string & dataset) override {
    auto s = inner_->open(dataset);
    return s ? std::make_shared<CountingStore>(std::move(s), counts_) : nullptr;
  }

 private:
  std::shared_ptr<SummaryStores> inner_;
  Counts & counts_;
};

// A directory of its own, gone with the object, and sessions on it: a store is made, used and dropped, and another made on the same files.
struct Disk {
  fs::path dir;
  Counts counts;
  TimestampMs now = 1'800'000'000'000;
  Disk() {
    static std::atomic<int> counter{0};
    dir = fs::temp_directory_path() /
          ("stocks-summary-store-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(counter++));
    fs::create_directories(dir);
  }
  ~Disk() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  StoreOptions options(size_t budget = kDefaultQueueBudget, std::function<void()> changed = {}) {
    StoreOptions o;
    o.facts = openSqliteFactStore(dir / "facts.sqlite", [](const std::string &) {});
    o.portfolios = openSqlitePortfolioStore(dir / "portfolios.sqlite", [](const std::string &) {});
    o.summaries = std::make_shared<CountingStores>(openSqliteSummaries(dir / "summaries", [this](const std::string & t) { say(t); }), counts);
    o.writer.changed_delay = o.writer.rechecked_delay = std::chrono::milliseconds(1);
    o.summary_writer.delay = std::chrono::milliseconds(1);
    o.clock = [this] { return now; };
    o.queue_budget = budget;
    o.changed = std::move(changed);
    return o;
  }
  void say(const std::string & text) {
    std::lock_guard lock(mutex);
    messages.push_back(text);
  }
  bool said(const std::string & part) {
    std::lock_guard lock(mutex);
    for (const std::string & m : messages)
      if (m.find(part) != std::string::npos) return true;
    return false;
  }
  std::mutex mutex;
  std::vector<std::string> messages;
};

// An instrument or two on a market, in euros, of a dataset.
struct Things {
  InstrumentId eur, nokia, elisa, fund, index;
  MarketId hel, off;
  DatasetId ds;
  explicit Things(Store & s) {
    eur = s.addInstrument("EUR", "Euro", InstrumentKind::Currency);
    nokia = s.addInstrument("", "Nokia Oyj", InstrumentKind::Company, "FI0009000681", "STOCK");
    elisa = s.addInstrument("", "Elisa", InstrumentKind::Company, testIsin(7), "STOCK");
    fund = s.addInstrument("", "A fund", InstrumentKind::Fund, testIsin(8), "FUND");
    index = s.addInstrument("", "An index", InstrumentKind::Index, testIsin(9), "INDEX");
    hel = s.addMarket("XHEL", "Nasdaq Helsinki");
    off = s.addMarket(kOffExchange, "Off exchange");
    ds = s.addDataset("nordnet", "Nordnet");
  }
  EdgeKey edge(InstrumentId base) const { return EdgeKey{base, eur, base == fund ? off : hel, ds}; }
};

}  // namespace

TEST(a_bar_newer_than_the_quotes_time_sets_the_last_price_and_a_quote_a_source_wrote_is_not_overwritten_by_an_older_bar) {
  auto store = makeMemoryStore();
  Things t(*store);
  const EdgeKey k = t.edge(t.nokia);
  CHECK(!store->snapshot()->quote(k));
  // A source that gives only bars still gives a price: the close of the newest bar, at the time of the bar.
  store->appendCandles(k, Resolution::Day, {bar(kLast - 1, 6), bar(kLast, 7)});
  auto q = store->snapshot()->quote(k);
  CHECK(q && q->last == 7 && q->time == midnight(kLast) && !isDefined(q->bid) && !isDefined(q->ask));
  CHECK(store->snapshot()->listed(k) && store->snapshot()->quoteRowCount() == 1);
  // A source's quote replaces it.
  const Timestamp noon = midnight(kLast) + 12 * 3600;
  store->setQuote(k, {10, 9.9, 10.1, noon});
  q = store->snapshot()->quote(k);
  CHECK(q && q->last == 10 && q->bid == 9.9 && q->ask == 10.1 && q->time == noon);
  // An older bar does not overwrite it, and neither does a bar from the same time.
  store->appendCandles(k, Resolution::Min5, {bar(slotOfInstant(Resolution::Min5, noon - 3600), 5)});
  CHECK(store->snapshot()->quote(k)->last == 10);
  store->appendCandles(k, Resolution::Min5, {bar(slotOfInstant(Resolution::Min5, noon), 4)});
  CHECK(store->snapshot()->quote(k)->last == 10);
  // A newer one does: its close and its time; the bid and the ask stay.
  store->appendCandles(k, Resolution::Min5, {bar(slotOfInstant(Resolution::Min5, noon + 3600), 11)});
  q = store->snapshot()->quote(k);
  CHECK(q && q->last == 11 && q->time == noon + 3600 && q->bid == 9.9 && q->ask == 10.1);
  // A quote said with an earlier time than the one there is left out: the newest says.
  store->setQuote(k, {1, kUndefined, kUndefined, noon});
  CHECK(store->snapshot()->quote(k)->last == 11);
  // A quote that says a last price only leaves the rest unset.
  store->setQuote(k, {12, kUndefined, kUndefined, noon + 7200});
  q = store->snapshot()->quote(k);
  CHECK(q && q->last == 12 && !isDefined(q->bid) && !isDefined(q->ask));
  // The data is as it was written: every bar, none cut.
  CHECK(store->snapshot()->candles(k, Resolution::Day).candles.size() == 2);
}

TEST(the_same_bar_or_quote_again_makes_no_version_and_writes_nothing) {
  Disk d;
  auto store = makeMemoryStore(d.options());
  Things t(*store);
  const EdgeKey k = t.edge(t.nokia);
  store->batch([&](Batch & b) {
    b.appendCandles(k, Resolution::Day, days(20));
    b.setQuote(k, {120, 119.9, 120.1, midnight(kLast) + 40000});
    b.setListing({k, "NOKIA", "id", "book-1", "STOCK", "Nokia"});
  });
  store->flush();
  const uint64_t version = store->snapshot()->version();
  const int applies = d.counts.applies;
  store->appendCandles(k, Resolution::Day, days(20));
  store->setQuote(k, {120, 119.9, 120.1, midnight(kLast) + 40000});
  store->setListing({k, "NOKIA", "id", "book-1", "STOCK", "Nokia"});
  store->flush();
  CHECK(store->snapshot()->version() == version);
  CHECK(d.counts.applies == applies);
  // A bar that differs from the one there below what is stored is the same bar for the summary: nothing is written, though the data (which
  // is as the source wrote it) is a different bar, so a version.
  std::vector<Candle> nearly = days(20);
  nearly.back().high += 1e-9;
  nearly.back().volume += 0.2;
  store->appendCandles(k, Resolution::Day, nearly);
  store->flush();
  CHECK(d.counts.applies == applies);
  CHECK(store->snapshot()->candles(k, Resolution::Day).candles.back().high == nearly.back().high);
  // One that differs is a new version and a write.
  const uint64_t before = store->snapshot()->version();
  std::vector<Candle> other = days(20);
  other.back().close += 1;
  store->appendCandles(k, Resolution::Day, other);
  store->flush();
  CHECK(store->snapshot()->version() == before + 1);
  CHECK(d.counts.applies == applies + 1);
}

TEST(a_queue_replaces_a_bar_at_a_time_it_has_pushes_a_new_one_and_pops_the_old_by_the_window_measured_from_its_last_bar) {
  auto store = makeMemoryStore();
  Things t(*store);
  const EdgeKey k = t.edge(t.nokia);
  store->setRetention(t.ds, Resolution::Day, std::chrono::seconds(10 * 86400));
  store->appendCandles(k, Resolution::Day, days(30));  // kLast - 29 .. kLast
  auto queue = store->snapshot()->queue(k, Resolution::Day);
  CHECK(queue.size() == 11 && queue.front().time == kLast - 10 && queue.back().time == kLast);  // the last bar and ten days back
  // A bar at a time already there replaces it.
  store->appendCandles(k, Resolution::Day, {Candle{kLast, 1, 2, 0.5, 1.5, 4}});
  queue = store->snapshot()->queue(k, Resolution::Day);
  CHECK(queue.size() == 11 && queue.back().close == 1.5 && queue.back().high == 2 && queue.back().volume == 4);
  // A new time is pushed, and the old end goes.
  store->appendCandles(k, Resolution::Day, {bar(kLast + 3, 9)});
  queue = store->snapshot()->queue(k, Resolution::Day);
  CHECK(queue.size() == 9 && queue.front().time == kLast - 7 && queue.back().time == kLast + 3);
  // A bar older than the window is no bar of it: the queue is as it was (it is in the data).
  const auto fetched = store->snapshot()->coverage(k, Resolution::Day)->fetched;
  store->appendCandles(k, Resolution::Day, {bar(kLast - 20, 1)});
  CHECK(store->snapshot()->queue(k, Resolution::Day).size() == 9 && store->snapshot()->queue(k, Resolution::Day).front().time == kLast - 7);
  CHECK(store->snapshot()->coverage(k, Resolution::Day)->fetched == fetched);
  // A bar between two is put in its place.
  store->appendCandles(k, Resolution::Day, {bar(kLast - 5, 77)});
  queue = store->snapshot()->queue(k, Resolution::Day);
  CHECK(queue.size() == 9 && queue[2].time == kLast - 5 && queue[2].close == 77);
  // The data is every bar there was, as written.
  CHECK(store->snapshot()->candles(k, Resolution::Day).candles.size() == 30 + 1);
  CHECK(store->snapshot()->candles(k, Resolution::Day).candles[0].time == kLast - 29);
  // No rule, no queue: the resolution has none, and the data has its bars.
  store->setRetention(t.ds, Resolution::Day, std::nullopt);
  store->appendCandles(k, Resolution::Hour4, {bar(1000, 5)});
  CHECK(store->snapshot()->queue(k, Resolution::Hour4).empty() && !store->snapshot()->coverage(k, Resolution::Hour4));
  CHECK(store->snapshot()->candles(k, Resolution::Hour4).candles.size() == 1);
}

TEST(a_sundays_day_queue_is_fridays_session_and_the_trace_ends_with_the_last_price) {
  auto store = makeMemoryStore();
  Things t(*store);
  const EdgeKey k = t.edge(t.nokia);
  const int64_t friday = kLast, thursday = kLast - 1;
  const auto slot = [](int64_t day, int hour, int minute) { return slotOfInstant(Resolution::Min5, midnight(day) + hour * 3600 + minute * 60); };
  store->appendCandles(k, Resolution::Day, {bar(thursday, 10), bar(friday, 12)});
  // Thursday's tail is older than a day before Friday's last bar and is popped; the rest of Thursday's session is within the day.
  store->appendCandles(k, Resolution::Min5, {bar(slot(thursday, 8, 0), 10), bar(slot(thursday, 15, 55), 10.5), bar(slot(friday, 8, 0), 11), bar(slot(friday, 8, 5), 11.5), bar(slot(friday, 15, 55), 12)});
  auto queue = store->snapshot()->queue(k, Resolution::Min5);
  CHECK(queue.size() == 4 && queue.front().time == slot(thursday, 15, 55));  // 24 hours back from 15:55 on Friday
  // Nothing is written on the Saturday and the Sunday: the day is Friday's session, as it was.
  const PeriodView v = periodView(*store->snapshot(), k, Period::Day, true);
  CHECK(v.primary && v.closes.size() == 3 && v.closes[0] == 11.0f && v.closes[2] == 12.0f);  // Friday's bars only (the queue has Thursday's last as well)
  CHECK(v.base == 10 && v.last == 12);  // the daily bar before the session
  store->setPreviousClose(k, friday, 10.25);
  CHECK(periodView(*store->snapshot(), k, Period::Day, false).base == 10.25);
  // A price that is newer than the bars ends the trace.
  store->setQuote(k, {12.5, kUndefined, kUndefined, midnight(friday) + 16 * 3600});
  const PeriodView later = periodView(*store->snapshot(), k, Period::Day, true);
  CHECK(later.last == 12.5 && later.closes.size() == 4 && later.closes.back() == 12.5f);
}

TEST(the_change_over_a_period_is_the_last_price_against_the_first_bar_of_its_queue) {
  auto store = makeMemoryStore();
  Things t(*store);
  const EdgeKey k = t.edge(t.nokia);
  store->setRetention(t.ds, Resolution::Day, std::chrono::seconds(92 * 86400));
  store->appendCandles(k, Resolution::Day, days(100));  // closes 100.5 .. 199.5, the newest on kLast
  const auto snapshot = store->snapshot();
  const PeriodView quarter = periodView(*snapshot, k, Period::Quarter, true);
  // The window is 90 days back from the last bar, and its first bar is what the price was at the start of it.
  CHECK(quarter.primary && quarter.closes.size() == 90);
  CHECK(quarter.base == quarter.closes.front() && quarter.base == snapshot->queue(k, Resolution::Day)[92 - 89].close);
  CHECK(quarter.last == 199.5 && quarter.closes.back() == 199.5f);
  CHECK_NEAR(quarter.change(), (199.5 / quarter.base - 1) * 100, 1e-9);
  // The month is a part of the same queue.
  const PeriodView month = periodView(*snapshot, k, Period::Month, true);
  CHECK(month.closes.size() == 30 && month.base == month.closes.front());
  // A last price that has moved: the trace ends with it, and the base is not moved.
  store->setQuote(k, {210, 209, 211, midnight(kLast) + 50000});
  const PeriodView moved = periodView(*store->snapshot(), k, Period::Quarter, true);
  CHECK(moved.base == quarter.base && moved.last == 210 && moved.closes.size() == 91 && moved.closes.back() == 210.0f);
  // One bar is not a quarter: no number rather than a wrong one.
  const EdgeKey fresh = t.edge(t.elisa);
  store->appendCandles(fresh, Resolution::Day, {bar(kLast, 5)});
  const PeriodView one = periodView(*store->snapshot(), fresh, Period::Quarter, true);
  CHECK(!one.primary && !isDefined(one.change()) && one.closes.empty());
}

TEST(a_fund_and_an_index_keep_value_queues_and_a_company_keeps_bars) {
  auto store = makeMemoryStore();
  Things t(*store);
  const EdgeKey fund = t.edge(t.fund), index = t.edge(t.index), company = t.edge(t.nokia);
  for (const EdgeKey & k : {fund, index, company}) store->appendCandles(k, Resolution::Day, {Candle{kLast - 1, 10, 12, 9, 11, 500}, Candle{kLast, 11, 13, 10, 12, 600}});
  const auto s = store->snapshot();
  CHECK(s->coverage(fund, Resolution::Day)->kind == QueueKind::Value && s->coverage(index, Resolution::Day)->kind == QueueKind::Value);
  CHECK(s->coverage(company, Resolution::Day)->kind == QueueKind::Candle);
  for (const EdgeKey & k : {fund, index}) {
    const auto q = s->queue(k, Resolution::Day);
    CHECK(q.size() == 2 && q[1].open == 12 && q[1].high == 12 && q[1].low == 12 && q[1].close == 12 && !isDefined(q[1].volume));
    // The data keeps what the source wrote.
    CHECK(s->candles(k, Resolution::Day).candles[1].high == 13 && s->candles(k, Resolution::Day).candles[1].volume == 600);
  }
  const auto c = s->queue(company, Resolution::Day);
  CHECK(c.size() == 2 && c[1].open == 11 && c[1].high == 13 && c[1].low == 10 && c[1].close == 12 && c[1].volume == 600);
}

TEST(after_a_restart_every_edge_shows_its_price_bid_and_ask_and_a_portfolio_is_valued_with_no_network_and_no_queue_in_memory) {
  Disk d;
  const PortfolioId mine{portfolioKey(0x1234567890abcdull)->value};
  EdgeKey nokia, elisa, fund;
  {
    auto store = makeMemoryStore(d.options());
    Things t(*store);
    nokia = t.edge(t.nokia);
    elisa = t.edge(t.elisa);
    fund = t.edge(t.fund);
    store->batch([&](Batch & b) {
      b.appendCandles(nokia, Resolution::Day, days(92));
      b.appendCandles(nokia, Resolution::Min5, {bar(slotOfInstant(Resolution::Min5, midnight(kLast) + 8 * 3600), 190)});
      b.setQuote(nokia, {5.5, 5.4, 5.6, midnight(kLast) + 50000});
      b.setListing({nokia, "NOKIA", "id-n", "book-n", "STOCK", "Nokia Oyj"});
      b.setPreviousClose(nokia, kLast, 5.25);
      b.appendCandles(elisa, Resolution::Day, days(10));
      b.setQuote(elisa, {50, 49, 51, midnight(kLast) + 3600});
      b.setListing({elisa, "ELISA", "id-e", "book-e", "STOCK", "Elisa Oyj"});
      b.appendCandles(fund, Resolution::Day, days(10));
      b.setListing({fund, "AFUND", "", "", "FUND", "A fund"});
    });
    const portfolio::Transaction buy{1, kLast - 3, portfolio::Side::Buy, t.nokia, t.hel, t.eur, 10, 4, 1, "x"};
    store->setAttributes(NodeRef(mine), {{predicate::name, "Mine"}, {predicate::currency, NodeKey{t.eur.value}}});
    store->appendTransactions({mine, store->addDataset(kUserDataset)}, {buy});
    store->flush();
  }
  // No network: no Http is made here at all. And no queue is read: `loadEdges` is all the start-up asks.
  d.counts.reads = 0;
  auto store = makeMemoryStore(d.options());
  CHECK(d.counts.reads == 0);
  const auto s = store->snapshot();
  CHECK(s->quoteRowCount() == 3 && s->edges(*s->findMarket("XHEL")).size() == 2 && s->edges(*s->findMarket(kOffExchange)).size() == 1);
  const auto q = s->quote(nokia);
  CHECK(q && q->last == 5.5 && q->bid == 5.4 && q->ask == 5.6 && q->time == midnight(kLast) + 50000);
  CHECK(s->quote(elisa)->bid == 49 && s->quote(elisa)->ask == 51);
  CHECK(s->quote(fund) && s->quote(fund)->last == 109.5);  // a fund edge with no quote said keeps the one its newest bar gave
  const auto l = s->listing(nokia);
  CHECK(l && l->symbol == "NOKIA" && l->identifier == "id-n" && l->orderBook == "book-n" && l->type == "STOCK" && l->name == "Nokia Oyj");
  CHECK(s->previousClose(nokia, kLast) == 5.25 && !s->previousClose(nokia, kLast + 1));
  CHECK(s->findEdge("NOKIA", *s->findMarket("XHEL")) == nokia);
  // Queues are on the disk only: coverage, no bars.
  CHECK(s->queue(nokia, Resolution::Day).empty() && !s->queueResident(nokia, Resolution::Day));
  const auto day = s->coverage(nokia, Resolution::Day);
  CHECK(day && day->count == 92 && day->first_key == kLast - 91 && day->last_key == kLast && day->kind == QueueKind::Candle);
  CHECK(s->coverage(fund, Resolution::Day)->kind == QueueKind::Value);
  CHECK(s->queues(nokia) == std::vector<Resolution>({Resolution::Min5, Resolution::Day}));
  CHECK(s->candles(nokia, Resolution::Day).candles.empty());  // the data is not kept
  // The portfolio is valued from the quote.
  const auto held = portfolio::holdings(*s, mine);
  CHECK(held.positions.size() == 1);
  const auto eur = *s->findInstrument("EUR");
  const portfolio::Valuation v = portfolio::value(held.positions, *s, eur);
  CHECK(!v.incomplete && v.rows.size() == 1 && v.rows[0].price && v.rows[0].price->close == 5.5);
  CHECK_NEAR(v.total, 55, 1e-9);
  // A row with no queue shows the price and no trace.
  const PeriodView view = periodView(*s, nokia, Period::Quarter, true);
  CHECK(view.closes.empty() && !isDefined(view.change()));
}

TEST(a_queue_is_read_once_when_wanted_and_nothing_read_is_written_back_and_a_snapshot_held_keeps_what_it_had) {
  Disk d;
  EdgeKey nokia;
  {
    auto store = makeMemoryStore(d.options());
    Things t(*store);
    nokia = t.edge(t.nokia);
    store->appendCandles(nokia, Resolution::Day, days(92));
    store->setListing({nokia, "NOKIA", "", "book", "STOCK", "Nokia"});
    store->flush();
  }
  std::atomic<int> changes{0};
  auto store = makeMemoryStore(d.options(kDefaultQueueBudget, [&] { changes++; }));
  d.counts.reset();
  const auto held = store->snapshot();
  const uint64_t version = held->version();
  store->want({SeriesKey{nokia, Resolution::Day}});
  CHECK(waitFor([&] { return store->snapshot()->queueResident(nokia, Resolution::Day); }));
  const auto s = store->snapshot();
  // Reading publishes a version: what shows a trace is drawn again for it. A snapshot already held has what it had.
  CHECK(s->version() == version + 1 && changes >= 1);
  CHECK(s->queue(nokia, Resolution::Day).size() == 92 && s->queue(nokia, Resolution::Day).back().time == kLast);
  CHECK(held->queue(nokia, Resolution::Day).empty() && !held->queueResident(nokia, Resolution::Day));
  // The same as was stored.
  CHECK(summary::sameBar(s->queue(nokia, Resolution::Day)[5], summary::normalized(QueueKind::Candle, days(92)[5])));
  // Wanted again: no second read, and no new version.
  store->want({SeriesKey{nokia, Resolution::Day}});
  store->want({SeriesKey{nokia, Resolution::Day}});
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(d.counts.reads == 1 && store->snapshot()->version() == s->version());
  // Nothing was written back.
  store->flush();
  CHECK(d.counts.applies == 0 && d.counts.queues_put == 0 && d.counts.edges_put == 0);
  // A trace is there for the period.
  const PeriodView view = periodView(*store->snapshot(), nokia, Period::Quarter, true);
  CHECK(view.primary && !view.closes.empty());
  // A bar a source writes now is put into it, and then the disk has it too.
  store->appendCandles(nokia, Resolution::Day, {Candle{kLast + 1, 1, 2, 0.5, 1.5, 3}});
  store->flush();
  CHECK(d.counts.applies == 1);
  CHECK(store->snapshot()->queue(nokia, Resolution::Day).back().time == kLast + 1);
}

TEST(a_bar_said_for_a_queue_that_is_on_the_disk_only_is_merged_and_wins_over_what_is_read) {
  Disk d;
  EdgeKey nokia;
  {
    auto store = makeMemoryStore(d.options());
    Things t(*store);
    nokia = t.edge(t.nokia);
    store->appendCandles(nokia, Resolution::Day, days(92));
    store->flush();
  }
  {
    auto store = makeMemoryStore(d.options());
    Things t(*store);
    // Said before the queue is read: it is not in memory, so the bar is kept beside it and merged on the disk without a read.
    store->appendCandles(nokia, Resolution::Day, {Candle{kLast, 1, 2, 0.5, 777, 3}, Candle{kLast + 1, 1, 2, 0.5, 888, 3}});
    CHECK(!store->snapshot()->queueResident(nokia, Resolution::Day) && store->snapshot()->coverage(nokia, Resolution::Day)->last_key == kLast + 1);
    store->flush();
    CHECK(d.counts.reads == 0);
    store->want({SeriesKey{nokia, Resolution::Day}});
    CHECK(waitFor([&] { return store->snapshot()->queueResident(nokia, Resolution::Day); }));
    const auto q = store->snapshot()->queue(nokia, Resolution::Day);
    CHECK(q.size() == 93 && q.back().close == 888 && q[91].close == 777);
  }
  // And the disk has it.
  auto store = makeMemoryStore(d.options());
  store->want({SeriesKey{nokia, Resolution::Day}});
  CHECK(waitFor([&] { return store->snapshot()->queueResident(nokia, Resolution::Day); }));
  const auto q = store->snapshot()->queue(nokia, Resolution::Day);
  CHECK(q.size() == 93 && q.back().close == 888 && q[91].close == 777 && q[91].time == kLast);
}

TEST(over_its_budget_the_queues_not_wanted_for_the_longest_go_and_the_quote_stays_and_a_queue_wanted_again_is_read_again) {
  Disk d;
  std::vector<EdgeKey> edges;
  {
    auto store = makeMemoryStore(d.options());
    Things t(*store);
    for (int i = 0; i < 6; i++) {
      const InstrumentId base = store->addInstrument("", "Co " + std::to_string(i), InstrumentKind::Company, testIsin(100 + i), "STOCK");
      edges.push_back(EdgeKey{base, t.eur, t.hel, t.ds});
      store->appendCandles(edges.back(), Resolution::Day, days(50));
      store->setQuote(edges.back(), {1.0 + i, 0.9 + i, 1.1 + i, midnight(kLast) + 3600});
    }
    store->flush();
  }
  // Room for 120 bars: two queues of fifty.
  auto store = makeMemoryStore(d.options(120 * sizeof(Candle)));
  d.counts.reset();
  const auto wantOne = [&](size_t i) {
    store->want({SeriesKey{edges[i], Resolution::Day}});
    CHECK(waitFor([&] { return store->snapshot()->queueResident(edges[i], Resolution::Day); }));
  };
  wantOne(0);
  wantOne(1);
  CHECK(store->snapshot()->queueResident(edges[0], Resolution::Day) && store->snapshot()->queueResident(edges[1], Resolution::Day));
  const auto held = store->snapshot();
  wantOne(2);  // over the budget: the one wanted the longest ago goes (the one on screen now, never)
  CHECK(waitFor([&] { return !store->snapshot()->queueResident(edges[0], Resolution::Day); }));
  const auto s = store->snapshot();
  CHECK(s->queueResident(edges[1], Resolution::Day) && s->queueResident(edges[2], Resolution::Day));
  CHECK(s->queueBytes() <= 120 * sizeof(Candle));
  // The quote of the dropped one stays, and so does what the file says of its queue; a snapshot held keeps the bars it had.
  CHECK(s->quote(edges[0])->last == 1.0 && s->quote(edges[0])->bid == 0.9);
  CHECK(s->coverage(edges[0], Resolution::Day)->count == 50 && s->queue(edges[0], Resolution::Day).empty());
  CHECK(held->queue(edges[0], Resolution::Day).size() == 50);
  // Wanted again, it is read again.
  const int reads = d.counts.reads;
  wantOne(0);
  CHECK(d.counts.reads == reads + 1 && store->snapshot()->queue(edges[0], Resolution::Day).size() == 50);
  // What is wanted now is not dropped, even over the budget: three queues of fifty are wanted at once.
  store->want({SeriesKey{edges[3], Resolution::Day}, SeriesKey{edges[4], Resolution::Day}, SeriesKey{edges[5], Resolution::Day}});
  CHECK(waitFor([&] {
    const auto n = store->snapshot();
    return n->queueResident(edges[3], Resolution::Day) && n->queueResident(edges[4], Resolution::Day) && n->queueResident(edges[5], Resolution::Day);
  }));
  const auto now = store->snapshot();
  CHECK(now->queueResident(edges[3], Resolution::Day) && now->queueResident(edges[4], Resolution::Day) && now->queueResident(edges[5], Resolution::Day));
  // Nothing was written back by all that.
  store->flush();
  CHECK(d.counts.applies == 0);
}

TEST(a_damaged_file_another_schema_or_a_directory_that_cannot_be_made_say_so_and_the_store_runs_without) {
  Disk d;
  fs::create_directories(d.dir / "summaries");
  { std::ofstream(d.dir / "summaries" / "nordnet.sqlite") << std::string(4000, 'x'); }
  {
    auto store = makeMemoryStore(d.options());
    Things t(*store);
    const EdgeKey k = t.edge(t.nokia);
    store->appendCandles(k, Resolution::Day, days(5));
    store->flush();
    CHECK(d.said("moved aside"));
    CHECK(store->snapshot()->quote(k)->last == 104.5);
  }
  // What was written after the damaged file was moved aside is there.
  {
    auto store = makeMemoryStore(d.options());
    CHECK(store->snapshot()->quoteRowCount() == 1);
  }
  // A path that is no directory: the program runs on, and says so.
  Disk e;
  { std::ofstream(e.dir / "in-the-way") << "x"; }
  StoreOptions o;
  o.summaries = openSqliteSummaries(e.dir / "in-the-way" / "summaries", [&](const std::string & t) { e.say(t); });
  auto store = makeMemoryStore(std::move(o));
  Things t(*store);
  const EdgeKey k = t.edge(t.nokia);
  store->appendCandles(k, Resolution::Day, days(5));
  store->flush();
  CHECK(e.said("not kept") || e.said("cannot"));
  CHECK(store->snapshot()->quote(k)->last == 104.5 && store->snapshot()->queue(k, Resolution::Day).size() == 5);
  store->want({SeriesKey{k, Resolution::Day}});  // nothing to read, and nothing breaks
}

// ---- The history loader asks the summary before the network

namespace {

// An answer of Nordnet's price time series: `n` daily points ending on kLast, or the 5-minute bars of that day.
std::string answer(const std::string & resolution, int n) {
  std::string body = "{\"returnedResolution\":\"" + resolution + "\",\"pricePoints\":[";
  for (int i = 0; i < n; i++) {
    const int64_t ms = resolution == "DAY" ? (midnight(kLast - n + 1 + i) + 3600) * 1000 - 3600 * 1000 : (midnight(kLast) + 8 * 3600 + i * 300) * 1000;
    if (i) body += ",";
    body += "{\"timeStamp\":" + std::to_string(ms) + ",\"open\":1,\"high\":2,\"low\":0.5,\"last\":" + std::to_string(1 + i) + ",\"volume\":10}";
  }
  return body + "]}";
}

}  // namespace

TEST(the_loader_asks_the_summary_before_the_network_and_only_a_missing_or_expired_queue_costs_a_request) {
  using history_test::borrow;
  using history_test::quickly;
  Disk d;
  EdgeKey a, b;
  const auto makeWorld = [&](Store & store) {
    Things t(store);
    a = t.edge(t.nokia);
    b = t.edge(t.elisa);
    return t;
  };
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("MIN_5") != std::string::npos) return ok(answer("MIN_5", 12));
    return ok(answer("DAY", 95));
  };
  {
    auto store = std::shared_ptr<Store>(makeMemoryStore(d.options()));
    makeWorld(*store);
    for (const EdgeKey & k : {a, b}) store->setListing({k, "SYM", "", "book-" + std::to_string(k.base.value), "STOCK", "Name"});
    store->setQuote(b, {5, 4.9, 5.1, midnight(kLast) + 3600});  // b has a price and no queue
    HistoryLoader loader(store, borrow(http), quickly(), [] {});
    loader.addProvider(std::make_shared<NordnetHistory>());
    loader.want({a}, Period::Quarter);  // a's daily queue is fetched
    loader.waitIdle();
    loader.want({a}, Period::Day);  // and its day
    loader.waitIdle();
    CHECK(http.urls.size() == 2);
    CHECK(store->snapshot()->queue(a, Resolution::Day).size() > 80 && store->snapshot()->queue(a, Resolution::Min5).size() == 12);
    store->flush();
  }
  http.urls.clear();
  // A new session, a quarter of an hour of the fetch later: the queues are on the disk and nothing is in memory.
  const TimestampMs fetched = d.now;
  const auto session = [&](TimestampMs at, const std::function<void(HistoryLoader &, Store &)> & body) {
    auto store = std::shared_ptr<Store>(makeMemoryStore(d.options()));
    HistoryLoader::Options o = quickly();
    o.clock = [at] { return at; };
    HistoryLoader loader(store, borrow(http), o, [] {});
    loader.addProvider(std::make_shared<NordnetHistory>());
    body(loader, *store);
    loader.waitIdle();
  };
  session(fetched + 60'000, [&](HistoryLoader & loader, Store & store) {
    CHECK(!store.snapshot()->queueResident(a, Resolution::Day));
    loader.want({a}, Period::Quarter);  // the quarter never expires
    loader.want({a}, Period::Day);      // the day was fetched a minute ago, and is good for 15
  });
  CHECK(http.urls.empty());  // no request: the sparkline needs no data
  session(fetched + 20 * 60'000, [&](HistoryLoader & loader, Store &) { loader.want({a}, Period::Day); });  // expired
  CHECK(http.urls.size() == 1 && http.urls[0].find("DAY_1") != std::string::npos);
  http.urls.clear();
  session(fetched + 60'000, [&](HistoryLoader & loader, Store &) { loader.want({b}, Period::Quarter); });  // never fetched: missing
  CHECK(http.urls.size() == 1 && http.urls[0].find("MONTH_3") != std::string::npos);
}
