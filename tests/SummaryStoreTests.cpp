#include "AttributeView.h"
#include "HistoryWorld.h"
#include "Quotes.h"
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
    o.summary_writer.delay = o.summary_writer.queue_delay = std::chrono::milliseconds(1);
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
  // The table's rows are the same: every edge, with its price and its name, and no trace until the queue is read.
  const std::vector<QuoteRow> rows = makeAllRows(*s, Period::Quarter);
  CHECK(rows.size() == 3);
  for (const QuoteRow & row : rows)
    if (row.key == nokia) CHECK(row.price == 5.5 && row.name == "Nokia Oyj" && row.history.empty() && !isDefined(row.change));
}

TEST(the_info_of_an_edge_shows_its_summary_and_where_each_queue_is) {
  Disk d;
  EdgeKey nokia;
  {
    auto store = makeMemoryStore(d.options());
    Things t(*store);
    nokia = t.edge(t.nokia);
    store->appendCandles(nokia, Resolution::Day, days(30));
    store->setQuote(nokia, {5.5, 5.4, 5.6, midnight(kLast) + 50000});
    store->flush();
  }
  auto store = makeMemoryStore(d.options());
  const auto text = [&] {
    std::string out;
    for (const InfoSection & section : buildInfo(*store->snapshot(), nokia))
      if (section.title == "Summary")
        for (const InfoRow & r : section.rows) out += r.predicate + "=" + r.value.text + "\n";
    return out;
  };
  const std::string cold = text();
  CHECK(cold.find("last=5.5") != std::string::npos && cold.find("bid=5.4") != std::string::npos && cold.find("ask=5.6") != std::string::npos);
  CHECK(cold.find("queue 1d=30 bars") != std::string::npos && cold.find("on the disk") != std::string::npos);
  store->want({SeriesKey{nokia, Resolution::Day}});
  CHECK(waitFor([&] { return store->snapshot()->queueResident(nokia, Resolution::Day); }));
  CHECK(text().find("in memory") != std::string::npos);
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

// ---- The restore, off the thread that draws

namespace {

// A facts file that is slow to read: the first `allowed` calls to read some nodes go through and the next waits, so that a test looks at the
// store while the restore is in the middle of it.
class SlowFacts : public FactStore {
 public:
  explicit SlowFacts(std::shared_ptr<FactStore> inner) : inner_(std::move(inner)) {}
  std::vector<StoredFact> loadAll() override { return inner_->loadAll(); }
  std::vector<StoredFact> loadNodes(const std::vector<uint64_t> & nodes) override {
    std::unique_lock lock(mutex_);
    const int index = static_cast<int>(calls_.size());  // the calls that pass are the first `allowed_`, whoever made them
    calls_.push_back(nodes);
    changed_.notify_all();
    changed_.wait(lock, [&] { return index < allowed_; });
    lock.unlock();
    return inner_->loadNodes(nodes);
  }
  std::vector<StoredEvent> loadEvents() override { return inner_->loadEvents(); }
  void apply(const FactBatch & b) override { inner_->apply(b); }
  void allow(int n) {
    {
      std::lock_guard lock(mutex_);
      allowed_ = n;
    }
    changed_.notify_all();
  }
  // Waits until the restore is waiting in its `n`th read.
  bool waitForCall(size_t n) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5), [&] { return calls_.size() >= n; });
  }
  std::vector<std::vector<uint64_t>> calls() {
    std::lock_guard lock(mutex_);
    return calls_;
  }

 private:
  std::shared_ptr<FactStore> inner_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::vector<uint64_t>> calls_;
  int allowed_ = 0;
};

constexpr size_t kMany = 2500;  // more edges than a chunk of the restore

struct Big {
  EdgeKey held;                      // Nokia, which the portfolio holds
  std::vector<EdgeKey> others;       // the rest
  InstrumentId orphan;               // facts and no edge
  PortfolioId mine{portfolioKey(0x1234567890abcdull)->value};
};

