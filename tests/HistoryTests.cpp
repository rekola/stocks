#include "HistoryWorld.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

// The history loader: it queues what is wanted, paces, retries and remembers, and names no provider. These tests give it a fake one;
// Nordnet's own is in NordnetHistoryTests.cpp.
using namespace history_test;

namespace {

// A provider of the dataset "other" that does what the test says, and keeps what it was asked.
struct FakeProvider : HistoryProvider {
  using Behaviour = std::function<Outcome(Http &, const EdgeKey &, int load)>;  // `load`: how many loads came before this one
  std::string code = "other";
  Behaviour behave = [](Http &, const EdgeKey &, int) { return Outcome::Ok; };
  std::chrono::milliseconds life{0}, wait{0};
  std::atomic<int> loads{0};
  mutable std::mutex mutex;
  std::vector<EdgeKey> order;  // the edges, in the order they were loaded
  std::atomic<bool> stored{false};

  std::string dataset() const override { return code; }
  std::optional<Need> need(const Snapshot &, const EdgeKey &, Period) const override { return Need{0, stored}; }
  std::chrono::milliseconds lifetime(int) const override { return life; }
  std::chrono::milliseconds pause() const override { return wait; }
  Outcome load(Store &, Http & http, const EdgeKey & edge, int, const Wait &) override {
    {
      std::lock_guard lock(mutex);
      order.push_back(edge);
    }
    return behave(http, edge, loads++);
  }
};

// A request the provider makes, so that the test sees it in the FakeHttp.
HttpResult askFor(Http & http, const EdgeKey & edge) { return http.get("fake://" + std::to_string(edge.base.value)); }

std::shared_ptr<FakeProvider> provider(HistoryLoader & loader) {
  auto p = std::make_shared<FakeProvider>();
  loader.addProvider(p);
  return p;
}

}  // namespace

TEST(wanting_an_edge_loads_it_once_through_its_datasets_provider_and_not_again) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Fund, "", /*mainData=*/false);
  FakeHttp http;
  std::atomic<int> changes{0};
  HistoryLoader loader(w.store, borrow(http), quickly(), [&] { changes++; });
  auto p = provider(loader);
  loader.want({k});
  loader.waitIdle();
  CHECK(p->loads == 1 && changes >= 1);
  loader.want({k});
  loader.waitIdle();
  CHECK(p->loads == 1);
}

TEST(an_edge_of_a_dataset_with_no_provider_or_that_its_provider_cannot_load_is_left_out) {
  World w;
  EdgeKey unknown = w.edge(InstrumentKind::Company, "book-1");  // a dataset with no provider here
  EdgeKey other = w.edge(InstrumentKind::Fund, "", /*mainData=*/false);
  struct Picky : FakeProvider {
    std::optional<Need> need(const Snapshot &, const EdgeKey &, Period) const override { return std::nullopt; }
  };
  FakeHttp http;
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  auto picky = std::make_shared<Picky>();
  loader.addProvider(picky);
  loader.want({unknown, other});
  loader.waitIdle();
  CHECK(picky->loads == 0);
}

TEST(an_edge_the_store_has_the_bars_for_is_not_asked_for_unless_it_was_loaded_and_has_expired) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Fund, "", /*mainData=*/false);
  FakeHttp http;
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  auto p = provider(loader);
  p->stored = true;
  loader.want({k});
  loader.waitIdle();
  CHECK(p->loads == 0);
}

TEST(a_missing_edge_is_remembered_and_a_refusal_stops_that_provider_and_not_the_others) {
  {
    World w;
    EdgeKey k = w.edge(InstrumentKind::Fund, "", false);
    FakeHttp http;
    HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
    auto p = provider(loader);
    p->behave = [](Http &, const EdgeKey &, int) { return HistoryProvider::Outcome::Missing; };
    loader.want({k});
    loader.waitIdle();
    loader.want({k});
    loader.waitIdle();
    CHECK(p->loads == 1);  // not asked for again
  }
  World w;
  EdgeKey a = w.edge(InstrumentKind::Fund, "", false), b = w.edge(InstrumentKind::Fund, "", false);
  FakeHttp http;
  std::vector<std::string> said;
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {}, [&](std::string m) { said.push_back(m); });
  auto refusing = provider(loader);
  refusing->behave = [](Http &, const EdgeKey &, int) { return HistoryProvider::Outcome::Refused; };
  auto fine = std::make_shared<FakeProvider>();
  fine->code = "third";
  loader.addProvider(fine);
  EdgeKey c{w.edge(InstrumentKind::Fund, "", false).base, w.usd, w.market, w.store->addDataset("third")};
  w.store->setListing({c, "SYM", "", "", "ESH", "Instrument"});
  loader.want({a, b, c});
  loader.waitIdle();
  CHECK(loader.refused() && refusing->loads == 1);  // the second edge is not tried
  CHECK(fine->loads == 1);                          // and the other provider goes on
  CHECK(said.size() == 1);
  loader.want({b});
  loader.waitIdle();
  CHECK(refusing->loads == 1);
}

