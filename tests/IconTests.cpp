#include "TestFramework.h"

#include "Browser.h"
#include "FakeHttp.h"
#include "Quotes.h"
#include "icons/IconLoader.h"

#include <cellui/Application.h>
#include <cellui/TestBackend.h>

#include <atomic>
#include <filesystem>

using namespace cellui;
namespace fs = std::filesystem;

namespace {

// A real 4x4 PNG.
const unsigned char kPng[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,4,0,0,0,4,8,6,0,0,0,169,241,158,126,0,0,0,18,73,68,65,84,120,156,99,56,33,39,247,31,25,51,144,46,0,0,65,184,32,49,251,53,123,102,0,0,0,0,73,69,78,68,174,66,96,130};
const std::string kPngBody(reinterpret_cast<const char *>(kPng), sizeof kPng);

struct Rig {
  FakeHttp * http;
  std::atomic<int> loaded{0};
  std::unique_ptr<IconLoader> loader;
  explicit Rig(IconLoader::Options options = {}, std::function<HttpResult(const std::string &)> answer = nullptr) {
    auto fake = std::make_unique<FakeHttp>();
    fake->answer = answer ? answer : [](const std::string &) { return ok(kPngBody); };
    http = fake.get();
    options.burst_pause = std::chrono::milliseconds(0);
    loader = std::make_unique<IconLoader>(std::move(fake), options, [this] { loaded++; });
  }
  // Requests are read after the loader is idle, so the thread is not writing them.
  size_t requests() {
    loader->waitIdle();
    return http->urls.size();
  }
};

fs::path tempDir(const char * name) {
  fs::path p = fs::temp_directory_path() / (std::string("stocks-icons-test-") + name);
  fs::remove_all(p);
  return p;
}

}  // namespace

TEST(an_icon_is_fetched_decoded_and_then_found_in_memory) {
  Rig rig;
  CHECK(rig.loader->get("https://cdn/a.png") == nullptr);
  rig.loader->want({"https://cdn/a.png"});
  CHECK(rig.requests() == 1);
  auto icon = rig.loader->get("https://cdn/a.png");
  CHECK(icon && icon->width == kIconSize && icon->height == kIconSize && icon->pixels && icon->pixels->size() == size_t(kIconSize * kIconSize));
  CHECK(rig.loaded == 1);
}

TEST(one_url_wanted_by_many_instruments_is_requested_once) {
  Rig rig;
  rig.loader->want({"https://cdn/brand.png", "https://cdn/brand.png", "https://cdn/brand.png"});
  CHECK(rig.requests() == 1);
  rig.loader->want({"https://cdn/brand.png"});  // and again at the next refresh
  CHECK(rig.requests() == 1);
}

TEST(a_404_and_a_body_that_is_no_image_are_remembered_and_not_asked_again) {
  Rig rig({}, [](const std::string & url) { return url.find("gone") != std::string::npos ? failed(404) : ok("<html>not an image</html>"); });
  rig.loader->want({"https://cdn/gone.png", "https://cdn/text.png"});
  CHECK(rig.requests() == 2);
  rig.loader->want({"https://cdn/gone.png", "https://cdn/text.png"});
  CHECK(rig.requests() == 2);
  CHECK(rig.loader->get("https://cdn/gone.png") == nullptr && rig.loader->get("https://cdn/text.png") == nullptr);
  CHECK(rig.loaded == 0);
}

TEST(a_429_stops_the_queue_and_later_requests) {
  Rig rig({}, [](const std::string &) { return failed(429); });
  rig.loader->want({"https://cdn/a.png", "https://cdn/b.png", "https://cdn/c.png"});
  CHECK(rig.requests() == 1);
  CHECK(rig.loader->refused());
  rig.loader->want({"https://cdn/d.png"});
  CHECK(rig.requests() == 1);
  Rig forbidden({}, [](const std::string &) { return failed(403); });
  forbidden.loader->want({"https://cdn/a.png", "https://cdn/b.png"});
  CHECK(forbidden.requests() == 1 && forbidden.loader->refused());
}

TEST(no_network_pauses_requests_without_remembering_a_failure) {
  Rig rig({}, [](const std::string &) { return failed(0); });
  rig.loader->want({"https://cdn/a.png", "https://cdn/b.png"});
  CHECK(rig.requests() == 1);  // the second waits out the pause
  CHECK(!rig.loader->refused());
}

TEST(the_second_run_draws_from_the_disk_without_the_network) {
  const fs::path dir = tempDir("second-run");
  {
    Rig first({.cache_dir = dir});
    first.loader->want({"https://cdn/a.png"});
    CHECK(first.requests() == 1);
  }
  Rig second({.cache_dir = dir}, [](const std::string &) { return failed(0); });
  second.loader->want({"https://cdn/a.png"});
  CHECK(second.requests() == 0);
  CHECK(second.loader->get("https://cdn/a.png") != nullptr);
  fs::remove_all(dir);
}