// A disk with a portfolio holding Nokia, `kMany` other edges with a price and a daily queue, and an instrument that has no edge.
Big populate(Disk & d) {
  Big big;
  auto store = makeMemoryStore(d.options());
  Things t(*store);
  big.held = t.edge(t.nokia);
  store->batch([&](Batch & b) {
    b.appendCandles(big.held, Resolution::Day, days(92));
    b.setQuote(big.held, {5.5, 5.4, 5.6, midnight(kLast) + 50000});
    b.setListing({big.held, "NOKIA", "id-n", "book-n", "STOCK", "Nokia Oyj"});
    for (size_t i = 0; i < kMany; i++) {
      const InstrumentId base = b.addInstrument("S" + std::to_string(i), "Company " + std::to_string(i), InstrumentKind::Company, testIsin(1000 + static_cast<int>(i)), "STOCK");
      const EdgeKey k{base, t.eur, t.hel, t.ds};
      big.others.push_back(k);
      b.appendCandles(k, Resolution::Day, days(30));
      b.setQuote(k, {10.0 + static_cast<double>(i), 9.5, 10.5, midnight(kLast) + 40000});
      b.setListing({k, "S" + std::to_string(i), "", "book", "STOCK", "Company " + std::to_string(i)});
    }
    big.orphan = b.addInstrument("", "Orphan", InstrumentKind::Company, testIsin(7777), "STOCK");
  });
  const portfolio::Transaction buy{1, kLast - 3, portfolio::Side::Buy, t.nokia, t.hel, t.eur, 10, 4, 1, "x"};
  store->setAttributes(NodeRef(big.mine), {{predicate::name, "Mine"}, {predicate::currency, NodeKey{t.eur.value}}});
  store->appendTransactions({big.mine, store->addDataset(kUserDataset)}, {buy});
  store->flush();
  return big;
}

}  // namespace

TEST(the_ui_works_while_the_files_are_read_and_what_the_portfolios_hold_comes_first) {
  Disk d;
  const Big big = populate(d);
  d.now += 3'600'000;  // an hour later
  StoreOptions o = d.options();
  auto slow = std::make_shared<SlowFacts>(o.facts);
  o.facts = slow;
  o.restore = StoreOptions::Restore::InBackground;
  std::atomic<int> changes{0};
  o.changed = [&] { changes++; };
  const auto start = std::chrono::steady_clock::now();
  auto store = makeMemoryStore(std::move(o));
  // The store is made at once, whatever the file takes, and works: the portfolios (the user's own small file) are there, the rest is coming.
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000));
  CHECK(store->restoring());
  CHECK(slow->waitForCall(1));
  CHECK(store->snapshot()->portfolios() == std::vector<PortfolioId>{big.mine} && store->snapshot()->quoteRowCount() == 0);
  // A source writes while the restore waits: it does not wait for it.
  const auto written = std::chrono::steady_clock::now();
  const InstrumentId fresh = store->addInstrument("NEW", "New", InstrumentKind::Company, testIsin(31337), "STOCK");
  const InstrumentId eur = store->addInstrument("EUR", "Euro", InstrumentKind::Currency);
  const EdgeKey freshEdge{fresh, eur, store->addMarket("XHEL", "Nasdaq Helsinki"), store->addDataset("nordnet", "Nordnet")};
  store->appendCandles(freshEdge, Resolution::Day, days(3));
  CHECK(std::chrono::steady_clock::now() - written < std::chrono::milliseconds(500));
  CHECK(store->restoring());
  // The first read is what the portfolio holds: its instrument's facts, its listing and its quote, and then the restore waits at the next.
  slow->allow(1);
  CHECK(waitFor([&] { return store->snapshot()->listing(big.held).has_value(); }));
  CHECK(slow->waitForCall(2));
  {
    const auto s = store->snapshot();
    CHECK(store->restoring());
    CHECK(s->name(NodeRef(big.held.base)) == "Nokia Oyj" && s->kind(big.held.base) == InstrumentKind::Company);
    CHECK(s->quote(big.held) && s->quote(big.held)->last == 5.5 && s->quote(big.held)->bid == 5.4);
    CHECK(s->listing(big.held)->symbol == "NOKIA");
    CHECK(s->coverage(big.held, Resolution::Day)->count == 92);
    // And not yet the rest: not their facts, not their rows.
    CHECK(!s->known(NodeRef(big.others[10].base)) && !s->quote(big.others[10]));
    CHECK(s->listed(big.held) && s->listed(freshEdge) && s->quoteRowCount() == 2);  // the held one, and the new one a source wrote
    CHECK(changes >= 1);
    // The held read was the first, and it asked for Nokia and what it is priced and listed in; the next asks for others.
    CHECK(slow->waitForCall(3));
    const auto calls = slow->calls();
    CHECK(calls.size() >= 3);  // (the edge a source made meanwhile has its own read, on the reader's thread)
    const auto has = [&](const std::vector<uint64_t> & nodes, uint64_t n) { return std::find(nodes.begin(), nodes.end(), n) != nodes.end(); };
    CHECK(has(calls[0], big.held.base.value) && has(calls[0], big.held.quote.value) && has(calls[0], big.held.market.value));
    CHECK(!has(calls[0], big.others[10].base.value));
    bool later = false;
    for (size_t i = 1; i < calls.size(); i++) later = later || has(calls[i], big.others[10].base.value);
    CHECK(later);
  }
  slow->allow(100);
  store->waitRestored();
  CHECK(!store->restoring());
  const auto s = store->snapshot();
  CHECK(s->quoteRowCount() == kMany + 2);
  CHECK(s->name(NodeRef(big.others[kMany - 1].base)) == "Company " + std::to_string(kMany - 1));
  CHECK(s->quote(big.others[kMany - 1])->last == 10.0 + static_cast<double>(kMany - 1));
  // The tables find an edge by what its market calls it, as before.
  CHECK(s->findEdge("S7", *s->findMarket("XHEL")) == big.others[7]);
  // A portfolio is valued with no network and no queue read: from the quote.
  const auto held = portfolio::holdings(*s, big.mine);
  const portfolio::Valuation v = portfolio::value(held.positions, *s, *s->findInstrument("EUR"));
  CHECK(!v.incomplete && v.rows[0].price && v.rows[0].price->close == 5.5);
}

