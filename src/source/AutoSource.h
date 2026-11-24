#pragma once

#include "source/Source.h"

#include <atomic>
#include <memory>

// One data source at a time, chosen by trying: the primary loads, and if it put no data in the store (no network, a
// refusal, the endpoints changed) the fallback loads instead, and from then on the session stays with it. A primary
// that gave something stays the source, whatever it does later: a refusal in the middle of a run is not a reason to
// start another provider's data beside the first's.
class AutoSource : public Source {
 public:
  AutoSource(std::unique_ptr<Source> primary, std::unique_ptr<Source> fallback);

  std::string name() const override { return "auto"; }
  std::vector<std::string> datasetCodes() const override {
    std::vector<std::string> codes = primary_->datasetCodes();
    for (std::string & code : fallback_->datasetCodes()) codes.push_back(std::move(code));
    return codes;
  }
  std::string description() const override {
    return primary_->name() + ", and " + fallback_->name() + " if " + primary_->name() + " gives nothing";
  }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return chosen()->refreshInterval(); }
  bool loadDetails(Store & store, Http & http, SourceListener & listener) override {
    return decided_ && chosen()->loadDetails(store, http, listener);
  }
  std::chrono::milliseconds detailsInterval() const override { return chosen()->detailsInterval(); }

  // The chosen source's: the program asks for it once, before the choice is made, so this is one provider that hands every call to the
  // primary's until the primary has failed, and then to the fallback's (none: it has no history of its own, or loads it with the rest).
  std::shared_ptr<HistoryProvider> historyProvider() override { return history_; }

  // The source in use: the primary until it has failed.
  const Source & chosen_source() const { return *chosen(); }

 private:
  Source * chosen() const { return *failed_ ? fallback_.get() : primary_.get(); }
  std::unique_ptr<Source> primary_, fallback_;
  bool decided_ = false;  // the primary has loaded once, and given something or not
  // It gave nothing: the fallback is the source. Shared with the history provider, which the loader's thread reads it through and
  // which outlives the sources.
  std::shared_ptr<std::atomic<bool>> failed_ = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<HistoryProvider> history_;  // null if neither has one
};
