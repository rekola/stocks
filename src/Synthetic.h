#pragma once

#include "model/Store.h"
#include "source/Source.h"
#include "source/Universe.h"

// Fills the store with made-up daily candles for the instruments of `universe`, all from the dataset "synthetic",
// the same every run.
void fillSyntheticData(Store & store, const Universe & universe);

// The same as a source, for runs without a network.
class SyntheticSource : public Source {
 public:
  explicit SyntheticSource(Universe universe) : universe_(std::move(universe)) {}
  std::string name() const override { return "synthetic"; }
  std::string description() const override { return "Made-up prices, the same every run"; }
  void load(Store & store, Http &, SourceListener & listener) override {
    fillSyntheticData(store, universe_);
    listener.changed();
  }

 private:
  Universe universe_;
};