TEST(a_source_that_writes_while_the_files_are_read_wins_over_what_the_disk_has) {
  Disk d;
  const Big big = populate(d);
  d.now += 3'600'000;
  StoreOptions o = d.options();
  auto slow = std::make_shared<SlowFacts>(o.facts);
  o.facts = slow;
  o.restore = StoreOptions::Restore::InBackground;
  auto store = makeMemoryStore(std::move(o));
  CHECK(slow->waitForCall(1));
  slow->allow(1);
  CHECK(slow->waitForCall(2));  // the held chunk is in; the restore waits before the rest
  // A source says things of edges and instruments that the disk has an older word on, before the restore has read them.
  const EdgeKey a = big.others[100], b = big.others[200], c = big.others[300];
  store->setAttribute(NodeRef(a.base), {predicate::name, "Said while restoring"});
  const Timestamp newer = midnight(kLast) + 80000;  // the disk's is 40000 into the day
  store->setQuote(b, {999, 998, 1000, newer});
  store->appendCandles(c, Resolution::Day, {Candle{kLast + 1, 1, 2, 0.5, 777, 3}});
  slow->allow(100);
  store->waitRestored();
  const auto s = store->snapshot();
  // What the source said stands; the rest of what the disk had of the same node is added.
  CHECK(s->name(NodeRef(a.base)) == "Said while restoring");
  CHECK(s->said(NodeRef(a.base), predicate::type) == "STOCK" && s->kind(a.base) == InstrumentKind::Company);
  CHECK(s->findByAttribute(NodeType::Instrument, predicate::name, "Said while restoring") == NodeRef(a.base));
  CHECK(!s->findByAttribute(NodeType::Instrument, predicate::name, "Company 100"));  // the old word is not found
  // A quote newer than the disk's stands, the listing that was not said is the disk's, and the rest of the disk's is added.
  CHECK(s->quote(b)->last == 999 && s->quote(b)->bid == 998 && s->quote(b)->time == newer);
  CHECK(s->listing(b)->symbol == "S200");
  // A bar said for a queue the disk has too: both are in it once it is read.
  CHECK(s->coverage(c, Resolution::Day)->last_key == kLast + 1);
  store->want({SeriesKey{c, Resolution::Day}});
  CHECK(waitFor([&] { return store->snapshot()->queueResident(c, Resolution::Day); }));
  const auto q = store->snapshot()->queue(c, Resolution::Day);
  CHECK(q.size() == 31 && q.back().time == kLast + 1 && q.back().close == 777 && q[q.size() - 2].time == kLast);
  // And what the disk is told is what stands: a restart sees it.
  store->flush();
  auto again = makeMemoryStore(d.options());
  CHECK(again->snapshot()->quote(b)->last == 999);
  CHECK(again->snapshot()->name(NodeRef(a.base)) == "Said while restoring");
}

