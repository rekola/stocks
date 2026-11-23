#pragma once

#include "source/Source.h"

#include <memory>

// One data source at a time, chosen by trying: the primary (Nordnet) loads, and if it gave the store nothing (no network, a
// refusal, the endpoints changed) the fallback (Yahoo) loads instead, and from then on the session stays with it. A primary
// that gave something stays the source, whatever it does later: a refusal in the middle of a run is not a reason to
// start another provider's data beside the first's.
class AutoSource : public Source {
 public:
  AutoSource(std::unique_ptr<Source> primary, std::unique_ptr<Source> fallback)
      : primary_(std::move(primary)), fallback_(std::move(fallback)) {}

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

  // The source in use: the primary until it has failed.
  const Source & chosen_source() const { return *chosen(); }

 private:
  Source * chosen() const { return failed_ ? fallback_.get() : primary_.get(); }
  std::unique_ptr<Source> primary_, fallback_;
  bool decided_ = false;  // the primary has loaded once, and given something or not
  bool failed_ = false;   // it gave nothing: the fallback is the source
};
