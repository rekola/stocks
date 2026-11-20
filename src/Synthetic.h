#pragma once

#include "model/Store.h"
#include "source/Source.h"

// Fills the store with made-up daily candles for the instruments of the presets, one market each, all from the dataset
// "synthetic", the same every run.
void fillSyntheticData(Store & store);

// The same as a source, for runs without a network.
class SyntheticSource : public Source {
 public:
  std::string name() const override { return "synthetic"; }
  std::string description() const override { return "Made-up prices, the same every run"; }
  void load(Store & store, Http &, SourceListener & listener) override {
    fillSyntheticData(store);
    listener.changed();
  }
};