TEST(restoring_makes_no_node_that_has_no_edge_and_keeps_the_time_a_fact_had_on_the_disk) {
  Disk d;
  const Big big = populate(d);
  d.now += 3'600'000;
  StoreOptions o = d.options();
  o.restore = StoreOptions::Restore::InBackground;
  auto store = makeMemoryStore(std::move(o));
  store->waitRestored();
  const auto s = store->snapshot();
  // The instrument with facts and no edge is not made; those with an edge, and the instrument the portfolio holds, are.
  CHECK(!s->findInstrumentByIsin(testIsin(7777)) && !s->known(NodeRef(big.orphan)));
  CHECK(s->nodeCount(NodeType::Instrument) == kMany + 2);  // the edges' instruments and the euro
  CHECK(s->known(NodeRef(big.held.base)) && s->known(NodeRef(big.others[0].base)));
  // A fact is as old as the disk had it, not as new as this restart; and nothing was a new version of the facts.
  const auto name = s->fact(NodeRef(big.held.base), predicate::name);
  CHECK(name && name->updated == 1'800'000'000'000 && name->updated != d.now);
  // No facts are written back either: what was read is on the disk already.
  store->flush();
  CHECK(d.counts.queues_put >= kMany);  // (session one's)
  d.counts.reset();
  store->flush();
  CHECK(d.counts.applies == 0);
}

TEST(a_restore_in_the_background_ends_as_one_made_before_the_constructor_returns) {
  Disk d;
  const Big big = populate(d);
  StoreOptions now = d.options();
  auto sync = makeMemoryStore(std::move(now));
  StoreOptions later = d.options();
  later.restore = StoreOptions::Restore::InBackground;
  auto background = makeMemoryStore(std::move(later));
  background->waitRestored();
  const auto a = sync->snapshot(), b = background->snapshot();
  CHECK(!sync->restoring());
  CHECK(a->quoteRowCount() == b->quoteRowCount() && a->quoteRowCount() == kMany + 1);
  CHECK(a->nodeCount(NodeType::Instrument) == b->nodeCount(NodeType::Instrument));
  for (const EdgeKey & k : {big.held, big.others[5], big.others[kMany - 1]}) {
    CHECK(a->quote(k) == b->quote(k) && a->listing(k) == b->listing(k));
    CHECK(a->coverage(k, Resolution::Day)->count == b->coverage(k, Resolution::Day)->count);
  }
  // Queues are on the disk only in both.
  CHECK(!a->queueResident(big.held, Resolution::Day) && !b->queueResident(big.held, Resolution::Day));
}

// ---- A fetch that brings nothing new, the first refresh after a start, and the facts of an instrument that gets its first edge

TEST(a_fetch_that_brings_the_same_bars_moves_when_the_queue_was_fetched_with_no_version_and_a_small_write) {
  Disk d;
  EdgeKey k;
  {
    auto store = makeMemoryStore(d.options());
    Things t(*store);
    k = t.edge(t.nokia);
    store->appendCandles(k, Resolution::Day, days(30));
    store->flush();
  }
  const TimestampMs first = d.now;
  auto store = makeMemoryStore(d.options());
  // The queue is in memory (a fetch of many bars cannot be told from the file's until it is: only the last bar has a digest).
  store->want({SeriesKey{k, Resolution::Day}});
  CHECK(waitFor([&] { return store->snapshot()->queueResident(k, Resolution::Day); }));
  d.counts.reset();
  // Within the recheck interval (an hour): said again and the same changes nothing (the bars are new to the data, which a start does not
  // keep, so that one has a version; the summary has none and writes none).
  d.now = first + 600'000;
  store->appendCandles(k, Resolution::Day, days(30));
  store->flush();
  CHECK(store->snapshot()->coverage(k, Resolution::Day)->fetched == first && d.counts.applies == 0);
  const uint64_t version = store->snapshot()->version();
  // Later: a fetch that went through is when it was fetched, with no version, and it is kept (one small write, and the bars are not rewritten).
  d.now = first + 7'200'000;
  store->appendCandles(k, Resolution::Day, days(30));
  store->flush();
  CHECK(store->snapshot()->version() == version);
  CHECK(store->snapshot()->coverage(k, Resolution::Day)->fetched == d.now);
  CHECK(d.counts.applies == 1);
  {
    auto again = makeMemoryStore(d.options());
    CHECK(again->snapshot()->coverage(k, Resolution::Day)->fetched == d.now && again->snapshot()->coverage(k, Resolution::Day)->count == 30);
    again->want({SeriesKey{k, Resolution::Day}});
    CHECK(waitFor([&] { return again->snapshot()->queueResident(k, Resolution::Day); }));
    CHECK(again->snapshot()->queue(k, Resolution::Day).size() == 30);
  }
}