TEST(no_answer_pauses_and_the_edge_is_asked_for_again) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Fund, "", false);
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(0); };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  auto p = provider(loader);
  p->behave = [](Http & h, const EdgeKey & e, int n) {
    askFor(h, e);
    return n == 0 ? HistoryProvider::Outcome::Offline : HistoryProvider::Outcome::Ok;
  };
  loader.want({k});
  for (int i = 0; i < 400 && p->loads < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  loader.waitIdle();
  CHECK(p->loads == 2);  // the first got no answer, the second was after the pause
  CHECK(http.urls.size() == 2);
}

TEST(edges_are_loaded_in_the_order_they_are_wanted) {
  World w;
  EdgeKey a = w.edge(InstrumentKind::Fund, "", false), b = w.edge(InstrumentKind::Fund, "", false), c = w.edge(InstrumentKind::Fund, "", false);
  FakeHttp http;
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  auto p = provider(loader);
  loader.want({c, a, b});
  loader.waitIdle();
  CHECK(p->order.size() == 3);
  CHECK(p->order[0] == c && p->order[1] == a && p->order[2] == b);
}

TEST(a_wanted_edge_is_requested_again_once_its_request_has_expired_and_not_before) {
  World w;
  EdgeKey a = w.edge(InstrumentKind::Fund, "", false), b = w.edge(InstrumentKind::Fund, "", false);
  FakeHttp http;
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  auto p = provider(loader);
  p->life = std::chrono::milliseconds(150);
  loader.want({a, b});
  loader.waitIdle();
  CHECK(p->loads == 2);
  loader.want({a, b});
  loader.waitIdle();
  CHECK(p->loads == 2);  // the request is young
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  loader.want({a});  // b is not wanted: it is not asked for, whatever its age
  loader.waitIdle();
  CHECK(p->loads == 3 && p->order[2] == a);
  loader.want({a});
  loader.waitIdle();
  CHECK(p->loads == 3);  // the time starts again from the new load
  loader.want({b});
  loader.waitIdle();
  CHECK(p->loads == 4 && p->order[3] == b);
}

TEST(an_expired_request_queued_again_by_want_is_fetched_and_not_dropped_as_done) {
  World w;
  EdgeKey a = w.edge(InstrumentKind::Fund, "", false);
  FakeHttp http;
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  auto p = provider(loader);
  p->life = std::chrono::milliseconds(30);
  for (int round = 1; round <= 3; round++) {
    loader.want({a});  // expired since the last round: queued, popped by run() (which finds it in done_) and loaded
    loader.waitIdle();
    CHECK(p->loads == round);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
  }
}

TEST(a_request_with_no_lifetime_never_expires) {
  World w;
  EdgeKey a = w.edge(InstrumentKind::Fund, "", false);
  FakeHttp http;
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  auto p = provider(loader);
  loader.want({a});
  loader.waitIdle();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  loader.want({a});
  loader.waitIdle();
  CHECK(p->loads == 1);
}

TEST(a_dataset_is_found_by_its_code_and_not_by_a_name_that_can_change) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Fund, "", false);
  w.store->addDataset("other", "Another, as a person calls it");  // the name is a fact, and it was changed
  CHECK(w.store->snapshot()->name(k.dataset) == "Another, as a person calls it");
  FakeHttp http;
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  auto p = provider(loader);
  loader.want({k});
  loader.waitIdle();
  CHECK(p->loads == 1);  // loaded, as before the name changed
}

namespace {

// A gate the loader's thread waits at, in the middle of a request, until the test opens it.
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool arrived = false, open = false;
  void wait() {  // on the loader's thread
    std::unique_lock lock(mutex);
    arrived = true;
    cv.notify_all();
    cv.wait(lock, [&] { return open; });
  }
  void awaitArrival() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return arrived; });
  }
  void release() {
    std::lock_guard lock(mutex);
    open = true;
    cv.notify_all();
  }
};

}  // namespace

TEST(an_edge_wanted_again_while_its_request_is_in_flight_is_asked_for_once) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Fund, "", false);
  FakeHttp http;
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  auto p = provider(loader);
  Gate gate;
  p->behave = [&](Http &, const EdgeKey &, int n) {
    if (n == 0) gate.wait();
    return HistoryProvider::Outcome::Ok;
  };
  loader.want({k});
  gate.awaitArrival();
  // The rows on screen change while the first request is out: the edge is wanted again, with its bars not in the store yet.
  loader.want({k});
  loader.want({k});
  gate.release();
  loader.waitIdle();
  CHECK(p->loads == 1);
  loader.want({k});
  loader.waitIdle();
  CHECK(p->loads == 1);
}

