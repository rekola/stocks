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

}  // namespace

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
  failed_ = true;
  listener.message(primary_->name() + " gave nothing: using " + fallback_->name());
  fallback_->load(store, http, listener);
}