TEST(the_first_refresh_after_a_start_writes_no_queue_whose_last_bar_it_says_again_unchanged) {
  Disk d;
  std::vector<EdgeKey> edges;
  {
    auto store = makeMemoryStore(d.options());
    Things t(*store);
    for (int i = 0; i < 20; i++) {
      const InstrumentId base = store->addInstrument("", "Co " + std::to_string(i), InstrumentKind::Company, testIsin(200 + i), "STOCK");
      edges.push_back(EdgeKey{base, t.eur, t.hel, t.ds});
      store->appendCandles(edges.back(), Resolution::Day, days(40));
    }
    store->flush();
  }
  auto store = makeMemoryStore(d.options());
  d.counts.reset();
  // The list says today's bar of each, as it was: nothing is written. Of two, it has moved, and only those are.
  for (size_t i = 0; i < edges.size(); i++) {
    Candle today = days(40).back();
    if (i < 2) today.close += 1;
    store->appendCandles(edges[i], Resolution::Day, {today});
  }
  store->flush();
  CHECK(d.counts.queues_put == 2);
  // And it is merged, not lost: the two read back with the new close.
  store->want({SeriesKey{edges[0], Resolution::Day}, SeriesKey{edges[5], Resolution::Day}});
  CHECK(waitFor([&] { return store->snapshot()->queueResident(edges[0], Resolution::Day) && store->snapshot()->queueResident(edges[5], Resolution::Day); }));
  CHECK(store->snapshot()->queue(edges[0], Resolution::Day).back().close == days(40).back().close + 1 && store->snapshot()->queue(edges[0], Resolution::Day).size() == 40);
  CHECK(store->snapshot()->queue(edges[5], Resolution::Day).back().close == days(40).back().close);
}

TEST(an_instrument_that_gets_its_first_edge_has_the_facts_the_disk_kept_of_it_read_and_what_the_source_said_stands) {
  Disk d;
  const InstrumentId orphan{instrumentKey(testIsin(555))->value};
  {
    auto store = makeMemoryStore(d.options());
    Things t(*store);
    store->resolveInstrument({{predicate::isin, testIsin(555)}}, {{predicate::name, "Old name"}, {"description", "Fetched slowly in an earlier run"}, {predicate::type, "STOCK"}});
    store->appendCandles(t.edge(t.nokia), Resolution::Day, days(5));  // one edge, so that summaries exist
    store->flush();
  }
  d.now += 3'600'000;
  auto store = makeMemoryStore(d.options());
  // No edge, so not restored: nothing selects it.
  CHECK(!store->snapshot()->known(NodeRef(orphan)));
  // A source gives it an edge and says its name: what was kept of it is read, and the name said stands.
  Things t(*store);
  store->setAttribute(NodeRef(orphan), {predicate::name, "New name"});
  store->appendCandles(EdgeKey{orphan, t.eur, t.hel, t.ds}, Resolution::Day, days(5));
  CHECK(waitFor([&] { return store->snapshot()->attribute(NodeRef(orphan), "description").has_value(); }));
  const auto s = store->snapshot();
  CHECK(std::get<std::string>(*s->attribute(NodeRef(orphan), "description")) == "Fetched slowly in an earlier run");
  CHECK(s->name(NodeRef(orphan)) == "New name" && s->said(NodeRef(orphan), predicate::type) == "STOCK");
  CHECK(s->fact(NodeRef(orphan), "description")->updated == 1'800'000'000'000);  // as the disk had it
  CHECK(waitFor([&] { return !store->restoring(); }));
}