TEST(a_provider_that_throws_counts_as_missing_and_the_queue_goes_on) {
  World w;
  EdgeKey a = w.edge(InstrumentKind::Fund, "", false);
  EdgeKey b = w.edge(InstrumentKind::Fund, "", false);
  FakeHttp http;
  std::mutex mutex;
  std::vector<std::string> messages;
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {}, [&](std::string m) {
    std::lock_guard lock(mutex);
    messages.push_back(std::move(m));
  });
  auto p = provider(loader);
  p->behave = [](Http &, const EdgeKey &, int n) -> HistoryProvider::Outcome {
    if (n == 0) throw std::runtime_error("broken answer");
    return HistoryProvider::Outcome::Ok;
  };
  loader.want({a, b});
  loader.waitIdle();
  CHECK(p->loads == 2);  // the second edge was loaded after the first threw
  loader.want({a, b});
  loader.waitIdle();
  CHECK(p->loads == 2);  // the first is not asked again, nor the second, which is done
  std::lock_guard lock(mutex);
  CHECK(messages.size() == 1 && messages[0].find("broken answer") != std::string::npos);
}

TEST(the_loader_stopping_cancels_the_request_in_progress) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Fund, "", false);
  // A request that gets no answer until the Http is cancelled (for a minute at most).
  struct Waiting : Http {
    std::atomic<bool> & entered;
    explicit Waiting(std::atomic<bool> & e) : entered(e) {}
    HttpResult get(const std::string &, const HttpHeaders &) override {
      entered = true;
      for (int i = 0; i < 6000 && !cancelled(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      return failed(0);
    }
  };
  std::atomic<bool> entered{false};
  const auto start = std::chrono::steady_clock::now();
  {
    HistoryLoader loader(w.store, std::make_unique<Waiting>(entered), quickly(), [] {});
    auto p = provider(loader);
    p->behave = [](Http & h, const EdgeKey & e, int) {
      askFor(h, e);
      return HistoryProvider::Outcome::Offline;
    };
    loader.want({k});
    for (int i = 0; i < 1000 && !entered; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(entered);
  }
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}

TEST(a_providers_pause_is_not_kept_for_the_asks_of_another_after_it) {
  World w;
  EdgeKey provided = w.edge(InstrumentKind::Fund, "", false);
  EdgeKey a = w.edge(InstrumentKind::Fund, "", false), b = w.edge(InstrumentKind::Fund, "", false);
  // `provided` is in the dataset "slow", whose provider waits 400 ms after each request; a and b are in "other".
  EdgeKey slowEdge{provided.base, w.usd, w.market, w.store->addDataset("slow")};
  w.store->setListing({slowEdge, "SYM", "", "", "ESH", "Instrument"});
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(""); };
  std::vector<std::chrono::steady_clock::time_point> times;
  HistoryLoader::Options options = quickly();
  options.pause = std::chrono::milliseconds(10);
  HistoryLoader loader(w.store, borrow(http), options, [] {});
  auto slow = std::make_shared<FakeProvider>();
  slow->code = "slow";
  slow->wait = std::chrono::milliseconds(400);
  loader.addProvider(slow);
  auto p = provider(loader);
  p->behave = [&](Http & h, const EdgeKey & e, int) {
    times.push_back(std::chrono::steady_clock::now());
    askFor(h, e);
    return HistoryProvider::Outcome::Ok;
  };
  loader.want({slowEdge, a, b});
  loader.waitIdle();
  CHECK(slow->loads == 1 && times.size() == 2);
  // The second ask waited the loader's own pause after the first, not the other provider's.
  CHECK(times.size() == 2 && times[1] - times[0] < std::chrono::milliseconds(300));
}

TEST(a_provider_waits_the_loaders_pause_between_the_requests_of_one_load_and_is_told_when_the_loader_stops) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Fund, "", false);
  FakeHttp http;
  std::atomic<bool> told{false}, inWait{false};
  {
    HistoryLoader::Options options = quickly();
    options.pause = std::chrono::minutes(10);  // only a stop ends it
    HistoryLoader loader(w.store, borrow(http), options, [] {});
    struct Waits : FakeProvider {
      std::atomic<bool> * told = nullptr;
      std::atomic<bool> * inWait = nullptr;
      Outcome load(Store &, Http &, const EdgeKey &, int, const Wait & wait) override {
        *inWait = true;
        *told = !wait();
        return Outcome::Ok;
      }
    };
    auto waits = std::make_shared<Waits>();
    waits->told = &told;
    waits->inWait = &inWait;
    loader.addProvider(waits);
    loader.want({k});
    for (int i = 0; i < 1000 && !inWait; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(inWait);
  }
  CHECK(told);  // the destructor cut the wait short
}
