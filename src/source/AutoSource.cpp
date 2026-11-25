#include "source/AutoSource.h"

#include <set>

namespace {

// Whether any edge has data from one of the datasets: what a source gave, read from the store and not from what it said it did.
bool hasData(const Snapshot & snapshot, const std::vector<std::string> & codes) {
  std::set<DatasetId> datasets;
  for (const std::string & code : codes)
    if (auto dataset = snapshot.findDataset(code)) datasets.insert(*dataset);
  if (datasets.empty()) return false;
  for (MarketId market : snapshot.edgeMarkets())
    for (const EdgeKey & edge : snapshot.edges(market))
      if (datasets.count(edge.dataset)) return true;
  return false;
}

// The history of whichever source is in use, which is not known when the program registers it: each call goes to the primary's provider
// until the primary has failed, then to the fallback's. A source with no provider has none: the dataset is empty, which no edge has.
class ChosenHistory : public HistoryProvider {
 public:
  ChosenHistory(std::shared_ptr<HistoryProvider> primary, std::shared_ptr<HistoryProvider> fallback, std::shared_ptr<const std::atomic<bool>> failed)
      : primary_(std::move(primary)), fallback_(std::move(fallback)), failed_(std::move(failed)) {}
  std::string dataset() const override { return in_use() ? in_use()->dataset() : std::string(); }
  std::vector<std::string> datasets() const override { return in_use() ? in_use()->datasets() : std::vector<std::string>{std::string()}; }
  std::optional<Need> need(const Snapshot & snapshot, const EdgeKey & edge, Period period) const override {
    return in_use() ? in_use()->need(snapshot, edge, period) : std::nullopt;
  }
  std::chrono::milliseconds lifetime(int request) const override { return in_use() ? in_use()->lifetime(request) : std::chrono::milliseconds(0); }
  std::optional<std::chrono::seconds> keep(Resolution r) const override { return in_use() ? in_use()->keep(r) : defaultKeep(r); }
  Outcome load(Store & store, Http & http, const EdgeKey & edge, int request, const Wait & wait) override {
    auto provider = in_use();
    return provider ? provider->load(store, http, edge, request, wait) : Outcome::Missing;
  }
  std::chrono::milliseconds pause() const override { return in_use() ? in_use()->pause() : std::chrono::milliseconds(0); }

 private:
  const std::shared_ptr<HistoryProvider> & in_use() const { return *failed_ ? fallback_ : primary_; }
  std::shared_ptr<HistoryProvider> primary_, fallback_;
  std::shared_ptr<const std::atomic<bool>> failed_;
};

}  // namespace

AutoSource::AutoSource(std::unique_ptr<Source> primary, std::unique_ptr<Source> fallback)
    : primary_(std::move(primary)), fallback_(std::move(fallback)) {
  auto first = primary_->historyProvider(), second = fallback_->historyProvider();
  if (first || second) history_ = std::make_shared<ChosenHistory>(std::move(first), std::move(second), failed_);
}

void AutoSource::load(Store & store, Http & http, SourceListener & listener) {
  if (decided_) {
    chosen()->load(store, http, listener);
    return;
  }
  primary_->load(store, http, listener);
  if (listener.cancelled()) return;  // stopped, not failed: decide at the next load
  decided_ = true;
  // Only what the primary put in the store counts, not other sources loading beside it.
  if (hasData(*store.snapshot(), primary_->datasetCodes())) return;
  *failed_ = true;
  listener.message(primary_->name() + " gave nothing: using " + fallback_->name());
  fallback_->load(store, http, listener);
}
