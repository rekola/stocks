// httplib first: the model declares a global seconds(Resolution) that its own `seconds` would otherwise be ambiguous with.
#include <httplib.h>

#include "FakeHttp.h"
#include "MarketTestSupport.h"
#include "TestFramework.h"
#include "market/Api.h"
#include "market/Ingest.h"
#include "model/Store.h"
#include "source/MarketServerSource.h"

#include <cmath>
#include <set>
#include <thread>

// The application's client of the server (source/MarketServerSource.h), end to end: an ingest of HistData's fake site into the server's store,
// the server on a free port, and the client's source and history provider reading it over the application's real Http.
using histdata::HistData;

namespace {

const Timestamp kLast = 1794607080;  // 2026-11-13 21:58:00 UTC

struct Served {
  std::shared_ptr<Store> store;
  TempDir dir;
  FakeSite site;
  std::unique_ptr<market::ApiServer> server;
  std::thread thread;
  Served() {
    StoreOptions options;
    options.invent_isins = false;
    store = makeMemoryStore(std::move(options));
    site.lastBar = kLast;
    site.step = 120;
    HistData adapter(site, market::RawCache(dir.path), fastSettings());
    market::IngestOptions ingest;
    ingest.codes = {"eurusd", "xauusd", "spxusd"};
    market::runIngest(adapter, *store, ingest, [](const std::string &) {});
    market::loadQueues(*store);
    server = std::make_unique<market::ApiServer>([this] { return store; });
    std::string error;
    CHECK(server->bind("127.0.0.1", 0, error));
    thread = std::thread([this] { server->serve(); });
  }
  ~Served() {
    server->stop();
    thread.join();
  }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(server->port()); }
};

bool sameBar(const Candle & a, const Candle & b) {
  const auto same = [](double x, double y) { return x == y || (!isDefined(x) && !isDefined(y)); };
  return a.time == b.time && same(a.open, b.open) && same(a.high, b.high) && same(a.low, b.low) && same(a.close, b.close) && same(a.volume, b.volume);
}

}  // namespace

TEST(the_client_reads_the_servers_rows_and_a_batch_of_bars_over_real_http) {
  Served served;
  const auto theirs = served.store->snapshot();
  const std::vector<EdgeKey> serverEdges = theirs->quoteRowsSince(0);
  CHECK(serverEdges.size() == 3);

  // The rows: the edges the server made, from /instruments alone, each under HistData's code and name.
  StoreOptions options;
  options.invent_isins = false;
  auto client = makeMemoryStore(std::move(options));
  auto http = makeHttp();
  MarketServerSource::Options source_options;
  source_options.url = served.url();
  MarketServerSource source(source_options);
  Listener listener;
  source.load(*client, *http, listener);
  CHECK(listener.messages.empty());
  const auto mine = client->snapshot();
  std::vector<EdgeKey> edges = mine->quoteRowsSince(0);
  CHECK(edges.size() == serverEdges.size());
  std::set<std::string> ours, their;
  for (const EdgeKey & e : edges) ours.insert(marketServerEdgeId(e));
  for (const EdgeKey & e : serverEdges) their.insert(marketServerEdgeId(e));
  CHECK(ours == their);
  for (const EdgeKey & e : serverEdges) {
    CHECK(mine->listed(e));  // the very edge: the keys mean the same in both
    CHECK(mine->listing(e) == theirs->listing(e));
    CHECK(mine->name(e.dataset) == "HistData.com" && datasetCode(e.dataset) == "HISTDATA");
    CHECK(mine->kind(e.base) == theirs->kind(e.base) && mine->isin(e.base) == theirs->isin(e.base) && mine->name(e.base) == theirs->name(e.base));
    CHECK(mine->kind(e.quote) == theirs->kind(e.quote));
    CHECK(mine->queues(e).empty());
  }

  // One batch of every edge, the weekly bars: the same bars as the server's queue.
  const auto provider = source.historyProvider();
  const auto outcomes = provider->loadBatch(*client, *http, edges, MarketServerHistory::Weekly, [] { return true; });
  CHECK(outcomes == std::vector<HistoryProvider::Outcome>(edges.size(), HistoryProvider::Outcome::Ok));
  const auto after = client->snapshot();
  for (const EdgeKey & e : serverEdges) {
    const auto want = theirs->queue(e, Resolution::Week);
    const auto got = after->candles(e, Resolution::Week).candles;
    CHECK(want.size() > 1 && got.size() == want.size());
    for (size_t i = 0; i < want.size() && i < got.size(); i++) CHECK(sameBar(want[i], got[i]));
    CHECK(after->coverage(e, Resolution::Week)->kind == theirs->coverage(e, Resolution::Week)->kind);
  }
  // The day's request: 5-minute and daily bars in one.
  CHECK(provider->loadBatch(*client, *http, edges, MarketServerHistory::Intraday, [] { return true; }) ==
        std::vector<HistoryProvider::Outcome>(edges.size(), HistoryProvider::Outcome::Ok));
  CHECK(!client->snapshot()->candles(edges[0], Resolution::Min5).candles.empty() && !client->snapshot()->candles(edges[0], Resolution::Day).candles.empty());
  // An id the server has not got is missing, and the others of the batch are not harmed.
  EdgeKey gone = edges[0];
  gone.dataset = client->addDataset("NOSUCH");
  const auto mixed = provider->loadBatch(*client, *http, {edges[0], gone}, MarketServerHistory::Daily, [] { return true; });
  CHECK((mixed == std::vector<HistoryProvider::Outcome>{HistoryProvider::Outcome::Ok, HistoryProvider::Outcome::Missing}));
  // The same answers again: nothing new in the store.
  const uint64_t version = client->snapshot()->version();
  source.load(*client, *http, listener);
  CHECK(provider->loadBatch(*client, *http, edges, MarketServerHistory::Weekly, [] { return true; })[0] == HistoryProvider::Outcome::Ok);
  CHECK(client->snapshot()->version() == version);
}

#ifdef CPPHTTPLIB_ZLIB_SUPPORT
TEST(the_servers_answer_the_client_reads_is_compressed_on_the_wire_and_arrives_decoded) {
  Served served;
  const std::string target = "/summary?edge=EUR/USD/XXXX/HISTDATA&resolution=1d,1w";
  httplib::Client raw("127.0.0.1", served.server->port());
  raw.set_decompress(false);  // the bytes as they travel
  auto packed = raw.Get(target, {{"Accept-Encoding", "gzip"}});
  CHECK(packed && packed->status == 200 && packed->get_header_value("Content-Encoding") == "gzip");
  // The application's own client asks the same, and what it hands over is plain JSON.
  auto http = makeHttp();
  const HttpResult r = http->get(served.url() + target);
  CHECK(r.ok() && !r.body.empty() && r.body.front() == '{' && packed && r.body.size() > packed->body.size());
}
#endif
