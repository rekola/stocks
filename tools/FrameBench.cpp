// Measures what a frame of the browser costs on a window-like test display: scrolling by a key, and a store change.
#include "Browser.h"
#include "model/Dates.h"
#include "model/Isin.h"
#include "source/Source.h"

#include <cellui/Application.h>
#include <cellui/TestBackend.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

using Clock = std::chrono::steady_clock;
// A valid ISIN for the row: ZZ (private use), the number as nine digits and the check digit.
static std::string isinOf(int n) {
  const std::string digits = std::to_string(n);
  const std::string first11 = "ZZ" + std::string(9 - digits.size(), '0') + digits;
  return first11 + isinCheckDigit(first11);
}
static double since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

int main(int argc, char ** argv) {
  const int rows = argc > 1 ? std::atoi(argv[1]) : 30000;
  std::shared_ptr<Store> store = makeMemoryStore();
  Instrument usd = store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  const MarketId market = store->addMarket("Nasdaq Helsinki");
  const DatasetId dataset = store->addDataset("nordnet");
  const int64_t today = dayNumber(2026, 12, 1);
  std::mt19937 rng(1);
  std::uniform_real_distribution<double> step(-0.02, 0.021);
  for (int i = 0; i < rows; i++) {
    Instrument inst = store->addInstrument({{}, "", "Stock " + std::to_string(i), InstrumentKind::Company, isinOf(i)});
    EdgeKey k{inst.id, usd.id, market, dataset};
    double p = 50;
    std::vector<Candle> bars;
    for (int j = 60; j >= 0; j--) bars.push_back({today - j, p, p, p, p *= 1.0 + step(rng), 1000});
    store->appendCandles(k, Resolution::Day, bars);
  }
  cellui::DisplayInfo d;
  d.pictures = true, d.cell_px_height = 20, d.cell_px_width = 10, d.smooth_scroll = true, d.proportional_text = true, d.hdr_pictures = true;
  auto owned = std::make_unique<cellui::test::TestBackend>(50, 200, d);
  auto * backend = owned.get();
  auto app = cellui::Application::adopt(std::move(owned));
  Browser browser(*app, store);
  browser.setMinRefreshInterval(0);
  app->execute("show-all");
  app->present();
  auto t = Clock::now();
  for (int i = 0; i < 20; i++) {
    app->handle(cellui::BackendEvent::makeInput(cellui::keyInput(cellui::Key::PageDown, 0, cellui::Input::Kind::PRESS)));
    app->present();
  }
  std::printf("PageDown + present: %.2f ms each\n", since(t) / 20);
  t = Clock::now();
  for (int i = 0; i < 20; i++) app->present();
  std::printf("present again: %.2f ms each\n", since(t) / 20);
  EdgeKey any{};
  for (int round = 0; round < 3; round++) {
    // a source adds a bar to one row, then the UI hears of it
    auto edges = store->snapshot()->edges(market);
    store->appendCandles(edges[round], Resolution::Day, {{today + 1 + round, 1, 1, 1, 50, 1}});
    t = Clock::now();
    app->handle(cellui::BackendEvent::makeInput(cellui::keyInput('x', 0, cellui::Input::Kind::PRESS)));  // warm
    t = Clock::now();
    browser.table().postToRoot(StoreChanged{});
    const double post = since(t);
    t = Clock::now();
    app->present();
    std::printf("store change: post %.2f ms, present %.2f ms\n", post, since(t));
  }
  (void)backend;
}