TEST(memory_keeps_the_most_recently_used_icons) {
  Rig rig({.max_memory_icons = 2});
  rig.loader->want({"https://cdn/a.png"});
  rig.requests();
  rig.loader->want({"https://cdn/b.png"});
  rig.requests();
  CHECK(rig.loader->get("https://cdn/a.png"));  // a is used again, so b is the oldest
  rig.loader->want({"https://cdn/c.png"});
  rig.requests();
  CHECK(rig.loader->get("https://cdn/a.png") && rig.loader->get("https://cdn/c.png") && !rig.loader->get("https://cdn/b.png"));
}

TEST(the_disk_cache_is_kept_under_its_cap_by_dropping_the_oldest) {
  const fs::path dir = tempDir("cap");
  const uintmax_t cap = 10 * 4100;
  Rig rig({.cache_dir = dir, .max_disk_bytes = cap});
  std::vector<std::string> urls;
  for (int i = 0; i < 51; i++) urls.push_back("https://cdn/" + std::to_string(i) + ".png");
  rig.loader->want(urls);
  rig.requests();
  uintmax_t total = 0;
  int files = 0;
  for (const auto & e : fs::directory_iterator(dir)) total += e.file_size(), files++;
  CHECK(files > 0 && total <= cap);
  fs::remove_all(dir);
}

namespace {

// A browser on a display that shows pictures, over one market with two instruments of one brand and one with none.
struct IconFixture {
  test::TestBackend * backend;
  std::unique_ptr<Application> app;
  std::shared_ptr<Store> store = makeMemoryStore();
  std::unique_ptr<Rig> rig = std::make_unique<Rig>();
  std::unique_ptr<Browser> browser;
  IconFixture() {
    DisplayInfo display;
    display.pictures = true;
    display.cell_px_height = 16;
    display.cell_px_width = 8;
    auto owned = std::make_unique<test::TestBackend>(24, 100, display);
    backend = owned.get();
    app = Application::adopt(std::move(owned));
    Market market = store->addMarket({{}, "Commodities"});
    Instrument usd = store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
    Dataset dataset = store->addDataset({{}, "test"});
    for (const char * symbol : {"AAA", "BBB", "CCC"}) {
      Instrument i = store->addInstrument({{}, symbol, symbol, InstrumentKind::Company});
      if (std::string(symbol) != "CCC") store->setAttribute(i.id, {predicate::iconUrl, "https://cdn/brand.png"});
      store->appendCandles({i.id, usd.id, market.id, dataset.id}, Resolution::Day, {{0, 1, 1, 1, 1.0, 1}, {1000, 1, 1, 1, 2.0, 1}});
    }
    browser = std::make_unique<Browser>(*app, store, std::shared_ptr<IconLoader>(rig->loader.get(), [](IconLoader *) {}));
  }
};

}  // namespace

TEST(the_table_shows_an_icon_once_it_has_loaded_and_keeps_its_place) {
  IconFixture f;
  Table & table = f.browser->table();
  table.select(1);
  const int selected = table.selectedRow();
  f.app->present();
  const auto * quotes = static_cast<const Quotes *>(table.model().get());
  auto icons = [&] {
    int n = 0;
    for (int r = 0; r < quotes->rowCount(); r++) n += quotes->icon(r, Quotes::ICON) != nullptr;
    return n;
  };
  f.app->frame_started.emit(0.016);  // a frame: the rows on screen are wanted
  f.rig->loader->waitIdle();
  CHECK(f.rig->requests() == 1);     // the brand shared by two rows, once
  CHECK(icons() == 2);               // and the row of no brand has none
  f.app->widgets().postToRoot(IconsLoaded{});  // what the loader thread's post turns into
  f.app->present();
  CHECK(table.selectedRow() == selected);
  int shown = 0;  // pictures where the display has them: the two rows of the brand show the one picture
  for (const ImagePlacement & p : f.backend->images()) shown += p.shared.valid() && std::string(p.shared.kind) == "icon";
  CHECK(shown == 2);
}

TEST(requests_go_in_bursts_and_every_wanted_url_still_arrives) {
  Rig rig({.burst = 2, .burst_pause = std::chrono::milliseconds(1)});
  std::vector<std::string> urls;
  for (int i = 0; i < 11; i++) urls.push_back("https://cdn/" + std::to_string(i) + ".png");
  rig.loader->want(urls);
  CHECK(rig.requests() == 11);
  for (const std::string & url : urls) CHECK(rig.loader->get(url));
}
